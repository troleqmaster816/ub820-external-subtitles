/* UB820 1.82 in-process subtitle controller.  Workspace build only.
 * The supervisor verifies firmware bytes, relocates this PIC object, fills
 * the four original trampolines, then installs hooks.  No ELF constructors.
 */
#define _GNU_SOURCE
#include "protocol.h"
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include "../../core/payload_thread.h"
#include "precision_render.h"
#include "precision_transport.h"
#include "precision_wake.h"
#include "luminance.h"
#include "brightness_io.h"
extern int UFS_OpenDir(const char *, uint32_t *);
extern int UFS_ReadDir(uint32_t, void *, uint32_t *);
extern int UFS_CloseDir(uint32_t);
extern int TiSetForeground(void *,void *,uint32_t);

#ifndef UB_PLAY_DAT
#define UB_PLAY_DAT "/tmp/apl_play3dlib_play_dat.dat"
#define UB_PLAY_LOCK "/tmp/apl_play3dlib_3ddynamic_lock"
#endif

int (*ub_original_open)(const char *);
int (*ub_original_draw)(uint32_t);
int (*ub_original_close)(void);
int (*ub_original_update)(uint32_t,uint32_t);

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
 int (*set_foreground)(void *,void *,uint32_t);
 void *volatile *ti_display;
 void *volatile *text_gc;
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
 (void *)0x45710, (const char *)0x45e38,
 TiSetForeground,(void *volatile *)0x45734,(void *volatile *)0x45748
};
#else
static struct ub_api api;
#endif

static struct ub_status state;
static volatile uint32_t gate, started, closing, lifecycle, session;
static uint32_t initialized, last_seq, preferred_enabled, saved_attrs_valid, status_dirty, explicit_selection;
static int32_t saved_attrs[5];
static unsigned text_level=UB_TEXT_LEVEL_DEFAULT;
static struct {
 struct ub_brightness_seed seed;
 unsigned seed_ok,seed_pending,fault;
 uint64_t born;
 uint32_t phase,id,epoch,owner;
 uint64_t owner_born;
 uint32_t base_color,base_level,base_desired;
 uint32_t commit_seq,commit_epoch,commit_percent,commit_level;
} brightness;
static char current_path[1024]; /* Always readable1024 bytes for native memcpy. */
static char candidate_path[1024], rollback_path[1024];
/* All clock snapshots and generation changes are protected by gate. A separate
 * clock worker does no catalogue/status I/O and never holds gate while querying
 * the other process. Fractions belong only to its synchronous native Draw call;
 * unrelated native Update callers retain the original seconds ABI. */
static volatile uint32_t precision_started,precision_call_active;
static pthread_t precision_call_owner;
static uint32_t precision_call_seconds,precision_call_cc;
static uint32_t precision_call_backward,precision_refresh_requested,precision_refresh_consumed,precision_refresh_generation,precision_refresh_ms;
static uint32_t precision_generation=1,precision_valid,precision_seconds,precision_cc;
static uint32_t precision_native_seen,precision_native_seconds,precision_drawn;
static uint32_t precision_render_failed,precision_force_seek;
static uint64_t precision_stamp,precision_last_ticks,precision_native_ms;
static struct ub_precision_wake precision_wake={-1,0};
static uint32_t precision_observed_display;
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

