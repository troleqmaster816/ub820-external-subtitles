/* UB820 1.82 in-process subtitle controller.  Workspace build only.
 * The supervisor verifies firmware bytes, relocates this PIC object, fills
 * the three original trampolines, then installs hooks.  No ELF constructors.
 */
#include "protocol.h"
#include <stddef.h>

extern int open(const char *, int, ...);
extern long read(int, void *, unsigned long);
extern long write(int, const void *, unsigned long);
extern int close(int);
extern long pwrite(int, const void *, unsigned long, long);
extern int flock(int, int);
extern int rename(const char *, const char *);
extern int unlink(const char *);
extern int usleep(unsigned int);
extern int getpid(void);
#if defined(UB_HOST_TEST) && !defined(__arm__)
#include <pthread.h>
#else
extern int pthread_create(unsigned long *, const void *, void *(*)(void *), void *);
extern int pthread_detach(unsigned long);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
#endif
extern int UFS_OpenDir(const char *, uint32_t *);
extern int UFS_ReadDir(uint32_t, void *, uint32_t *);
extern int UFS_CloseDir(uint32_t);

#ifndef UB_PLAY_DAT
#define UB_PLAY_DAT "/tmp/apl_play3dlib_play_dat.dat"
#define UB_PLAY_LOCK "/tmp/apl_play3dlib_3ddynamic_lock"
#endif

int (*ub_original_open)(const char *);
int (*ub_original_draw)(uint32_t);
int (*ub_original_close)(void);

struct ub_api {
 int (*start)(const char *);
 void (*dirty)(uint32_t);
 void (*seek)(uint32_t);
 int (*update)(uint32_t, uint32_t);
 int (*set_attr)(const int32_t *);
 int (*set_display)(uint32_t);
 volatile uint32_t *active, *parser, *display, *clock;
 volatile int32_t *attrs;
 volatile unsigned char *loaded;
 void *mutex;
 const char *native_path;
};

#ifndef UB_HOST_TEST
static struct ub_api api = {
 (int (*)(const char *))0x1de9d,
 (void (*)(uint32_t))0x1d7a9,
 (void (*)(uint32_t))0x1d731,
 (int (*)(uint32_t,uint32_t))0x20135,
 (int (*)(const int32_t *))0x1d2ad,
 (int (*)(uint32_t))0x1d4e1,
 (volatile uint32_t *)0x45728, (volatile uint32_t *)0x459fc,
 (volatile uint32_t *)0x45e30, (volatile uint32_t *)0x4440c,
 (volatile int32_t *)0x4623c, (volatile unsigned char *)0x434e8,
 (void *)0x45710, (const char *)0x45e38
};
#else
static struct ub_api api;
#endif

