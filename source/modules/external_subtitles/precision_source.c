/* A native-media-time source in apl_bdv. The loader verifies the exact
 * executable and hook bytes and initializes every binding before publishing
 * the branch. No constructors or target API calls occur in the bootstrap.
 *
 * The natural EXT timer callback is only a safe one-time thread entry point;
 * its interval, native registration and original behavior remain unchanged.
 * The independent server thread requests fresh title time through the public
 * synchronous MM API, which serializes the underlying PCE/AVD query on its
 * existing owner thread. Never issue that synchronous request from a native
 * callback: the caller can still hold the PCE event/callback mutexes.
 */
#include <stdint.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>
#ifdef UB_PRECISION_SOURCE_TEST
extern void ub_precision_source_serve(int (*get)(uint32_t[2]),int (*paused)(uint32_t *),int event_fd);
#else
#include "precision_transport.h"
#endif

#include "../../core/payload_thread.h"

void (*ub_original_clock_callback)(const uint32_t *);
void (*ub_original_notify_callback)(const uint32_t *);
int (*ub_native_get_media_time)(uint32_t,uint32_t *);
volatile uint32_t *ub_precision_mm_id;
volatile uint32_t *ub_precision_mm_state;
volatile uint32_t *ub_precision_pce_state;

/* 0 not started, 1 thread creation in progress, 2 resident server started.
 * A failed pthread_create returns to 0 and can retry on the next natural
 * callback. Once a server exists, it owns reconnect/retry for this process's
 * lifetime; no forced cancellation or payload unloading is safe here. */
static uint32_t source_started;
/* The published descriptor remains open for the payload/process lifetime:
 * closing/reusing it while a native callback writes would be unsafe. Only
 * the server creates it, before entering the transport loop. The event
 * counter carries meaning; eventfd coalesces wakeups without losing epochs. */
static int source_event_fd=-1;
static uint32_t source_generation=1,source_clock_seen,source_clock_lo,source_clock_hi;
static uint32_t source_read(volatile uint32_t *p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
static int source_ready(void){
 uint32_t state;
 if(!ub_precision_mm_id||!ub_precision_mm_state||!ub_native_get_media_time)return 0;
 state=source_read(ub_precision_mm_state);
 /* Stock callback dispatcher 0x72b98 admits 2..11; the queued getter's
  * handler at 0x6cb66 additionally rejects 9 and 11. Avoid posting requests
  * in those known inadmissible states instead of guessing their enum names. */
 return state>=2&&state<=11&&state!=9&&state!=11&&source_read(ub_precision_mm_id)!=0;
}
/* Transport contract: zero supplies a valid little-endian two-word 45000-Hz
 * title-media time. On error, the caller's output is left untouched. The
 * native API copies its output even on some error paths, so querying into a
 * local buffer and checking its return value is mandatory. */
static int precision_native_time(uint32_t ticks[2]){
 uint32_t id,value[2]={0,0};int rc;
 if(!ticks||!source_ready())return -1;
 id=source_read(ub_precision_mm_id);
 rc=ub_native_get_media_time(id,value);
 if(rc||!source_ready()||source_read(ub_precision_mm_id)!=id)return -1;
 ticks[0]=value[0];ticks[1]=value[1];return 0;
}
static int precision_native_paused(uint32_t *generation){
 uint32_t before,after,state;
 if(!generation)return -1;
 do {
  before=__atomic_load_n(&source_generation,__ATOMIC_ACQUIRE);
  state=ub_precision_pce_state?source_read(ub_precision_pce_state):0;
  after=__atomic_load_n(&source_generation,__ATOMIC_ACQUIRE);
 }while(before!=after);
 *generation=after;
 if(!ub_precision_pce_state||!source_ready()||source_read(ub_precision_mm_state)==2)return -1;
 /* Native PauseOff calls 0xb5270, whose exact predicate is state==7.
  * State8 is disc STILL (0xb5258), explicitly separate from PauseOnOff.
  * Other states do not authorize sleeping: normal play, title menus and
  * transitions keep the normal request behavior. Never infer pause from
  * an unchanged media timestamp or from the MM readiness enum. MM state2
  * is explicitly called TERMINATE by stock 0x73874: a residual PCE7 there
  * must not authorize sleeping, even though the time getter admits it. */
 return state==7;
}
static void precision_source_changed(void){
 uint64_t one=1;int fd;ssize_t rc;
 __atomic_fetch_add(&source_generation,1,__ATOMIC_RELEASE);
 fd=__atomic_load_n(&source_event_fd,__ATOMIC_ACQUIRE);
 if(fd<0)return;
 do {rc=write(fd,&one,sizeof(one));}while(rc<0&&errno==EINTR);
 /* EAGAIN means a previous event already makes the descriptor readable.
  * No queue operation, allocation or blocking native API belongs here. */
}
static void *precision_source_thread(void *unused){
 int fd;(void)unused;
 fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
 if(fd>=0)__atomic_store_n(&source_event_fd,fd,__ATOMIC_RELEASE);
 __atomic_store_n(&source_started,2,__ATOMIC_RELEASE);
 /* The transport must retain polling when eventfd creation failed. */
 ub_precision_source_serve(precision_native_time,precision_native_paused,fd);
 /* A server that returns must not be respawned from every video callback.
  * The transport owns its retry policy and this payload remains resident. */
 return 0;
}
static void precision_source_bootstrap(void){
 uint32_t expected=0;pthread_t thread;
 if(!source_ready()||!__atomic_compare_exchange_n(&source_started,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
 if(ub_payload_thread_start(&thread,precision_source_thread,0)){
  __atomic_store_n(&source_started,0,__ATOMIC_RELEASE);return;
 }
 /* Failure to detach does not justify killing a thread that may own a
  * synchronous native request. Process teardown eventually reclaims it. */
 (void)pthread_detach(thread);
}
void ub_hook_clock_callback(const uint32_t *info){
 int saved_errno;uint32_t lo,hi,seen;
 if(ub_original_clock_callback)ub_original_clock_callback(info);
 saved_errno=errno;
 if(info){
  /* Callback records are borrowed. Retain only scalar values; do not use
   * atomic64 on ARM32. Concurrent records can cause a harmless extra wake,
   * never an accepted clock sample. Actual samples still use the queued API.
   * An unchanged paused clock does not wake the sleeping server/client. */
  lo=__atomic_exchange_n(&source_clock_lo,info[0],__ATOMIC_RELAXED);
  hi=__atomic_exchange_n(&source_clock_hi,info[1],__ATOMIC_RELAXED);
  seen=__atomic_exchange_n(&source_clock_seen,1,__ATOMIC_RELAXED);
  if(!seen||lo!=info[0]||hi!=info[1])precision_source_changed();
 }
 precision_source_bootstrap();errno=saved_errno;
}
void ub_hook_notify_callback(const uint32_t *info){
 int saved_errno;
 if(ub_original_notify_callback)ub_original_notify_callback(info);
 saved_errno=errno;
 /* Original APL_MEDA_BDV_cb_NotifyInfo (0x626ac) owns this record. Mask0x2
  * reports playback state, but other native notifications include title,
  * speed, boundary and lifecycle changes which must wake a paused client
  * too. A notification is only a wake: it never supplies a clock sample. */
 if(info&&info[0])precision_source_changed();
 errno=saved_errno;
}