static void precision_invalidate(void) {
 uint32_t next=atomic_read(&precision_generation)+1;
 atomic_write(&precision_generation,next?next:1);
 precision_refresh_consumed=atomic_read(&precision_refresh_requested);
 precision_valid=precision_drawn=precision_render_failed=precision_force_seek=0;
 ub_precision_wake_signal(&precision_wake);
}
static int precision_draw(uint32_t seconds,uint32_t cc) {
 int rc;uint64_t ticks=(uint64_t)seconds*45000u+cc*450u;
 if(!ub_original_draw)return 3;
 /* Native Draw detects backwards whole seconds. It cannot detect a backwards
  * seek within one second, so preserve that operation explicitly as well. */
 __atomic_store_n(&precision_call_backward,precision_force_seek || (precision_drawn && ticks<precision_last_ticks),__ATOMIC_RELAXED);
 __atomic_store_n(&precision_call_owner,pthread_self(),__ATOMIC_RELAXED);
 __atomic_store_n(&precision_call_seconds,seconds,__ATOMIC_RELAXED);
 __atomic_store_n(&precision_call_cc,cc,__ATOMIC_RELAXED);
 atomic_write(&precision_call_active,1);
 rc=ub_original_draw(seconds);
 atomic_write(&precision_call_active,0);
 if(rc==0 || rc==1){precision_last_ticks=ticks;precision_drawn=1;}
 precision_render_failed=rc!=0 && rc!=1;
 precision_force_seek=precision_render_failed;
 return rc;
}
int ub_hook_update(uint32_t display,uint32_t seconds) {
 /* Do not take gate: the caller already owns the native engine mutex. Taking
  * gate here would invert gate -> engine ordering in concurrent native paths. */
 int own=atomic_read(&precision_call_active) &&
    pthread_equal(__atomic_load_n(&precision_call_owner,__ATOMIC_RELAXED),pthread_self());
 if(own && seconds==__atomic_load_n(&precision_call_seconds,__ATOMIC_RELAXED)){
  /* Consume subsecond seek state in the same engine critical section as the
   * update itself. No other native Update can steal it between two locks. */
  if(__atomic_load_n(&precision_call_backward,__ATOMIC_RELAXED))api.seek(1);
  return ub_precision_render_update_fast(display,seconds,__atomic_load_n(&precision_call_cc,__ATOMIC_RELAXED));
 }
 uint32_t generation=atomic_read(&precision_generation);
 int rc=ub_original_update?ub_original_update(display,seconds):7;
 /* An explicit native style/display refresh on pause authorizes one precise
  * refresh even when normal second-level playback events have stopped. */
 uint64_t now;
 if(!own && display && (rc==0 || rc==1) && !ub_payload_now_ms(&now)){
  atomic_write(&precision_refresh_ms,(uint32_t)now);
  atomic_write(&precision_refresh_generation,generation);
  __atomic_add_fetch(&precision_refresh_requested,1,__ATOMIC_RELEASE);
  ub_precision_wake_signal(&precision_wake);
 }
 if(!own && (rc==0 || rc==1) &&
    __atomic_exchange_n(&precision_observed_display,display!=0,__ATOMIC_ACQ_REL)!=(display!=0))
  ub_precision_wake_signal(&precision_wake);
 return rc;
}
static int precision_fresh(uint32_t native_seconds) {
 uint64_t now,low,high,ticks;
 if(!precision_valid || ub_payload_now_ms(&now) || now<precision_stamp || now-precision_stamp>200)return 0;
 ticks=(uint64_t)precision_seconds*45000u+precision_cc*450u;
 low=(uint64_t)(native_seconds?native_seconds-1:0)*45000u;
 high=((uint64_t)native_seconds+2)*45000u;
 return ticks>=low && ticks<high;
}
static void precision_split(uint32_t ticks,uint32_t *seconds,uint32_t *cc) {
 /* Floor-reciprocal division, followed by its at-most-one correction. Avoid
  * introducing __aeabi_uidiv imports with the payload's -Os toolchain. */
 uint32_t sec=(uint32_t)(((uint64_t)ticks*95443u)>>32),rem,fraction;
 if(ticks-sec*45000u>=45000u)sec++;
 rem=ticks-sec*45000u;
 fraction=(uint32_t)(((uint64_t)rem*9544371u)>>32);
 if(rem-fraction*450u>=450u)fraction++;
 *seconds=sec;*cc=fraction;
}
static int precision_commit(const struct ub_pc_state *clock,uint32_t generation) {
 uint64_t now,ticks;uint32_t seconds,cc,refresh;
 if(generation!=precision_generation)return 0;
 if(ub_payload_now_ms(&now)){precision_valid=0;return 0;}
 refresh=atomic_read(&precision_refresh_requested);
 if(atomic_read(&precision_refresh_generation)!=generation ||
    (uint32_t)((uint32_t)now-atomic_read(&precision_refresh_ms))>500)
  precision_refresh_consumed=refresh;
 /* Socket validation happened without gate. Validate again after any wait for
  * a parser/style/native operation, using the current coarse clock/lifecycle. */
 if(!atomic_read(&session) || atomic_read(&closing) || *api.active!=1 || !*api.parser ||
    !*api.display ||
    !ub_pc_sample(clock,now,UB_PC_MAX_AGE_MS,*api.clock,&ticks) || ticks>=3888000000ULL){
  precision_valid=0;return 0;
 }
 precision_split((uint32_t)ticks,&seconds,&cc);
 precision_valid=1;precision_seconds=seconds;precision_cc=cc;precision_stamp=clock->monotonic_ms;
 /* Reapply a paused position after a native style/toggle refresh, but do not
  * redraw cached unchanged samples merely because poll woke a second time. */
 int native_live=precision_native_seen && now>=precision_native_ms && now-precision_native_ms<=1500;
 if((native_live && !precision_render_failed &&
     (!precision_drawn || precision_last_ticks!=(uint64_t)seconds*45000u+cc*450u)) ||
    (refresh!=precision_refresh_consumed && atomic_read(&precision_refresh_generation)==generation)){
  precision_refresh_consumed=refresh;precision_draw(seconds,cc);
 }
 return 1;
}