static struct ub_status state;
static volatile uint32_t gate, started, closing, lifecycle, session;
static uint32_t initialized, last_seq, preferred_enabled, saved_attrs_valid, status_dirty, explicit_selection;
static int32_t saved_attrs[5];
static char current_path[1024]; /* Always readable1024 bytes for native memcpy. */
static char candidate_path[1024], rollback_path[1024];
static uint32_t atomic_read(volatile uint32_t *p) {return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
static void atomic_write(volatile uint32_t *p,uint32_t v) {__atomic_store_n(p,v,__ATOMIC_RELEASE);}

static void zero(void *v, unsigned long n) {
 unsigned char *p=(unsigned char *)v; while(n--) *p++=0;
}
static unsigned long length(const char *p, unsigned long cap) {
 unsigned long n=0; while(n<cap && p[n]) n++; return n;
}
static void copy(char *d, const char *s, unsigned long cap) {
 unsigned long i=0; if(!cap)return;
 while(i+1<cap && s[i]) {d[i]=s[i];i++;} d[i]=0;
}
static int equal(const char *a,const char *b) {
 while(*a && *a==*b) {a++;b++;} return *a==*b;
}
static unsigned char lower(unsigned char c) {
 return c>='A'&&c<='Z' ? (unsigned char)(c+32):c;
}
static int name_compare(const char *a,const char *b) {
 while(*a && lower((unsigned char)*a)==lower((unsigned char)*b)){a++;b++;}
 return (int)lower((unsigned char)*a)-(int)lower((unsigned char)*b);
}
static int srt_name(const char *s) {
 unsigned long n=length(s,UB_PATH_BYTES);
 return n>4 && n<UB_PATH_BYTES && s[n-4]=='.' && lower(s[n-3])=='s' &&
        lower(s[n-2])=='r' && lower(s[n-1])=='t';
}
static int valid_name(const char *s, unsigned long cap) {
 unsigned long i,n=length(s,cap);
 if(!n || n==cap || equal(s,".") || equal(s,".."))return 0;
 for(i=0;i<n;i++)if((unsigned char)s[i]<32 || (unsigned char)s[i]==127 || s[i]=='/' || s[i]=='\\')return 0;
 return 1;
}
static int valid_path(const char *s) {
 static const char root[]="/mnt/usb";
 unsigned long i,n=length(s,UB_PATH_BYTES),begin=1;
 if(n<8 || n>UB_PATH_LIMIT)return 0;
 for(i=0;i<8;i++)if(s[i]!=root[i])return 0;
 if(n>8 && s[8]!='/')return 0;
 if(n>8 && s[n-1]=='/')return 0;
 for(i=0;i<=n;i++) {
  unsigned char c=(unsigned char)s[i];
  if(i<n && (c<32 || c==127 || c=='\\'))return 0;
  if(i>0 && (c=='/' || c==0)) {
   unsigned long size=i-begin;
   if(!size || (size==1 && s[begin]=='.') || (size==2 && s[begin]=='.' && s[begin+1]=='.'))return 0;
   begin=i+1;
  }
 }
 return 1;
}
static int join(char *out,const char *dir,const char *name) {
 unsigned long a=length(dir,UB_PATH_BYTES),b=length(name,UB_PATH_BYTES),i;
 if(a+b+1>UB_PATH_LIMIT)return 0;
 copy(out,dir,UB_PATH_BYTES);out[a++]='/';
 for(i=0;i<=b;i++)out[a+i]=name[i];
 return valid_path(out);
}
static void gate_lock(void) {
 while(!__sync_bool_compare_and_swap(&gate,0,1))usleep(1000);
 __sync_synchronize();
}
static void gate_unlock(void) {__sync_synchronize();__sync_lock_release(&gate);}
static void engine_lock(void) {pthread_mutex_lock(api.mutex);}
static void engine_unlock(void) {pthread_mutex_unlock(api.mutex);}

static void initialize(void) {
 if(initialized)return;
 zero(&state,sizeof(state));state.magic=UB_MAGIC;state.version=UB_VERSION;
#ifdef UB_HOST_TEST
 state.epoch=0x1234;
#else
 state.epoch=(uint32_t)getpid() ^ (uint32_t)(uintptr_t)&state;
 if(!state.epoch)state.epoch=1;
#endif
 copy(state.dir,"/mnt/usb/sub",sizeof(state.dir));
 copy(state.message,"Choose a subtitle file",sizeof(state.message));
 initialized=1;
}
static void snapshot(void) {
 uint32_t i;
 engine_lock();
 state.active=(*api.active==1 && *api.parser!=0);
 state.enabled=state.active && *api.display!=0;
 if(state.active) {
  preferred_enabled=state.enabled;
  for(i=0;i<5;i++)saved_attrs[i]=api.attrs[i];
  saved_attrs[0]=0;saved_attrs_valid=1;
  if(!current_path[0] && api.native_path && valid_path(api.native_path))
   copy(current_path,api.native_path,sizeof(current_path));
 }
 engine_unlock();
 copy(state.current,current_path,sizeof(state.current));
}
static int finish(int result,const char *message) {
 state.result=result;copy(state.message,message,sizeof(state.message));snapshot();return result;
}
static void sort_items(void) {
 uint32_t i,j;struct ub_item temp;
 for(i=1;i<state.count;i++)for(j=i;j>0;j--) {
  struct ub_item *a=&state.items[j-1],*b=&state.items[j];
  if(a->kind>b->kind || (a->kind==b->kind && name_compare(a->name,b->name)<=0))break;
  temp=*a;*a=*b;*b=temp;
 }
}
static int catalogue(const char *path) {
 uint32_t handle=0,result=0,n=0,limited=0;int rc=0;
 unsigned char record[0x120];char full[UB_PATH_BYTES],parent[UB_PATH_BYTES];
 if(!valid_path(path))return finish(UB_BAD_PATH,"Invalid USB directory");
 zero(state.items,sizeof(state.items));state.count=0;
 copy(state.dir,path,sizeof(state.dir));
 if(UFS_OpenDir(path,&handle)!=0)return finish(UB_IO_ERROR,"Cannot open USB directory");
 if(!equal(path,"/mnt/usb")) {
  unsigned long k=length(path,UB_PATH_BYTES);
  copy(parent,path,sizeof(parent));while(k>8 && parent[k-1]!='/')k--;
  parent[k>8?k-1:8]=0;
  state.items[0].kind=UB_KIND_DIR;copy(state.items[0].name,"..",UB_NAME_BYTES);
  copy(state.items[0].path,parent,UB_PATH_BYTES);state.count=1;
 }
 for(n=0;n<8192;n++) {
  char *name=(char *)record+11;uint32_t kind;
  zero(record,sizeof(record));result=0;
  rc=UFS_ReadDir(handle,record,&result);
  if(rc || !result)break;
  if(!valid_name(name,sizeof(record)-11))continue;
  kind=record[10]==4?UB_KIND_DIR:(record[10]==8 && srt_name(name)?UB_KIND_FILE:0);
  if(!kind)continue;
  if(!join(full,path,name)){limited=1;continue;}
  if(state.count==UB_MAX_ITEMS){limited=1;continue;}
  state.items[state.count].kind=kind;
  copy(state.items[state.count].name,name,UB_NAME_BYTES);
  copy(state.items[state.count].path,full,UB_PATH_BYTES);state.count++;
 }
 if(UFS_CloseDir(handle)!=0)rc=1;
 if(rc){zero(state.items,sizeof(state.items));state.count=0;return finish(UB_IO_ERROR,"USB directory read failed");}
 if(n==8192)limited=1;
 sort_items();
 return finish(limited?UB_LIMIT:UB_OK,limited?"Directory limited to64 entries; shorten long paths":"Choose a subtitle file");
}
static int listed_file(const char *path) {
 uint32_t i;for(i=0;i<state.count;i++)if(state.items[i].kind==UB_KIND_FILE && equal(path,state.items[i].path))return 1;
 return 0;
}

static int replace_file(const char *path,int require_listed) {
 uint32_t epoch=atomic_read(&lifecycle),display,i,time;int rc,rollback_rc;
 if(!valid_path(path) || !srt_name(path) || (require_listed && !listed_file(path)))return finish(UB_BAD_PATH,"Choose a listed SRT file");
 if(!atomic_read(&session) || atomic_read(&closing))return finish(UB_NO_ENGINE,"Playback subtitle session is closed");
 zero(candidate_path,sizeof(candidate_path));copy(candidate_path,path,sizeof(candidate_path));
 zero(rollback_path,sizeof(rollback_path));copy(rollback_path,current_path,sizeof(rollback_path));
 engine_lock();
 if(!atomic_read(&session) || atomic_read(&closing) || epoch!=atomic_read(&lifecycle)){engine_unlock();return finish(UB_STALE,"Playback changed; try again");}
 if(*api.active!=1 || !*api.parser) {
  /* Public Open owns the same mutex: call only after releasing it. The
   * controller gate prevents Close/Open hooks interleaving this transition. */
  display=preferred_enabled;
  engine_unlock();
  if(!ub_original_open)return finish(UB_NO_ENGINE,"Native Open trampoline missing");
  rc=ub_original_open(candidate_path);
  if(rc)return finish(UB_PARSE_ERROR,"Cannot load selected subtitle");
  if(saved_attrs_valid && api.set_attr(saved_attrs)!=0)return finish(UB_PARSE_ERROR,"Subtitle style restore failed");
  if(api.set_display(display?0:1)!=0)return finish(UB_PARSE_ERROR,"Subtitle display restore failed");
  engine_lock();
  if(!atomic_read(&session) || atomic_read(&closing) || epoch!=atomic_read(&lifecycle)){engine_unlock();return finish(UB_STALE,"Playback changed; subtitle not published");}
  if(*api.active!=1 || !*api.parser){engine_unlock();return finish(UB_NO_ENGINE,"Native subtitle engine did not initialize");}
  *api.loaded=1;
 } else {
  display=*api.display!=0;
  for(i=0;i<5;i++)saved_attrs[i]=api.attrs[i];
  saved_attrs[0]=0;saved_attrs_valid=1;
  preferred_enabled=display;
  rc=api.start(candidate_path);
  if(rc) {
   rollback_rc=rollback_path[0]?api.start(rollback_path):-1;
   if(rollback_rc==0){api.dirty(1);api.seek(1);api.update(display,*api.clock);}
   else {api.update(0,0);}
   engine_unlock();
   return finish(UB_PARSE_ERROR,rollback_rc==0?"Load failed; previous subtitle restored":"Load failed; choose another subtitle");
  }
 }
 if(!atomic_read(&session) || atomic_read(&closing) || epoch!=atomic_read(&lifecycle)){engine_unlock();return finish(UB_STALE,"Playback changed; subtitle not published");}
 /* Parsing has committed the new native file even if graphics subsequently
  * reports an error. Keep the visible selection truthful in that case. */
 copy(current_path,candidate_path,sizeof(current_path));
 if(require_listed)explicit_selection=1;
 api.dirty(1);api.seek(1);time=*api.clock;
 rc=api.update(display,time);
 if(rc>1){engine_unlock();return finish(UB_PARSE_ERROR,"Subtitle loaded; display update failed");}
 if(!atomic_read(&session) || atomic_read(&closing) || epoch!=atomic_read(&lifecycle)){engine_unlock();return finish(UB_STALE,"Playback changed; try again");}
 engine_unlock();
 return finish(UB_OK,display?"Subtitle selected":"Subtitle selected; display remains off");
}
/* The vendor also stores the external-caption toggle in byte5 of this
 * eight-byte record. Hold its own lock and write only the changed byte;
 * positions, sync, charset, font and reserved bytes cannot be clobbered. */
static int playdat_begin(int *lock_fd,int *data_fd) {
 unsigned char record[8];unsigned long done=0;long n;int lf,fd;
 *lock_fd=*data_fd=-1;
 lf=open(UB_PLAY_LOCK,2|0x40|0x800|0x20000,0666);
 if(lf<0)return -1;
 if(flock(lf,2|4)!=0){close(lf);return -1;}
 fd=open(UB_PLAY_DAT,2|0x800|0x20000);
 if(fd<0){flock(lf,8);close(lf);return -1;}
 while(done<sizeof(record)) {
  n=read(fd,record+done,sizeof(record)-done);
  if(n<=0){close(fd);flock(lf,8);close(lf);return -1;}
  done+=(unsigned long)n;
 }
 *lock_fd=lf;*data_fd=fd;return 0;
}
static void playdat_end(int lock_fd,int data_fd) {
 if(data_fd>=0)close(data_fd);
 if(lock_fd>=0){flock(lock_fd,8);close(lock_fd);}
}
static void restore_display(uint32_t enabled) {
 api.set_display(enabled?0:1);
 if(enabled){engine_lock();if(*api.active==1 && *api.parser){api.seek(1);api.update(1,*api.clock);}engine_unlock();}
}
static int display_set(uint32_t enabled) {
 uint32_t epoch=atomic_read(&lifecycle),previous;int rc,lf,fd;unsigned char byte;
 if(!atomic_read(&session) || atomic_read(&closing))return finish(UB_NO_ENGINE,"Playback subtitle session is closed");
 engine_lock();
 if(*api.active!=1 || !*api.parser){engine_unlock();return finish(UB_NO_ENGINE,"Select a subtitle first");}
 previous=*api.display!=0;
 engine_unlock();
 if(playdat_begin(&lf,&fd)!=0)return finish(UB_IO_ERROR,"Cannot lock/read subtitle settings; display unchanged");
 rc=api.set_display(enabled?0:1);
 if(rc){playdat_end(lf,fd);return finish(UB_PARSE_ERROR,"Native display request failed");}
 if(enabled) {
  engine_lock();
  if(*api.active==1 && *api.parser && epoch==atomic_read(&lifecycle) && atomic_read(&session) && !atomic_read(&closing)) {
   api.seek(1);rc=api.update(1,*api.clock);
  } else rc=3;
  engine_unlock();
  if(rc>1){restore_display(previous);playdat_end(lf,fd);return finish(UB_STALE,"Playback changed; try again");}
 }
 if(epoch!=atomic_read(&lifecycle) || !atomic_read(&session) || atomic_read(&closing)) {
  restore_display(previous);playdat_end(lf,fd);return finish(UB_STALE,"Playback changed; display restored");
 }
 byte=enabled?1:0;
 if(pwrite(fd,&byte,1,5)!=1) {
  restore_display(previous);playdat_end(lf,fd);return finish(UB_IO_ERROR,"Cannot save subtitle toggle; display restored");
 }
 playdat_end(lf,fd);preferred_enabled=enabled;
 return finish(UB_OK,enabled?"Subtitles on":"Subtitles off");
}

static int process(const struct ub_request *r) {
 if(r->magic!=UB_MAGIC || r->version!=UB_VERSION || !r->seq ||
    r->op<UB_OP_LIST || r->op>UB_OP_RELOAD || length(r->path,UB_PATH_BYTES)==UB_PATH_BYTES)
  return finish(UB_BAD_PATH,"Invalid menu request");
 if((int32_t)(r->seq-last_seq)<=0)return UB_STALE;
 if(r->epoch!=state.epoch && !(r->op==UB_OP_LIST && r->epoch==0)) {
  state.seq=r->seq;last_seq=r->seq;return finish(UB_STALE,"Player restarted; refresh the menu");
 }
 state.seq=r->seq;last_seq=r->seq;
 switch(r->op) {
 case UB_OP_LIST:return catalogue(r->path[0]?r->path:state.dir);
 case UB_OP_SELECT:return replace_file(r->path,1);
 case UB_OP_RELOAD:
  if(r->path[0])return finish(UB_BAD_PATH,"Reload does not accept a new path");
  if(!current_path[0])return finish(UB_NO_ENGINE,"No selected subtitle to reload");
  return replace_file(current_path,0);
 case UB_OP_OFF:return display_set(0);
 case UB_OP_ON:return display_set(1);
 }
 return finish(UB_BAD_PATH,"Unknown menu request");
}

static int write_status(void) {
 const char *tmp=UB_STATUS_FILE ".tmp";int fd;unsigned long done=0;long n;
 fd=open(tmp,1|0x40|0x200|0x800|0x20000,0600);
 if(fd<0)return -1;
 while(done<sizeof(state)) {
  n=write(fd,(const char *)&state+done,sizeof(state)-done);
  if(n<=0){close(fd);unlink(tmp);return -1;}done+=(unsigned long)n;
 }
 if(close(fd)!=0){unlink(tmp);return -1;}
 if(rename(tmp,UB_STATUS_FILE)!=0){unlink(tmp);return -1;}return 0;
}
static int request_read(struct ub_request *r) {
 int fd;unsigned long done=0;long n;unsigned char extra;
 fd=open(UB_REQUEST_FILE,0x800|0x20000);
 if(fd<0)return 0;
 while(done<sizeof(*r)) {
  n=read(fd,(char *)r+done,sizeof(*r)-done);
  if(n<=0)break;
  done+=(unsigned long)n;
 }
 n=read(fd,&extra,1);close(fd);
 return done==sizeof(*r) && n==0;
}
static void *worker(void *unused) {
 struct ub_request request;uint32_t tick=0;unsigned long old_signature; (void)unused;
 gate_lock();initialize();
 if(catalogue("/mnt/usb/sub")==UB_IO_ERROR)catalogue("/mnt/usb");
 snapshot();write_status();gate_unlock();
 for(;;) {
  usleep(200000);gate_lock();
  if(request_read(&request) && (int32_t)(request.seq-last_seq)>0) {
   process(&request);write_status();
  }
  if(status_dirty){snapshot();write_status();status_dirty=0;}
  if(++tick==5) {
   tick=0;old_signature=state.active*2+state.enabled;snapshot();
   if(old_signature!=state.active*2+state.enabled)write_status();
  }
  gate_unlock();
 }
 return (void *)0;
}
static void bootstrap(void) {
#ifndef UB_HOST_TEST
 unsigned long tid;
 if(__sync_bool_compare_and_swap(&started,0,1)) {
  if(pthread_create(&tid,(const void *)0,worker,(void *)0)==0){pthread_detach(tid);atomic_write(&started,2);}
  else atomic_write(&started,0);
 }
#endif
}

int ub_hook_open(const char *path) {
 int rc;const char *chosen;
 if(!ub_original_open)return 3;
 gate_lock();initialize();atomic_write(&session,1);__sync_add_and_fetch(&lifecycle,1);
 if(++state.epoch==0)state.epoch=1;
 status_dirty=1;
 chosen=explicit_selection && valid_path(current_path)?current_path:path;
 rc=ub_original_open(chosen);
 if(rc==0 && chosen!=current_path && length(chosen,1024)<=UB_PATH_LIMIT && valid_path(chosen)) {
  zero(current_path,sizeof(current_path));copy(current_path,chosen,sizeof(current_path));
 }
 gate_unlock();bootstrap();return rc;
}
int ub_hook_draw(uint32_t time) {
 int rc;
 gate_lock();initialize();
 if(*api.active==1 && !atomic_read(&closing))atomic_write(&session,1);
 rc=ub_original_draw?ub_original_draw(time):3;
 gate_unlock();bootstrap();return rc;
}
int ub_hook_close(void) {
 int rc;
 if(!ub_original_close)return 3;
 __sync_add_and_fetch(&closing,1);__sync_add_and_fetch(&lifecycle,1);atomic_write(&session,0);
 gate_lock();initialize();snapshot();
 if(++state.epoch==0)state.epoch=1;
 status_dirty=1;
 rc=ub_original_close(); /* Preserve original return, including native defects. */
 /* The native helper can return an uninitialized nonzero success value.
  * Clear the outer draw gate only when both authoritative engine indicators
  * confirm shutdown; retain it if Close leaves a live engine/parser. */
 engine_lock();if(*api.active==0 && *api.parser==0)*api.loaded=0;engine_unlock();
 gate_unlock();__sync_sub_and_fetch(&closing,1);return rc;
}

#ifdef UB_HOST_TEST
void ub_test_bind(const struct ub_api *a) {api=*a;}
void ub_test_reset(void) {
 zero(&state,sizeof(state));zero(current_path,sizeof(current_path));
 zero(candidate_path,sizeof(candidate_path));zero(rollback_path,sizeof(rollback_path));
 gate=started=closing=lifecycle=session=initialized=last_seq=preferred_enabled=saved_attrs_valid=status_dirty=explicit_selection=0;
 initialize();
}
int ub_test_process(const struct ub_request *r,struct ub_status *out) {
 int rc;gate_lock();rc=process(r);*out=state;gate_unlock();return rc;
}
int ub_test_valid_path(const char *path){return valid_path(path);}
void ub_test_refresh(struct ub_status *out){snapshot();*out=state;}
#endif
