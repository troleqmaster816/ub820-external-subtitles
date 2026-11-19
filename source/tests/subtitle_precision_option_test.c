/* precision=0: the supervisor's marker keeps the real backend clock worker
 * from running its client loop; native Draw stays on the stock seconds path.
 * Without the marker the same worker loops while subtitles are visible. */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
#define UB_PC_SOURCE_PEER "subtitle_precision_option_test"
#define UB_PC_CLIENT_PEER "subtitle_precision_option_test"
static char option_socket[80],marker[96];
#define UB_PC_SOCKET_NAME option_socket
#define UB_PC_DISABLED_FILE marker
#include "../modules/external_subtitles/precision_transport.h"
static int tracked_step(struct ub_pc_client *,uint64_t,int,uint32_t,uint64_t *);
#define ub_pc_client_step tracked_step
#include "../modules/external_subtitles/backend.c"
#undef ub_pc_client_step
#include <assert.h>
#include <time.h>

static unsigned iterations,draws;
static uint32_t fake_active=1,fake_parser=1,fake_display=1,fake_clock=10;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static int32_t attrs[5];static unsigned char loaded=1;
static int tracked_step(struct ub_pc_client *c,uint64_t nonce,int active,uint32_t coarse,uint64_t *ticks){
 __atomic_add_fetch(&iterations,1,__ATOMIC_RELEASE);return ub_pc_client_step(c,nonce,active,coarse,ticks);
}
static int native_draw(uint32_t seconds){(void)seconds;__atomic_add_fetch(&draws,1,__ATOMIC_RELEASE);return 0;}
static int native_update(uint32_t d,uint32_t s){(void)d;(void)s;return 0;}
static void seek(uint32_t value){(void)value;}
int UFS_OpenDir(const char *path,uint32_t *h){(void)path;(void)h;return -1;}
int UFS_ReadDir(uint32_t h,void *p,uint32_t *n){(void)h;(void)p;(void)n;return -1;}
int UFS_CloseDir(uint32_t h){(void)h;return 0;}
static unsigned count(unsigned *p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
static void setup(void){
 struct ub_api a={0};a.active=&fake_active;a.parser=&fake_parser;a.display=&fake_display;a.clock=&fake_clock;
 a.attrs=attrs;a.loaded=&loaded;a.mutex=&mutex;a.seek=seek;
 ub_test_bind(&a);ub_test_reset();ub_original_draw=native_draw;ub_original_update=native_update;
 atomic_write(&session,1);__atomic_store_n(&iterations,0,__ATOMIC_RELEASE);
}
int main(void){
 pthread_t thread;struct timespec deadline;unsigned cases=0;
 snprintf(option_socket,sizeof option_socket,"ub820.option.missing.%u",(unsigned)getpid());
 snprintf(marker,sizeof marker,"/tmp/ub820-precision-off-%u",(unsigned)getpid());
 /* Marker present: the worker returns at once, with no client step. */
 FILE *f=fopen(marker,"w");assert(f);assert(!fclose(f));setup();
 assert(!pthread_create(&thread,0,precision_worker,0));
 clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=2;
 int joined=pthread_timedjoin_np(thread,0,&deadline);
 if(joined){pthread_cancel(thread);pthread_join(thread,0);}
 assert(!joined&&!count(&iterations));cases++;
 /* Native Draw then uses the original seconds API, with no fractional state. */
 assert(!ub_hook_draw(10)&&count(&draws)==1&&!precision_valid&&!precision_drawn);cases++;
 /* Marker absent (precision=1): the same worker runs its client loop. */
 unlink(marker);setup();
 assert(!pthread_create(&thread,0,precision_worker,0));
 for(unsigned i=0;i<500&&count(&iterations)<3;i++)usleep(2000);
 assert(count(&iterations)>=3);cases++;
 assert(!pthread_cancel(thread));assert(!pthread_join(thread,0));
 printf("subtitle_precision_option_test passed: %u cases; precision=0 starts no clock client loop, stock Draw kept\n",cases);
 return 0;
}