static void *precision_worker(void *unused) {
 struct ub_pc_client client;uint64_t ticks;uint32_t generation,coarse,active,previous_active=0;
 uint32_t observed_wake=0;
 (void)unused;ub_pc_client_init(&client);ub_precision_wake_init(&precision_wake);
 for(;;){
  ub_precision_wake_drain(&precision_wake);
  uint32_t wake=ub_precision_wake_generation(&precision_wake);
  if(wake!=observed_wake){ub_pc_client_wake(&client);observed_wake=wake;}
  gate_lock();
  active=atomic_read(&session) && !atomic_read(&closing) && *api.active==1 && *api.parser && *api.display;
  if(!active && previous_active)precision_invalidate();
  previous_active=active;generation=precision_generation;coarse=*api.clock;
  gate_unlock();
  /* Datagrams carry a whole coherent sample; no unprotected ARM32 64-bit
   * shared load, extrapolation, native API call or socket wait under gate. */
  (void)ub_pc_client_step(&client,((uint64_t)(uint32_t)getpid()<<32)|generation,active,coarse,&ticks);
  gate_lock();precision_commit(&client.state,generation);gate_unlock();
  if(ub_precision_wake_generation(&precision_wake)!=wake)continue;
  struct pollfd fds[2];unsigned count=0;int local_fd=ub_precision_wake_fd(&precision_wake);
  if(local_fd>=0)fds[count++]=(struct pollfd){local_fd,POLLIN,0};
  if(active && client.fd>=0)fds[count++]=(struct pollfd){client.fd,POLLIN,0};
  /* Off and explicit pause have no periodic wake when event delivery exists.
   * A native action increments the generation and wakes local_fd; native
   * source state/seek notifications wake client.fd. No pause interpolation. */
  int timeout=local_fd>=0 && (!active || client.paused)?-1:(int)UB_PC_PERIOD_MS;
  if(count)(void)poll(fds,count,timeout);
  else usleep(20000);
 }
 return 0;
}

static void initialize(void) {
 if(initialized)return;
 zero(&state,sizeof(state));state.magic=UB_MAGIC;state.version=UB_VERSION;
#ifdef UB_HOST_TEST
 state.epoch=0x1234;
#else
 state.epoch=(uint32_t)getpid() ^ (uint32_t)(uintptr_t)&state;
 if(!state.epoch)state.epoch=1;
#endif
 state.brightness_pid=(uint32_t)getpid();
 if(!ub_br_birth(state.brightness_pid,&brightness.born)){
  state.brightness_birth_lo=(uint32_t)brightness.born;state.brightness_birth_hi=(uint32_t)(brightness.born>>32);
  struct ub_brightness_seed seed;
  if(!ub_br_private_read(UB_BRIGHTNESS_SEED,&seed,sizeof seed)&&
     ub_br_seed_valid(&seed,state.brightness_pid,brightness.born)){
   brightness.seed=seed;brightness.seed_ok=1;brightness.seed_pending=1;
   text_level=ub_brightness_level(seed.percent);memcpy(state.brightness_token,seed.token,16);
  }
 }
 copy(state.dir,"/mnt/usb/sub",sizeof(state.dir));
 copy(state.message,"Choose a subtitle file",sizeof(state.message));
 initialized=1;
}
static void brightness_snapshot_locked(void);
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
 brightness_snapshot_locked();
 engine_unlock();
 copy(state.current,current_path,sizeof(state.current));
}
static int finish(int result,const char *message) {
 state.result=result;copy(state.message,message,sizeof(state.message));snapshot();return result;
}

/* Caller owns gate AND the native ESL mutex. These are the exact native
 * PreDisplay fields; Close frees them under that same mutex. The GC foreground
 * at+8 is read only to preserve alpha and to restore the complete prior color
 * on a render error. All color WRITES use TiSetForeground's native lock/API. */
