/* Engine lifecycle through the real controller and the luminance fixture's
 * native callbacks: an opened engine is always visible to the firmware's
 * Draw/Close gate, a file change takes the old line down, the first Open
 * gets the stock play_dat attributes, and a session ends without Close. */
static char hint_path[160],selection_path[160];
#define UB_SUB_HINT_FILE hint_path
#define UB_SUB_SELECTION_FILE selection_path
#define main luminance_fixture_main
#include "subtitle_luminance_backend_test.c"
#undef main
#include <fcntl.h>
#include <sched.h>
#include <sys/file.h>

static unsigned attr_calls,start_seen_mapped,start_fail,session_end_calls;
static int32_t last_attr[5];static int attr_result,attr_ignore;
static int recording_attr(const int32_t *a){
 attr_calls++;memcpy(last_attr,a,sizeof last_attr);
 if(attr_ignore)return 0; /* stock: change undone, repaint reported */
 return attr_result?attr_result:set_attr(a);
}
static int failing_display(uint32_t value){(void)value;return 3;}
static int checked_start(const char *path){
 locked();start_calls++;if(mapped_value)start_seen_mapped++;
 mapped_value=0; /* stock esl_tst_Start clears the mapped flag without unmapping */
 return start_fail&&strstr(path,"bad")?7:0;
}
/* Stock FUN_00018a90: ESL_Close only while loaded. */
static int native_session_end(int notify){
 (void)notify;session_end_calls++;
 if(loaded_value&&ub_hook_close()==0)loaded_value=0;
 return 0x5e; /* passed through */
}
static int reload(const char *path){gate_lock();int rc=replace_file(path,0);gate_unlock();return rc;}
/* SELECT from the browser: the file must be in the current listing. */
static void listed(const char *path){state.count=1;state.items[0].kind=UB_KIND_FILE;copy(state.items[0].path,path,sizeof state.items[0].path);}
static int select_file(const char *path){gate_lock();int rc=replace_file(path,1);gate_unlock();return rc;}
static int playdat_byte(unsigned index,unsigned char *out){
 unsigned char b[8];int fd=open(UB_PLAY_DAT,O_RDONLY);ssize_t n;
 if(fd<0)return -1;
 n=read(fd,b,8);close(fd);if(n!=8)return -1;
 *out=b[index];return 0;
}
static pthread_barrier_t opening;
static int open_during_stop(const char *path){
 int rc=pthread_barrier_wait(&opening);assert(!rc||rc==PTHREAD_BARRIER_SERIAL_THREAD);
 while(!atomic_read(&closing))sched_yield();
 return native_open(path);
}
static void *select_during_stop(void *result){
 struct ub_request q=request(UB_OP_SELECT);struct ub_status s;
 copy(q.path,state.items[0].path,sizeof q.path);
 *(int *)result=ub_test_process(&q,&s);return 0;
}
static void playdat(const unsigned char b[8]){
 FILE *f=fopen(UB_PLAY_DAT,"wb");assert(f);assert(fwrite(b,1,8,f)==8);assert(!fclose(f));
}
/* Native Open that records its paths; a missing file is any path with "gone". */
static char opened[4][UB_SUB_PATH_BYTES];static unsigned opened_count;
static int recording_open(const char *path){
 if(opened_count<4)snprintf(opened[opened_count],sizeof opened[0],"%s",path);
 opened_count++;
 if(strstr(path,"gone")){assert(atomic_read(&gate));open_calls++;return 7;}
 return native_open(path);
}
static void write_hint(uint32_t flags,const char *autoload,int8_t sync,int8_t code){
 struct ub_sub_hint h;uint64_t born;assert(!ub_br_birth((unsigned)getpid(),&born));
 memset(&h,0,sizeof h);h.magic=UB_SUB_HINT_MAGIC;h.version=UB_SUB_RECORD_VERSION;h.pid=(uint32_t)getpid();
 h.birth_lo=(uint32_t)born;h.birth_hi=(uint32_t)(born>>32);h.flags=flags;
 if(autoload)snprintf(h.autoload,sizeof h.autoload,"%s",autoload);
 h.sync=sync;h.charcode=code;
 assert(!ub_br_private_write(UB_SUB_HINT_FILE,&h,sizeof h));
}
static void setup(void){
 struct ub_api a={0};
 a.active=&active_value;a.parser=&parser_value;a.display=&display_value;a.clock=&clock_value;
 a.attrs=attrs;a.loaded=&loaded_value;a.mutex=&mutex;a.start=checked_start;a.dirty=dirty;a.seek=seek;a.update=update;
 a.set_attr=recording_attr;a.set_display=set_display;a.set_foreground=foreground;a.ti_display=&ti_value;a.text_gc=&gc_value;
 ub_test_bind(&a);ub_test_reset();ub_original_open=native_open;ub_original_close=native_close;
 ub_precision_renderer=(struct ub_precision_render_api){.prepare=prepare,.render=render,.present=present,.unmap=unmap,.cue=cue_value,.mapped=&mapped_value};
 active_value=parser_value=display_value=mapped_value=0;loaded_value=0;ti_value=gc_value=0;memset(attrs,0,sizeof attrs);
 open_calls=close_calls=start_calls=attr_calls=start_seen_mapped=start_fail=session_end_calls=0;
 attr_result=attr_ignore=fg_failure=open_error=0;atomic_write(&session,1);unlink(UB_PLAY_DAT);
}
static void remembered_setup(uint32_t flags,const char *autoload,int8_t sync,int8_t code,const unsigned char record[8]){
 write_hint(flags,autoload,sync,code);setup();playdat(record);
 ub_original_open=recording_open;opened_count=0;memset(opened,0,sizeof opened);
}
int main(void){
 unsigned cases=0;char directory[]="/tmp/ub820-lifecycle-XXXXXX";assert(mkdtemp(directory));
 snprintf(play_path,sizeof play_path,"%s/play",directory);snprintf(lock_path,sizeof lock_path,"%s/lock",directory);
 snprintf(hint_path,sizeof hint_path,"%s/hint",directory);snprintf(selection_path,sizeof selection_path,"%s/selection",directory);
 /* 1. A restore error after a successful Open still leaves an engine the
  *    firmware draws and closes, and the selection names the loaded file. */
 for(int fault=0;fault<3;fault++){
  setup();
  if(fault==0){text_level=100;fg_failure=1;brightness.seed_ok=1;}
  if(fault==1){saved_attrs_valid=1;attr_result=3;}
  if(fault==2)api.set_display=failing_display;
  assert(reload("/mnt/usb/sub/a.srt")==UB_PARSE_ERROR);
  assert(open_calls==1&&active_value==1&&parser_value==1&&loaded_value==1&&!close_calls);
  assert(!strcmp(current_path,"/mnt/usb/sub/a.srt")&&strstr(state.message,"loaded"));
  fg_failure=attr_result=0;api.set_display=set_display;
  assert(reload("/mnt/usb/sub/a.srt")==UB_OK&&loaded_value==1&&open_calls==1);
  if(fault==0)assert(gc.foreground==0xff646464&&!brightness.fault&&state.brightness_available);
  ub_original_session_end=native_session_end;ub_hook_session_end(1);
  assert(session_end_calls==1&&close_calls==1&&!active_value&&!loaded_value&&!atomic_read(&session));cases++;
 }
 /* A success on a live engine that the firmware does not see yet (loaded=0,
  * the state the old restore errors left) makes it visible as well. */
 setup();gate_lock();assert(!native_open("/mnt/usb/sub/a.srt"));gate_unlock();open_calls=0;assert(!loaded_value);
 assert(reload("/mnt/usb/sub/b.srt")==UB_OK&&loaded_value==1&&start_calls==1&&!open_calls);cases++;
 assert(!ub_hook_close());
 /* 2. Switching files on a live engine unmaps the visible line under the
  *    engine mutex before stock Start; the rollback path inherits that. */
 setup();unsigned char record[8]={0xf8,0,3,2,1,1,0,0};playdat(record);
 assert(reload("/mnt/usb/sub/a.srt")==UB_OK&&loaded_value==1&&display_value==1&&mapped_value==1);
 assert(reload("/mnt/usb/sub/b.srt")==UB_OK&&start_calls==1&&!start_seen_mapped&&mapped_value==1);cases++;
 start_fail=1;assert(reload("/mnt/usb/sub/bad.srt")==UB_PARSE_ERROR);
 assert(start_calls==3&&!start_seen_mapped&&!strcmp(current_path,"/mnt/usb/sub/b.srt"));cases++;
 assert(!ub_hook_close());
 /* 3. The first engine opened here gets what stock applies after its own
  *    Open: {0, b4, -b0, b2, b3}, sign-extended, display from byte5. */
 setup();playdat(record);
 assert(reload("/mnt/usb/sub/a.srt")==UB_OK&&attr_calls==1);
 assert(last_attr[0]==0&&last_attr[1]==1&&last_attr[2]==8&&last_attr[3]==3&&last_attr[4]==2&&display_value==1);cases++;
 assert(!ub_hook_close());
 setup();record[0]=4;record[3]=0xfe;record[5]=0;playdat(record);
 assert(reload("/mnt/usb/sub/a.srt")==UB_OK&&attr_calls==1&&last_attr[2]==-4&&last_attr[4]==-2&&!display_value);cases++;
 /* A snapshot of an engine seen in this process still wins over play_dat. */
 attrs[1]=5;attrs[2]=6;attrs[3]=7;attrs[4]=8;struct ub_status status;ub_test_refresh(&status);
 assert(!ub_hook_close());playdat(record);attr_calls=0;atomic_write(&session,1);
 assert(reload("/mnt/usb/sub/a.srt")==UB_OK&&attr_calls==1);
 assert(last_attr[0]==0&&last_attr[1]==5&&last_attr[2]==6&&last_attr[3]==7&&last_attr[4]==8);cases++;
 assert(!ub_hook_close());
 /* Unreadable play_dat keeps the previous behavior: no invented attributes. */
 setup();assert(reload("/mnt/usb/sub/a.srt")==UB_OK&&!attr_calls&&loaded_value==1);cases++;
 assert(!ub_hook_close());
 /* 5. play_dat held by its owner at the first Open: nothing is opened, and
  *    the same selection once it is free gets play_dat's style and display. */
 {unsigned char fresh[8]={0xf8,0,3,2,1,1,0,0};
  setup();playdat(fresh);
  int held=open(UB_PLAY_LOCK,O_RDWR|O_CREAT,0666);assert(held>=0&&!flock(held,LOCK_EX));
  assert(reload("/mnt/usb/sub/a.srt")==UB_BUSY&&!open_calls&&!active_value&&!attr_calls);
  assert(!flock(held,LOCK_UN));close(held);
  assert(reload("/mnt/usb/sub/a.srt")==UB_OK&&open_calls==1&&display_value==1&&attrs[2]==8);cases++;
  assert(!ub_hook_close());
  /* 6. Set_Attr reports success after undoing the change: the style stays
   *    pending through snapshots and the next selection applies it. */
  setup();playdat(fresh);attr_ignore=1;
  assert(reload("/mnt/usb/sub/a.srt")==UB_PARSE_ERROR&&loaded_value==1&&attrs[2]==0);
  {struct ub_status st;ub_test_refresh(&st);}
  attr_ignore=0;assert(reload("/mnt/usb/sub/a.srt")==UB_OK&&attrs[2]==8&&attrs[4]==2&&display_value==1);cases++;
  assert(!ub_hook_close());
  /* The same after a Set_Attr error. */
  setup();playdat(fresh);attr_result=3;
  assert(reload("/mnt/usb/sub/a.srt")==UB_PARSE_ERROR&&attrs[2]==0);
  attr_result=0;assert(reload("/mnt/usb/sub/a.srt")==UB_OK&&attrs[2]==8&&display_value==1);cases++;
  assert(!ub_hook_close());
 }
 /* 4. A failed natural Open keeps the session for a menu selection during
  *    this playback; the stock session end (no Close: loaded is 0) ends it. */
 setup();atomic_write(&session,0);open_error=3;
 assert(ub_hook_open("/mnt/usb/sub/sub.srt")==3&&atomic_read(&session)==1&&!loaded_value&&!active_value);
 open_error=0;uint32_t epoch=state.epoch;
 ub_original_session_end=native_session_end;assert(ub_hook_session_end(1)==0x5e);
 assert(session_end_calls==1&&!close_calls&&!atomic_read(&session)&&state.epoch!=epoch&&status_dirty);
 assert(reload("/mnt/usb/sub/a.srt")==UB_NO_ENGINE&&open_calls==1&&!active_value);cases++;
 setup();atomic_write(&session,0);open_error=3;assert(ub_hook_open("/mnt/usb/sub/sub.srt")==3);open_error=0;
 assert(reload("/mnt/usb/sub/a.srt")==UB_OK&&loaded_value==1&&atomic_read(&session)==1);
 epoch=state.epoch;ub_hook_session_end(1);
 assert(close_calls==1&&!loaded_value&&!atomic_read(&session)&&state.epoch==epoch+1);cases++;
 /* A rejected first file hasn't established a style baseline. */
 setup();record[0]=0xf8;record[5]=1;playdat(record);open_error=3;
 assert(reload("/mnt/usb/sub/bad.srt")==UB_PARSE_ERROR&&!saved_attrs_valid&&!restore_pending);
 record[0]=0xfe;record[5]=0;playdat(record);open_error=0;
 assert(reload("/mnt/usb/sub/good.srt")==UB_OK&&attrs[2]==2&&!display_value);cases++;
 assert(!ub_hook_close());
 /* Hold SELECT inside Open until STOP starts. Stock samples loaded only
  * after SELECT finishes and can reenter Close without waiting on itself. */
 setup();open_error=3;assert(ub_hook_open("/mnt/usb/sub/sub.srt")==3);open_error=0;
 state.count=1;state.items[0].kind=UB_KIND_FILE;copy(state.items[0].path,"/mnt/usb/sub/user.srt",sizeof state.items[0].path);
 ub_original_open=open_during_stop;ub_original_session_end=native_session_end;
 pthread_t selector;int select_result=-1;
 assert(!pthread_barrier_init(&opening,0,2));
 alarm(10);
 assert(!pthread_create(&selector,0,select_during_stop,&select_result));
 int reached=pthread_barrier_wait(&opening);assert(!reached||reached==PTHREAD_BARRIER_SERIAL_THREAD);
 epoch=state.epoch;assert(ub_hook_session_end(1)==0x5e);
 assert(!pthread_join(selector,0));alarm(0);assert(!pthread_barrier_destroy(&opening));
 assert(select_result==UB_STALE&&close_calls==1&&!loaded_value&&!active_value&&!session&&state.epoch==epoch+1);
 assert(reload("/mnt/usb/sub/after.srt")==UB_NO_ENGINE);
 ub_original_open=native_open;
 assert(!ub_hook_open("/mnt/usb/sub/next.srt")&&session);
 assert(!ub_hook_close());cases++;
 /* A failed restore from one session must not override native preferences
  * chosen after that engine was confirmed closed. */
 setup();saved_attrs_valid=1;saved_attrs[2]=8;preferred_enabled=1;attr_result=3;
 assert(reload("/mnt/usb/sub/a.srt")==UB_PARSE_ERROR&&restore_pending);
 attr_result=0;ub_original_session_end=native_session_end;ub_hook_session_end(1);
 assert(!active_value&&!restore_pending&&!saved_attrs_valid);
 assert(!ub_hook_open("/mnt/usb/sub/new.srt"));attrs[2]=2;display_value=0;
 gate_lock();snapshot();gate_unlock();assert(saved_attrs[2]==2&&!preferred_enabled);
 assert(reload("/mnt/usb/sub/selected.srt")==UB_OK&&attrs[2]==2&&!display_value);cases++;
 /* Choosing a file in the browser turns the subtitles on (both branches),
  * stores byte5=1 for the native toggle and tells the helper what opened.
  * A failed choice keeps Off; RELOAD keeps the current display. */
 {
  unsigned char b5;struct ub_sub_selection sel;
  /* live engine, Off */
  setup();record[0]=0xf8;record[5]=1;playdat(record);
  assert(reload("/mnt/usb/sub/a.srt")==UB_OK);
  {struct ub_request off=request(UB_OP_OFF);struct ub_status st;assert(!ub_test_process(&off,&st)&&!display_value);}
  assert(!playdat_byte(5,&b5)&&b5==0);
  start_fail=1;listed("/mnt/usb/sub/bad.srt");
  assert(select_file("/mnt/usb/sub/bad.srt")==UB_PARSE_ERROR&&!display_value&&!preferred_enabled);
  assert(!playdat_byte(5,&b5)&&b5==0);start_fail=0;cases++;
  listed("/mnt/usb/Filmy/b.srt");
  assert(select_file("/mnt/usb/Filmy/b.srt")==UB_OK&&display_value&&preferred_enabled&&!strcmp(state.message,"Subtitle selected"));
  assert(!playdat_byte(5,&b5)&&b5==1);
  /* staged under the gate; the worker writes it after releasing the gate */
  unlink(UB_SUB_SELECTION_FILE);assert(selection_pending);
  {struct ub_sub_selection staged=selection_out;selection_pending=0;selection_flush(&staged);assert(!selection_pending);}
  assert(!ub_br_private_read(UB_SUB_SELECTION_FILE,&sel,sizeof sel)&&ub_sub_selection_valid(&sel,state.brightness_pid,brightness.born));
  assert(sel.kind==UB_SUB_KIND_CHOSEN&&!strcmp(sel.path,"/mnt/usb/Filmy/b.srt")&&sel.seq);cases++;
  {struct ub_request off=request(UB_OP_OFF);struct ub_status st;assert(!ub_test_process(&off,&st)&&!display_value);}
  assert(reload("/mnt/usb/Filmy/b.srt")==UB_OK&&!display_value);cases++; /* RELOAD keeps Off */
  assert(!ub_hook_close());
  /* no engine yet, byte5 0: the choice still shows the file */
  setup();record[5]=0;playdat(record);listed("/mnt/usb/sub/c.srt");
  assert(select_file("/mnt/usb/sub/c.srt")==UB_OK&&display_value&&preferred_enabled);
  assert(!playdat_byte(5,&b5)&&b5==1);cases++;
  assert(!ub_hook_close());
  /* play_dat lock held elsewhere: shown anyway, reported busy, byte5 unchanged */
  setup();record[5]=0;playdat(record);
  assert(reload("/mnt/usb/sub/a.srt")==UB_OK);
  {struct ub_request off=request(UB_OP_OFF);struct ub_status st;assert(!ub_test_process(&off,&st)&&!display_value);}
  {int holder=open(UB_PLAY_LOCK,O_RDWR|O_CREAT,0600);assert(holder>=0&&!flock(holder,LOCK_EX));
   listed("/mnt/usb/sub/d.srt");
   assert(select_file("/mnt/usb/sub/d.srt")==UB_OK&&display_value&&!strcmp(state.message,"Subtitle selected; settings busy"));
   flock(holder,LOCK_UN);close(holder);}
  assert(!playdat_byte(5,&b5)&&b5==0);cases++;
  assert(!ub_hook_close());
  /* the player's own Open of sub.srt is reported as the default kind */
  /* the native Open itself writes nothing */
  unlink(UB_SUB_SELECTION_FILE);setup();assert(!ub_hook_open("/mnt/usb/sub/sub.srt"));
  assert(access(UB_SUB_SELECTION_FILE,F_OK)&&selection_pending);
  assert(selection_out.kind==UB_SUB_KIND_DEFAULT&&!strcmp(selection_out.path,"/mnt/usb/sub/sub.srt"));
  /* a failed write is staged again for the next tick */
  {struct ub_sub_selection staged=selection_out;char saved[160];
   selection_pending=0;memcpy(saved,selection_path,sizeof saved);strcpy(selection_path,"/nonexistent-ub820/selection");
   selection_flush(&staged);assert(selection_pending&&selection_out.seq==staged.seq);
   memcpy(selection_path,saved,sizeof saved);selection_pending=0;selection_flush(&staged);}
  assert(!ub_br_private_read(UB_SUB_SELECTION_FILE,&sel,sizeof sel)&&sel.kind==UB_SUB_KIND_DEFAULT&&!strcmp(sel.path,"/mnt/usb/sub/sub.srt"));cases++;
  assert(!ub_hook_close());
  /* start folder: only a hint for this exact process is used */
  {struct ub_sub_hint h;uint64_t born;assert(!ub_br_birth((unsigned)getpid(),&born));
   memset(&h,0,sizeof h);h.magic=UB_SUB_HINT_MAGIC;h.version=UB_SUB_RECORD_VERSION;h.pid=(uint32_t)getpid();
   h.birth_lo=(uint32_t)born;h.birth_hi=(uint32_t)(born>>32);h.flags=UB_SUB_HINT_START_DIR;strcpy(h.start_dir,"/mnt/usb/Filmy");
   assert(!ub_br_private_write(UB_SUB_HINT_FILE,&h,sizeof h));
   setup();assert(!strcmp(hint_start_dir,"/mnt/usb/Filmy"));
   h.pid++;assert(!ub_br_private_write(UB_SUB_HINT_FILE,&h,sizeof h));setup();assert(!hint_start_dir[0]);
   h.pid--;strcpy(h.start_dir,"/mnt/usb/../etc");assert(!ub_br_private_write(UB_SUB_HINT_FILE,&h,sizeof h));setup();assert(!hint_start_dir[0]);
   h.reserved[0]=1;strcpy(h.start_dir,"/mnt/usb/Filmy");assert(!ub_br_private_write(UB_SUB_HINT_FILE,&h,sizeof h));setup();assert(!hint_start_dir[0]);
   cases++;}
  unlink(UB_SUB_HINT_FILE);unlink(UB_SUB_SELECTION_FILE);
 }
 /* Disc memory: the remembered file replaces only the player's own sub.srt
  * Open; a missing one falls back once with sync/charcode reset; a choice
  * made in this process wins. */
 {
  unsigned char b3,b4,rec[8]={0xf8,0,0,2,1,1,0,0};
  /* success: helper already wrote b3/b4; the kind is "remembered" */
  remembered_setup(UB_SUB_HINT_AUTOLOAD,"/mnt/usb/Filmy/film.srt",2,1,rec);
  assert(!ub_hook_open(UB_DEFAULT_SRT));
  assert(opened_count==1&&!strcmp(opened[0],"/mnt/usb/Filmy/film.srt")&&!strcmp(current_path,"/mnt/usb/Filmy/film.srt"));
  assert(selection_pending&&selection_out.kind==UB_SUB_KIND_REMEMBERED&&!strcmp(selection_out.path,"/mnt/usb/Filmy/film.srt"));
  assert(!playdat_byte(3,&b3)&&b3==2&&!playdat_byte(4,&b4)&&b4==1);
  /* a later natural Open opens it again and keeps what the viewer changed */
  {unsigned char changed[8]={0xf8,0,0,5,0,1,0,0};playdat(changed);}
  assert(!ub_hook_close());assert(!ub_hook_open(UB_DEFAULT_SRT));
  assert(opened_count==2&&!strcmp(opened[1],"/mnt/usb/Filmy/film.srt"));
  assert(!playdat_byte(3,&b3)&&b3==5&&!playdat_byte(4,&b4)&&b4==0);
  assert(!ub_hook_close());cases++;
  /* success without the helper's start write: the first Open applies the entry */
  {unsigned char zero_rec[8]={0xf8,0,0,0,0,1,0,0};remembered_setup(UB_SUB_HINT_AUTOLOAD,"/mnt/usb/Filmy/film.srt",-3,1,zero_rec);}
  assert(!ub_hook_open(UB_DEFAULT_SRT));
  assert(!playdat_byte(3,&b3)&&(signed char)b3==-3&&!playdat_byte(4,&b4)&&b4==1);
  assert(!ub_hook_close());cases++;
  /* missing file: one try, then sub.srt with sync 0 and UTF-8 */
  remembered_setup(UB_SUB_HINT_AUTOLOAD,"/mnt/usb/gone.srt",2,1,rec);
  assert(!ub_hook_open(UB_DEFAULT_SRT));
  assert(opened_count==2&&!strcmp(opened[0],"/mnt/usb/gone.srt")&&!strcmp(opened[1],UB_DEFAULT_SRT));
  assert(!playdat_byte(3,&b3)&&b3==0&&!playdat_byte(4,&b4)&&b4==0);
  assert(selection_out.kind==UB_SUB_KIND_DEFAULT&&!strcmp(state.message,"Remembered subtitle file could not be opened"));
  assert(!ub_hook_close());assert(!ub_hook_open(UB_DEFAULT_SRT));
  assert(opened_count==3&&!strcmp(opened[2],UB_DEFAULT_SRT));
  assert(!ub_hook_close());cases++;
  /* missing file while play_dat is locked: bounded retries, b3/b4 left, sub.srt still opened */
  remembered_setup(UB_SUB_HINT_AUTOLOAD,"/mnt/usb/gone.srt",2,1,rec);
  {int holder=open(UB_PLAY_LOCK,O_RDWR|O_CREAT,0600);assert(holder>=0&&!flock(holder,LOCK_EX));
   assert(!ub_hook_open(UB_DEFAULT_SRT));flock(holder,LOCK_UN);close(holder);}
  assert(opened_count==2&&!playdat_byte(3,&b3)&&b3==2);
  assert(!ub_hook_close());cases++;
  /* only sub.srt is replaced */
  remembered_setup(UB_SUB_HINT_AUTOLOAD,"/mnt/usb/Filmy/film.srt",2,1,rec);
  assert(!ub_hook_open("/mnt/usb/sub/other.srt"));
  assert(opened_count==1&&!strcmp(opened[0],"/mnt/usb/sub/other.srt")&&selection_out.kind==UB_SUB_KIND_DEFAULT);
  assert(!ub_hook_close());cases++;
  /* a file chosen in this process wins over the memory, also on later natural Opens */
  remembered_setup(UB_SUB_HINT_AUTOLOAD,"/mnt/usb/Filmy/film.srt",2,1,rec);
  listed("/mnt/usb/sub/chosen.srt");assert(select_file("/mnt/usb/sub/chosen.srt")==UB_OK);
  assert(!ub_hook_close());
  opened_count=0;assert(!ub_hook_open(UB_DEFAULT_SRT));
  assert(opened_count==1&&!strcmp(opened[0],"/mnt/usb/sub/chosen.srt")&&selection_out.kind==UB_SUB_KIND_CHOSEN);
  assert(!ub_hook_close());cases++;
  /* a hint without the autoload flag, or with a non-SRT file, autoloads nothing */
  remembered_setup(UB_SUB_HINT_START_DIR,"/mnt/usb/Filmy/film.srt",2,1,rec);
  assert(!hint_autoload[0]);
  remembered_setup(UB_SUB_HINT_AUTOLOAD,"/mnt/usb/Filmy/film.txt",2,1,rec);
  assert(!hint_autoload[0]);assert(!ub_hook_open(UB_DEFAULT_SRT));assert(!strcmp(opened[0],UB_DEFAULT_SRT));
  assert(!ub_hook_close());cases++;
  unlink(UB_SUB_HINT_FILE);unlink(UB_SUB_SELECTION_FILE);
 }
 unlink(UB_PLAY_DAT);unlink(UB_PLAY_LOCK);assert(!rmdir(directory));
 printf("subtitle_engine_lifecycle_test passed: %u cases; published Open after restore errors, unmap before Start, stock first-Open attributes, session end without Close\n",cases);
 return 0;
}
