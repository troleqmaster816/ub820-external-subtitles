/* Drive the actual backend clock worker and real UDS source. Native calls and
 * playback events are synthetic; no firmware, media or device is accessed. */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
#define UB_PC_SOURCE_PEER "precision_idle_integration_test"
#define UB_PC_CLIENT_PEER "precision_idle_integration_test"
static char idle_socket[80];
#define UB_PC_SOCKET_NAME idle_socket
#include "../modules/external_subtitles/precision_transport.h"
static int tracked_step(struct ub_pc_client *,uint64_t,int,uint32_t,uint64_t *);
#define ub_pc_client_step tracked_step
#include "../modules/external_subtitles/backend.c"
#undef ub_pc_client_step
#include <assert.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

static unsigned iterations,draws;
static uint32_t fake_active,fake_parser,fake_display,fake_clock=10;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static int32_t attrs[5];static unsigned char loaded;
struct idle_shared {unsigned calls;int paused;uint32_t generation;};
static struct idle_shared *shared;
static int event;
static int tracked_step(struct ub_pc_client *c,uint64_t nonce,int active,uint32_t coarse,uint64_t *ticks){
 __atomic_add_fetch(&iterations,1,__ATOMIC_RELEASE);
 return ub_pc_client_step(c,nonce,active,coarse,ticks);
}
static unsigned count(unsigned *p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
static int getter(uint32_t words[2]){
 __atomic_add_fetch(&shared->calls,1,__ATOMIC_RELEASE);words[0]=10*45000+12345;words[1]=0;return 0;
}
static int paused(uint32_t *generation){
 *generation=__atomic_load_n(&shared->generation,__ATOMIC_ACQUIRE);
 return __atomic_load_n(&shared->paused,__ATOMIC_ACQUIRE);
}
static void pause_event(int value){
 uint64_t one=1;__atomic_store_n(&shared->paused,value,__ATOMIC_RELEASE);
 __atomic_add_fetch(&shared->generation,1,__ATOMIC_RELEASE);assert(write(event,&one,sizeof one)==sizeof one);
}
static int native_update(uint32_t d,uint32_t seconds){(void)d;(void)seconds;return 0;}
static int native_draw(uint32_t seconds){(void)seconds;__atomic_add_fetch(&draws,1,__ATOMIC_RELEASE);return 0;}
static void seek(uint32_t value){(void)value;}
int UFS_OpenDir(const char *path,uint32_t *h){(void)path;(void)h;return -1;}
int UFS_ReadDir(uint32_t h,void *p,uint32_t *n){(void)h;(void)p;(void)n;return -1;}
int UFS_CloseDir(uint32_t h){(void)h;return 0;}
static void wait_more(unsigned *value,unsigned before){
 for(unsigned i=0;i<500&&count(value)<=before;i++)usleep(2000);
 assert(count(value)>before);
}
static void set_display(uint32_t value){
 gate_lock();fake_display=value;gate_unlock();
 /* Match a successful native display update, including the actual wake hook. */
 assert(!ub_hook_update(value,10));
}
static void assert_quiet(void){
 usleep(120000);unsigned loops=count(&iterations),calls=count(&shared->calls);
 usleep(120000);assert(count(&iterations)==loops);assert(count(&shared->calls)==calls);
}
int main(void){
 pthread_t worker_thread;pid_t server;int status;
 snprintf(idle_socket,sizeof idle_socket,"ub820.idle.integration.%u",(unsigned)getpid());
 shared=mmap(0,sizeof(*shared),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
 assert(shared!=MAP_FAILED);memset(shared,0,sizeof(*shared));event=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);assert(event>=0);
 server=fork();assert(server>=0);if(!server){ub_precision_source_serve(getter,paused,event);_exit(2);}
 struct ub_api a={0};a.active=&fake_active;a.parser=&fake_parser;a.display=&fake_display;a.clock=&fake_clock;
 a.attrs=attrs;a.loaded=&loaded;a.mutex=&mutex;a.seek=seek;
 ub_test_bind(&a);ub_test_reset();ub_original_draw=native_draw;ub_original_update=native_update;
 assert(!pthread_create(&worker_thread,0,precision_worker,0));wait_more(&iterations,0);assert_quiet();assert(!count(&shared->calls));
 gate_lock();fake_active=fake_parser=1;atomic_write(&session,1);precision_native_seen=1;assert(!ub_payload_now_ms(&precision_native_ms));gate_unlock();
 set_display(1);wait_more(&shared->calls,0);wait_more(&draws,0);
 pause_event(1);assert_quiet(); /* no heartbeat or periodic query needed */
 unsigned before=count(&shared->calls);pause_event(0);wait_more(&shared->calls,before); /* source event alone resumes */
 set_display(0);assert_quiet();
 /* Each native On wakes the indefinitely sleeping worker, without a 20ms
  * guard or reliance on the next whole-second Draw event. */
 for(unsigned i=0;i<4;i++){
  before=count(&shared->calls);set_display(1);wait_more(&shared->calls,before);
  set_display(0);assert_quiet();
 }
 assert(!pthread_cancel(worker_thread));pthread_join(worker_thread,0);
 close(ub_precision_wake_fd(&precision_wake));close(event);
 assert(!kill(server,SIGTERM));assert(waitpid(server,&status,0)==server);munmap(shared,sizeof(*shared));
 puts("precision_idle_integration_test passed: actual worker blocks on Off/pause and wakes on native/display events");return 0;
}