static int text_color_apply_locked(uint32_t value,unsigned exact,unsigned redraw,struct ub_text_level_outcome *out){
 void *ti,*gc;uint32_t previous;struct ub_text_level_api color;
 if(!api.active||!api.parser||*api.active!=1||!*api.parser||!api.set_foreground||
    !api.ti_display||!api.text_gc||!(ti=*api.ti_display)||!(gc=*api.text_gc))return UB_NO_ENGINE;
 previous=*(const uint32_t *)((const unsigned char *)gc+8);
 color=(struct ub_text_level_api){api.set_foreground,api.dirty,
  ub_precision_renderer.prepare,ub_precision_renderer.render,ub_precision_renderer.present,
  ub_precision_renderer.unmap,ub_precision_renderer.mapped};
 unsigned visible=redraw&&api.display&&*api.display&&color.mapped&&*color.mapped==1;
 int rc=exact?ub_text_color_apply(&color,ti,gc,previous,value,visible,out):
  ub_text_level_apply(&color,ti,gc,previous,value,visible,out);
 return rc==UB_TEXT_LEVEL_OK?UB_OK:UB_PARSE_ERROR;
}
static int text_level_apply_locked(unsigned value,unsigned redraw,struct ub_text_level_outcome *out){
 return text_color_apply_locked(value,0,redraw,out);
}
static int brightness_surface_locked(uint32_t *color){
 if(!api.active||!api.parser||*api.active!=1||!*api.parser||!api.set_foreground||
    !api.ti_display||!api.text_gc||!*api.ti_display||!*api.text_gc)return 0;
 *color=*(const uint32_t *)((const unsigned char *)*api.text_gc+8);return 1;
}
static void brightness_snapshot_locked(void){
 uint32_t color=0;int live=brightness_surface_locked(&color);
 unsigned level=live?(color&255u):text_level;
 state.text_level=level;state.brightness_percent=ub_brightness_percent(level);
 state.brightness_known=live&&!brightness.fault&&((color>>8)&255u)==level&&((color>>16)&255u)==level;
 state.brightness_available=state.brightness_known&&brightness.seed_ok&&!brightness.seed_pending&&
  atomic_read(&session)&&!atomic_read(&closing);
 state.brightness_edit=brightness.id;state.brightness_edit_epoch=brightness.epoch;
 state.brightness_base_raw=brightness.base_level;state.brightness_phase=brightness.phase;
 state.brightness_owner_pid=brightness.owner;
 state.brightness_owner_start_lo=(uint32_t)brightness.owner_born;state.brightness_owner_start_hi=(uint32_t)(brightness.owner_born>>32);
 state.brightness_commit_seq=brightness.commit_seq;state.brightness_commit_epoch=brightness.commit_epoch;
 state.brightness_commit_percent=brightness.commit_percent;state.brightness_commit_level=brightness.commit_level;
}
/* Caller owns gate, not ESL mutex: both public Open paths call this before
 * Set_Attr/Set_Display can render the newly created surface. Native Open
 * already supplies white, so the default adds no setter or redraw work. */
static int text_level_restore(unsigned redraw){
 struct ub_text_level_outcome outcome={0};int rc;
 brightness.seed_pending=0;
 engine_lock();rc=text_level_apply_locked(text_level,redraw,&outcome);engine_unlock();
 /* Native Open supplies default white even without a live optional color
  * surface. The status snapshot still reports its availability honestly. */
 if(rc==UB_NO_ENGINE&&text_level==UB_TEXT_LEVEL_DEFAULT)rc=UB_OK;
 brightness.fault=rc!=UB_OK;
 return rc;
}
static int text_level_restore_after_open(void){return text_level_restore(0);}
static int text_level_set(unsigned value){
 struct ub_text_level_outcome outcome={0};int rc;char message[96];
 if(value>UB_TEXT_LEVEL_MAX)return finish(UB_BAD_PATH,"Invalid subtitle text level");
 if(!atomic_read(&session)||atomic_read(&closing))return finish(UB_NO_ENGINE,"Playback subtitle session is closed");
 engine_lock();rc=text_level_apply_locked(value,1,&outcome);
 if(!rc){text_level=value;brightness.fault=0;brightness.seed_pending=0;}
 else if(outcome.restore_rc||outcome.clear_rc||outcome.repaint_rc||outcome.native_rc<0)brightness.fault=1;
 engine_unlock();
 if(rc==UB_NO_ENGINE)return finish(rc,"No active subtitle text surface");
 if(rc)return finish(rc,outcome.restore_rc||outcome.clear_rc?
  "Text level failed; graphics state unconfirmed":outcome.repaint_rc?
  "Text level failed; previous color restored, text cleared":"Text level failed; previous color retained");
 snprintf(message,sizeof message,"Subtitle text level %u set",value);
 return finish(UB_OK,message);
}

static void brightness_edit_end(void){
 brightness.phase=UB_BRIGHTNESS_NONE;brightness.id=brightness.epoch=brightness.owner=0;
 brightness.owner_born=0;status_dirty=1;
}
/* gate held. Death/Close is a one-shot abort, never a recurring recovery
 * writer if graphics reject the restoration. The saved RAM intent is retained. */
