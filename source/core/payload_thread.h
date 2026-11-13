#ifndef UB_PAYLOAD_THREAD_H
#define UB_PAYLOAD_THREAD_H

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>

/* A payload may bootstrap in a native real-time callback. Its background
 * worker must not inherit that policy or become a recipient of asynchronous
 * native process signals. Synchronous fault signals retain the creator's
 * mask so the native crash handlers can still run. Attribute defaults retain
 * the platform's normal stack size and guard page; no guessed small stack is
 * imposed here.
 *
 * A zero result means a thread exists. Cleanup failure after successful
 * creation must not report failure and cause the caller to spawn a duplicate.
 * With valid local attributes/masks, destroy/restore have no expected failure.
 * SIGKILL/SIGSTOP and libc's reserved threading signals remain unmaskable.
 */
static inline int ub_payload_thread_start(pthread_t *thread,
                                         void *(*entry)(void *),void *arg){
 pthread_attr_t attr;struct sched_param sched={0};sigset_t all,previous;
 int rc=pthread_attr_init(&attr);
 if(rc)return rc;
 rc=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
 if(!rc)rc=pthread_attr_setschedpolicy(&attr,SCHED_OTHER);
 if(!rc)rc=pthread_attr_setschedparam(&attr,&sched);
 if(!rc && sigfillset(&all))rc=errno;
 if(!rc){
  sigdelset(&all,SIGSEGV);sigdelset(&all,SIGBUS);
  sigdelset(&all,SIGILL);sigdelset(&all,SIGFPE);
  sigdelset(&all,SIGTRAP);sigdelset(&all,SIGABRT);
 }
 if(!rc){
  rc=pthread_sigmask(SIG_BLOCK,&all,&previous);
  if(!rc){
   rc=pthread_create(thread,&attr,entry,arg);
   (void)pthread_sigmask(SIG_SETMASK,&previous,0);
  }
 }
 (void)pthread_attr_destroy(&attr);
 return rc;
}

#endif
