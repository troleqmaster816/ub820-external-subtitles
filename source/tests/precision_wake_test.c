#define _GNU_SOURCE
#include "../modules/external_subtitles/precision_wake.h"
#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static struct ub_precision_wake wake={-1,0};
static void *delayed_wake(void *unused){(void)unused;usleep(20000);ub_precision_wake_signal(&wake);return 0;}
int main(void){
 pthread_t thread;struct pollfd p;uint32_t generation;
 errno=ENOENT;ub_precision_wake_signal(&wake);assert(errno==ENOENT);
 assert(ub_precision_wake_generation(&wake)==1 && ub_precision_wake_fd(&wake)==-1);
 ub_precision_wake_init(&wake);assert(ub_precision_wake_fd(&wake)>=0);
 p=(struct pollfd){ub_precision_wake_fd(&wake),POLLIN,0};
 assert(poll(&p,1,0)==0);
 generation=ub_precision_wake_generation(&wake);
 /* Event between eligibility check and poll remains readable. */
 ub_precision_wake_signal(&wake);assert(ub_precision_wake_generation(&wake)!=generation);
 assert(poll(&p,1,0)==1);ub_precision_wake_drain(&wake);assert(poll(&p,1,0)==0);
 /* Coalesced events retain generation changes without queued-byte storms. */
 generation=ub_precision_wake_generation(&wake);
 for(unsigned i=0;i<10000;i++)ub_precision_wake_signal(&wake);
 assert(ub_precision_wake_generation(&wake)-generation==10000);
 ub_precision_wake_drain(&wake);assert(poll(&p,1,0)==0);
 assert(!pthread_create(&thread,0,delayed_wake,0));assert(poll(&p,1,1000)==1);pthread_join(thread,0);
 ub_precision_wake_drain(&wake);assert(poll(&p,1,0)==0);
 /* No periodic wake or CPU work while inactive: the kernel wait times out. */
 struct timespec begin,end;clock_gettime(CLOCK_MONOTONIC,&begin);assert(poll(&p,1,80)==0);clock_gettime(CLOCK_MONOTONIC,&end);
 assert((end.tv_sec-begin.tv_sec)*1000000000LL+end.tv_nsec-begin.tv_nsec>=70000000);
 close(ub_precision_wake_fd(&wake));
 puts("precision_wake_test passed: lost-wake window, coalescing, preserved errno and idle blocking");return 0;
}