static int brightness_cancel(unsigned terminal){
 struct ub_text_level_outcome outcome={0};int rc;
 if(brightness.phase!=UB_BRIGHTNESS_PREVIEW)return UB_STALE;
 engine_lock();rc=text_color_apply_locked(brightness.base_color,1,1,&outcome);engine_unlock();
 if(!rc||terminal||rc==UB_NO_ENGINE){
  text_level=brightness.base_desired;brightness_edit_end();
 }
 if(rc)brightness.fault=1;else brightness.fault=0;
 return rc;
}
static int brightness_request_valid(const struct ub_request *r){
 if(!brightness.seed_ok||memcmp(r->brightness.token,brightness.seed.token,16)||
    !r->brightness.owner_pid||r->brightness.owner_pid>INT32_MAX)return 0;
 for(unsigned i=0;i<sizeof r->brightness.reserved;i++)if(r->brightness.reserved[i])return 0;
 return 1;
}
static int brightness_request(const struct ub_request *r){
 uint64_t owner_born=((uint64_t)r->brightness.owner_start_hi<<32)|r->brightness.owner_start_lo;
 uint32_t color=0;int rc;
 if(!brightness_request_valid(r))return finish(UB_BAD_PATH,"Invalid brightness transaction");
 if(r->op==UB_OP_BRIGHTNESS_BEGIN){
  uint64_t observed=0;
  if(brightness.phase!=UB_BRIGHTNESS_NONE)return finish(UB_BUSY,"Brightness transaction is still running");
  if(r->brightness.edit||r->brightness.value||!owner_born||ub_br_birth(r->brightness.owner_pid,&observed)||observed!=owner_born)
   return finish(UB_STALE,"Brightness owner changed");
  engine_lock();brightness_snapshot_locked();
  if(!state.brightness_available||!brightness_surface_locked(&color)){engine_unlock();return finish(UB_NO_ENGINE,"Subtitle brightness is not ready");}
  brightness.phase=UB_BRIGHTNESS_PREVIEW;brightness.id=r->seq;brightness.epoch=state.epoch;
  brightness.owner=r->brightness.owner_pid;brightness.owner_born=owner_born;
  brightness.base_color=color;brightness.base_level=color&255u;brightness.base_desired=text_level;
  brightness.commit_seq=brightness.commit_epoch=brightness.commit_percent=brightness.commit_level=0;
  engine_unlock();return finish(UB_OK,"Brightness preview ready");
 }
 if(brightness.phase==UB_BRIGHTNESS_NONE||r->brightness.edit!=brightness.id||brightness.epoch!=state.epoch||
    r->brightness.owner_pid!=brightness.owner||owner_born!=brightness.owner_born)
  return finish(UB_STALE,"Brightness transaction changed");
 if(brightness.phase==UB_BRIGHTNESS_COMMIT)return finish(UB_BUSY,"Brightness save is already authorized");
 switch(r->op){
 case UB_OP_BRIGHTNESS_PREVIEW:
  if(r->brightness.value>100)return finish(UB_BAD_PATH,"Invalid brightness percentage");
  return text_level_set(ub_brightness_level(r->brightness.value));
 case UB_OP_BRIGHTNESS_CANCEL:
  if(r->brightness.value)return finish(UB_BAD_PATH,"Invalid brightness cancel");
  rc=brightness_cancel(0);
  return finish(rc,rc?"Previous brightness could not be restored":"Previous brightness restored");
 case UB_OP_BRIGHTNESS_COMMIT:
  if(r->brightness.value>100)return finish(UB_BAD_PATH,"Invalid brightness percentage");
  engine_lock();brightness_snapshot_locked();
  if(!state.brightness_available||state.text_level!=ub_brightness_level(r->brightness.value)||
     state.brightness_percent!=r->brightness.value){engine_unlock();return finish(UB_STALE,"Confirm the latest brightness preview first");}
  {
   struct ub_brightness_save_ack notice={0};
   notice.magic=UB_BRIGHTNESS_SAVE_MAGIC;notice.version=UB_BRIGHTNESS_PROTOCOL;
   memcpy(notice.token,brightness.seed.token,16);notice.pid=brightness.seed.pid;
   notice.birth_lo=brightness.seed.birth_lo;notice.birth_hi=brightness.seed.birth_hi;
   notice.edit=brightness.id;notice.epoch=brightness.epoch;notice.seq=r->seq;
   notice.percent=r->brightness.value;notice.level=state.text_level;notice.result=-1;
   engine_unlock();
   /* The gate preserves this transaction, but RAM IO never holds ESL mutex.
    * Failure leaves PREVIEW editable/cancellable and authorizes no save. */
   if(ub_br_private_write(UB_BRIGHTNESS_COMMIT_NOTICE,&notice,sizeof notice))
    return finish(UB_IO_ERROR,"Brightness applied; save request could not be published");
   brightness.phase=UB_BRIGHTNESS_COMMIT;brightness.commit_seq=notice.seq;
   brightness.commit_epoch=notice.epoch;brightness.commit_percent=notice.percent;
   brightness.commit_level=notice.level;text_level=notice.level;
  }
  return finish(UB_OK,"Brightness applied; saving choice");
 }
 return finish(UB_BAD_PATH,"Unknown brightness operation");
}
static int brightness_save_matches(const struct ub_brightness_save_ack *ack){
 return ack->magic==UB_BRIGHTNESS_SAVE_MAGIC&&ack->version==UB_BRIGHTNESS_PROTOCOL&&
  !memcmp(ack->token,brightness.seed.token,16)&&ack->pid==brightness.seed.pid&&
  ack->birth_lo==brightness.seed.birth_lo&&ack->birth_hi==brightness.seed.birth_hi&&
  ack->edit==brightness.id&&ack->epoch==brightness.commit_epoch&&ack->seq==brightness.commit_seq&&
  ack->percent==brightness.commit_percent&&ack->level==brightness.commit_level&&
  ack->result>=UB_BRIGHTNESS_SAVED&&ack->result<=UB_BRIGHTNESS_SAVE_UNKNOWN;
}
static void brightness_tick(void){
 if(brightness.phase==UB_BRIGHTNESS_PREVIEW){
  uint64_t observed=0;int rc=ub_br_birth(brightness.owner,&observed);
  int gone=rc==-2||(!rc&&observed!=brightness.owner_born);
  if(rc&&kill((pid_t)brightness.owner,0)&&errno==ESRCH)gone=1;
  if(gone){int result=brightness_cancel(1);finish(result?result:UB_STALE,result?
   "Brightness owner ended; restoration unconfirmed":"Brightness owner ended; preview restored");status_dirty=1;}
 }else if(brightness.phase==UB_BRIGHTNESS_COMMIT){
  struct ub_brightness_save_ack ack;
  if(!ub_br_private_read(UB_BRIGHTNESS_SAVE_ACK,&ack,sizeof ack)&&brightness_save_matches(&ack)){
   brightness_edit_end();
   copy(state.message,ack.result==UB_BRIGHTNESS_SAVED?"Brightness saved":"Brightness applied; not saved to USB",sizeof state.message);
  }
 }
}
/* Called before a new native Open. A preview cannot cross engine epochs;
 * an already authorized COMMIT remains immutable until its save ACK. */
static void brightness_before_open(void){
 if(brightness.phase==UB_BRIGHTNESS_PREVIEW){
  (void)brightness_cancel(1);
 }
}
struct ub_catalogue {
 uint32_t count;
 char dir[UB_PATH_BYTES],message[128];
 struct ub_item items[UB_MAX_ITEMS];
};
static void sort_items(struct ub_catalogue *list) {
 uint32_t i,j;struct ub_item temp;
 for(i=1;i<list->count;i++)for(j=i;j>0;j--) {
  struct ub_item *a=&list->items[j-1],*b=&list->items[j];
  if(a->kind>b->kind || (a->kind==b->kind && name_compare(a->name,b->name)<=0))break;
  temp=*a;*a=*b;*b=temp;
 }
}
/* Does not read controller/native state. UFS may block on slow media. */
static int scan_catalogue(const char *path,struct ub_catalogue *list) {
 uint32_t handle=0,result=0,n=0,limited=0;int rc=0;
 unsigned char record[0x120];char full[UB_PATH_BYTES],parent[UB_PATH_BYTES];
 zero(list,sizeof(*list));
 if(!valid_path(path)){copy(list->message,"Invalid USB directory",sizeof(list->message));return UB_BAD_PATH;}
 zero(list->items,sizeof(list->items));list->count=0;
 copy(list->dir,path,sizeof(list->dir));
 if(UFS_OpenDir(path,&handle)!=0){copy(list->message,"Cannot open USB directory",sizeof(list->message));return UB_IO_ERROR;}
 if(!equal(path,"/mnt/usb")) {
  unsigned long k=length(path,UB_PATH_BYTES);
  copy(parent,path,sizeof(parent));while(k>8 && parent[k-1]!='/')k--;
  parent[k>8?k-1:8]=0;
  list->items[0].kind=UB_KIND_DIR;copy(list->items[0].name,"..",UB_NAME_BYTES);
  copy(list->items[0].path,parent,UB_PATH_BYTES);list->count=1;
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
  if(list->count==UB_MAX_ITEMS){limited=1;continue;}
  list->items[list->count].kind=kind;
  copy(list->items[list->count].name,name,UB_NAME_BYTES);
  copy(list->items[list->count].path,full,UB_PATH_BYTES);list->count++;
 }
 if(UFS_CloseDir(handle)!=0)rc=1;
 if(rc){zero(list->items,sizeof(list->items));list->count=0;copy(list->message,"USB directory read failed",sizeof(list->message));return UB_IO_ERROR;}
 if(n==8192)limited=1;
 sort_items(list);
 copy(list->message,limited?"Directory limited to64 entries; shorten long paths":"Choose a subtitle file",sizeof(list->message));
 return limited?UB_LIMIT:UB_OK;
}
/* Called with gate held; retain the old catalogue while the scan runs.
 * A Close/Open during UFS invalidates this result without publishing a list
 * into the new playback epoch. Only this worker owns catalogue requests. */
static int catalogue(const char *path) {
 struct ub_catalogue list;char directory[UB_PATH_BYTES];
 uint32_t epoch=state.epoch,generation=atomic_read(&lifecycle);int rc;
 if(!valid_path(path))return finish(UB_BAD_PATH,"Invalid USB directory");
 copy(directory,path,sizeof(directory));
 gate_unlock();
 rc=scan_catalogue(directory,&list);
 gate_lock();
 if(epoch!=state.epoch || generation!=atomic_read(&lifecycle))
  return finish(UB_STALE,"Playback changed; refresh the menu");
 state.count=list.count;copy(state.dir,list.dir,sizeof(state.dir));
 for(uint32_t i=0;i<UB_MAX_ITEMS;i++)state.items[i]=list.items[i];
 return finish(rc,list.message);
}
static int listed_file(const char *path) {
 uint32_t i;for(i=0;i<state.count;i++)if(state.items[i].kind==UB_KIND_FILE && equal(path,state.items[i].path))return 1;
 return 0;
}

static int replace_file(const char *path,int require_listed) {
 uint32_t epoch=atomic_read(&lifecycle),display,i,time;int rc,rollback_rc;
 if(!valid_path(path) || !srt_name(path) || (require_listed && !listed_file(path)))return finish(UB_BAD_PATH,"Choose a listed SRT file");
 if(!atomic_read(&session) || atomic_read(&closing))return finish(UB_NO_ENGINE,"Playback subtitle session is closed");
 precision_invalidate();
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
  brightness_before_open();
  rc=ub_original_open(candidate_path);
  if(rc)return finish(UB_PARSE_ERROR,"Cannot load selected subtitle");
  if(text_level_restore_after_open())return finish(UB_PARSE_ERROR,"Subtitle text level restore failed");
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
    r->op<UB_OP_LIST || r->op>UB_OP_BRIGHTNESS_CANCEL ||
    (r->op<=UB_OP_RELOAD&&length(r->path,UB_PATH_BYTES)==UB_PATH_BYTES))
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
 case UB_OP_SET_TEXT_LEVEL:
  if(brightness.phase!=UB_BRIGHTNESS_NONE)return finish(UB_BUSY,"Finish the brightness transaction first");
  if(r->text_level.value>UB_TEXT_LEVEL_MAX)return finish(UB_BAD_PATH,"Invalid subtitle text level");
  for(unsigned i=0;i<sizeof r->text_level.reserved;i++)if(r->text_level.reserved[i])
   return finish(UB_BAD_PATH,"Invalid subtitle text-level request");
  return text_level_set(r->text_level.value);
 case UB_OP_BRIGHTNESS_BEGIN:case UB_OP_BRIGHTNESS_PREVIEW:
 case UB_OP_BRIGHTNESS_COMMIT:case UB_OP_BRIGHTNESS_CANCEL:return brightness_request(r);
 }
 return finish(UB_BAD_PATH,"Unknown menu request");
}

/* The worker captured this immutable snapshot under gate. No native hook
 * waits for filesystem writes or rename, and a later dirty event is retained. */
static int write_status(const struct ub_status *published) {
 char tmp[192];int fd;unsigned long done=0;long n;
 int length=snprintf(tmp,sizeof tmp,"%s.tmp",UB_STATUS_FILE);
 if(length<0||(size_t)length>=sizeof tmp)return -1;
 fd=open(tmp,1|0x40|0x200|0x800|0x20000,0600);
 if(fd<0)return -1;
 while(done<sizeof(*published)) {
  n=write(fd,(const char *)published+done,sizeof(*published)-done);
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
 struct ub_request request;struct ub_status published;
 uint32_t tick=0;unsigned long old_signature;int publish,have_request;(void)unused;
 gate_lock();initialize();
 if(brightness.seed_pending)(void)text_level_restore(1);
 if(catalogue("/mnt/usb/sub")==UB_IO_ERROR)catalogue("/mnt/usb");
 snapshot();published=state;status_dirty=0;gate_unlock();
 if(write_status(&published)){gate_lock();status_dirty=1;gate_unlock();}
 for(;;) {
  usleep(200000);have_request=request_read(&request);gate_lock();publish=0;
  brightness_tick();
  if(have_request && (int32_t)(request.seq-last_seq)>0) {
   process(&request);publish=1;
  }
  if(status_dirty){snapshot();status_dirty=0;publish=1;}
  if(++tick==5) {
   tick=0;old_signature=state.active*2+state.enabled;snapshot();
   if(old_signature!=state.active*2+state.enabled)publish=1;
  }
  if(publish)published=state;
  gate_unlock();
  if(publish && write_status(&published)){gate_lock();status_dirty=1;gate_unlock();}
 }
 return (void *)0;
}
static void bootstrap(void) {
#ifndef UB_HOST_TEST
 unsigned long tid;
 if(__sync_bool_compare_and_swap(&started,0,1)) {
  if(ub_payload_thread_start(&tid,worker,0)==0){pthread_detach(tid);atomic_write(&started,2);}
  else atomic_write(&started,0);
 }
 if(__sync_bool_compare_and_swap(&precision_started,0,1)){
  if(ub_payload_thread_start(&tid,precision_worker,0)==0){pthread_detach(tid);atomic_write(&precision_started,2);}
  else atomic_write(&precision_started,0);
 }
#endif
}

int ub_hook_open(const char *path) {
 int rc;const char *chosen;
 if(!ub_original_open)return 3;
 gate_lock();initialize();brightness_before_open();atomic_write(&session,1);__sync_add_and_fetch(&lifecycle,1);
 precision_invalidate();precision_native_seen=0;
 if(++state.epoch==0)state.epoch=1;
 status_dirty=1;
 chosen=explicit_selection && valid_path(current_path)?current_path:path;
 rc=ub_original_open(chosen);
 if(!rc){
  /* Open has released its native mutex and created a fresh GC. Apply the
   * existing choice before returning to the first Draw; no file/timer/seek. */
  int color_rc=text_level_restore_after_open();
  if(color_rc){state.result=color_rc;copy(state.message,"Subtitle text level could not be restored",sizeof state.message);}
 }
 if(rc==0 && chosen!=current_path && length(chosen,1024)<=UB_PATH_LIMIT && valid_path(chosen)) {
  zero(current_path,sizeof(current_path));copy(current_path,chosen,sizeof(current_path));
 }
 gate_unlock();bootstrap();return rc;
}
int ub_hook_draw(uint32_t time) {
 int rc;
 gate_lock();initialize();
 if(brightness.seed_pending)(void)text_level_restore_after_open();
 if(*api.active==1 && !atomic_read(&closing))atomic_write(&session,1);
 if(precision_native_seen && time!=precision_native_seconds && time!=precision_native_seconds+1)
  precision_invalidate();
 precision_native_seen=1;precision_native_seconds=time;
 if(ub_payload_now_ms(&precision_native_ms))precision_native_seen=0;
 if(precision_fresh(time))rc=precision_draw(precision_seconds,precision_cc);
 else if(precision_drawn && precision_last_ticks>=(uint64_t)time*45000u &&
         precision_last_ticks<((uint64_t)time+2)*45000u){
  uint32_t sec,cc;precision_split((uint32_t)precision_last_ticks,&sec,&cc);
  rc=precision_draw(sec,cc); /* Hold observed time; never extrapolate or rewind on fallback. */
 }else rc=ub_original_draw?ub_original_draw(time):3;
 int wake=*api.active==1 && *api.parser && *api.display;
 gate_unlock();if(wake)ub_precision_wake_signal(&precision_wake);bootstrap();return rc;
}
int ub_hook_close(void) {
 int rc;unsigned cancelled;uint32_t old_color=0;
 if(!ub_original_close)return 3;
 __sync_add_and_fetch(&closing,1);__sync_add_and_fetch(&lifecycle,1);atomic_write(&session,0);
 ub_precision_wake_signal(&precision_wake);
 gate_lock();initialize();snapshot();
 cancelled=brightness.phase==UB_BRIGHTNESS_PREVIEW;
 if(cancelled){old_color=brightness.base_color;text_level=brightness.base_desired;brightness_edit_end();}
 precision_invalidate();precision_native_seen=0;
 if(++state.epoch==0)state.epoch=1;
 status_dirty=1;
 rc=ub_original_close(); /* Preserve original return, including native defects. */
 /* The native helper can return an uninitialized nonzero success value.
  * Clear the outer draw gate only when both authoritative engine indicators
  * confirm shutdown; retain it if Close leaves a live engine/parser. */
 engine_lock();
 if(*api.active==0 && *api.parser==0)*api.loaded=0;
 else if(cancelled){struct ub_text_level_outcome outcome={0};if(text_color_apply_locked(old_color,1,1,&outcome))brightness.fault=1;}
 engine_unlock();
 gate_unlock();__sync_sub_and_fetch(&closing,1);return rc;
}

#ifdef UB_HOST_TEST
void ub_test_bind(const struct ub_api *a) {api=*a;}
void ub_test_reset(void) {
 zero(&state,sizeof(state));zero(current_path,sizeof(current_path));
 zero(candidate_path,sizeof(candidate_path));zero(rollback_path,sizeof(rollback_path));
 gate=started=closing=lifecycle=session=initialized=last_seq=preferred_enabled=saved_attrs_valid=status_dirty=explicit_selection=0;
 text_level=UB_TEXT_LEVEL_DEFAULT;
 zero(&brightness,sizeof brightness);
 precision_started=precision_call_active=precision_valid=precision_drawn=precision_native_seen=0;
 precision_generation=1;
 precision_last_ticks=precision_stamp=precision_native_ms=0;
 precision_seconds=precision_cc=precision_native_seconds=precision_render_failed=precision_force_seek=0;
 precision_refresh_requested=precision_refresh_consumed=precision_refresh_generation=precision_refresh_ms=precision_call_backward=0;
 precision_call_owner=(pthread_t)0;precision_call_seconds=precision_call_cc=0;
 precision_observed_display=0;
 initialize();
}
int ub_test_process(const struct ub_request *r,struct ub_status *out) {
 int rc;gate_lock();rc=process(r);*out=state;gate_unlock();return rc;
}
int ub_test_valid_path(const char *path){return valid_path(path);}
void ub_test_refresh(struct ub_status *out){snapshot();*out=state;}
#endif
