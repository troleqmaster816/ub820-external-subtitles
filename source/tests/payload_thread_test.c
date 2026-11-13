/* Inspect the real pthread attributes and signal mask at a stubbed create.
 * Inject setup/create failures without starting threads or requiring RT caps. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static int test_attr_init(pthread_attr_t *);
static int test_attr_destroy(pthread_attr_t *);
static int test_inherit(pthread_attr_t *,int);
static int test_policy(pthread_attr_t *,int);
static int test_param(pthread_attr_t *,const struct sched_param *);
static int test_mask(int,const sigset_t *,sigset_t *);
static int test_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
#define pthread_attr_init test_attr_init
#define pthread_attr_destroy test_attr_destroy
#define pthread_attr_setinheritsched test_inherit
#define pthread_attr_setschedpolicy test_policy
#define pthread_attr_setschedparam test_param
#define pthread_sigmask test_mask
#define pthread_create test_create
#include "../core/payload_thread.h"
#undef pthread_attr_init
#undef pthread_attr_destroy
#undef pthread_attr_setinheritsched
#undef pthread_attr_setschedpolicy
#undef pthread_attr_setschedparam
#undef pthread_sigmask
#undef pthread_create

static int failures,fail_step,destroys,creates,blocks,restores;
static size_t default_stack,default_guard;
#define CHECK(c) do{if(!(c)){printf("FAIL payload_thread_test:%d: %s\n",__LINE__,#c);failures++;}}while(0)
static int test_attr_init(pthread_attr_t *a){return fail_step==1?EAGAIN:pthread_attr_init(a);}
static int test_attr_destroy(pthread_attr_t *a){destroys++;return pthread_attr_destroy(a);}
static int test_inherit(pthread_attr_t *a,int v){return fail_step==2?EAGAIN:pthread_attr_setinheritsched(a,v);}
static int test_policy(pthread_attr_t *a,int v){return fail_step==3?EAGAIN:pthread_attr_setschedpolicy(a,v);}
static int test_param(pthread_attr_t *a,const struct sched_param *v){return fail_step==4?EAGAIN:pthread_attr_setschedparam(a,v);}
static int test_mask(int how,const sigset_t *set,sigset_t *old){
 if(how==SIG_BLOCK){blocks++;if(fail_step==5)return EAGAIN;}
 if(how==SIG_SETMASK)restores++;
 return pthread_sigmask(how,set,old);
}
static void *entry(void *arg){return arg;}
static int test_create(pthread_t *thread,const pthread_attr_t *attr,void *(*fn)(void *),void *arg){
 int inherit,policy;struct sched_param sched;size_t stack,guard;sigset_t mask;
 creates++;CHECK(attr&&fn==entry&&arg==(void *)42);
 CHECK(!pthread_attr_getinheritsched(attr,&inherit)&&inherit==PTHREAD_EXPLICIT_SCHED);
 CHECK(!pthread_attr_getschedpolicy(attr,&policy)&&policy==SCHED_OTHER);
 CHECK(!pthread_attr_getschedparam(attr,&sched)&&sched.sched_priority==0);
 CHECK(!pthread_attr_getstacksize(attr,&stack)&&stack==default_stack);
 CHECK(!pthread_attr_getguardsize(attr,&guard)&&guard==default_guard);
 CHECK(!pthread_sigmask(SIG_SETMASK,0,&mask));
 CHECK(sigismember(&mask,SIGUSR1)==1&&sigismember(&mask,SIGUSR2)==1&&sigismember(&mask,SIGTERM)==1);
 CHECK(!sigismember(&mask,SIGSEGV)&&!sigismember(&mask,SIGBUS));
 CHECK(!sigismember(&mask,SIGILL)&&!sigismember(&mask,SIGFPE));
 CHECK(!sigismember(&mask,SIGTRAP)&&!sigismember(&mask,SIGABRT));
 for(int s=1;s<NSIG;s++){
  sigset_t one;sigemptyset(&one);
  if(s==SIGKILL||s==SIGSTOP||s==SIGSEGV||s==SIGBUS||s==SIGILL||s==SIGFPE||s==SIGTRAP||s==SIGABRT)continue;
  /* libc excludes its reserved threading signals from sigaddset. */
  if(!sigaddset(&one,s))CHECK(sigismember(&mask,s)==1);
 }
 if(fail_step==6)return EAGAIN;
 *thread=123;return 0;
}
int main(void){
 pthread_attr_t defaults;sigset_t original,chosen,after;pthread_t thread;
 CHECK(!pthread_attr_init(&defaults));
 CHECK(!pthread_attr_getstacksize(&defaults,&default_stack));
 CHECK(!pthread_attr_getguardsize(&defaults,&default_guard));
 CHECK(!pthread_attr_destroy(&defaults));
 sigemptyset(&chosen);sigaddset(&chosen,SIGUSR1);
 CHECK(!pthread_sigmask(SIG_SETMASK,&chosen,&original));
 for(fail_step=0;fail_step<=6;fail_step++){
  int rc;destroys=creates=blocks=restores=0;thread=0;
  rc=ub_payload_thread_start(&thread,entry,(void *)42);
  CHECK(rc==(fail_step?EAGAIN:0));CHECK(thread==(pthread_t)(fail_step?0:123));
  CHECK(destroys==(fail_step==1?0:1));
  CHECK(creates==(!fail_step||fail_step==6));
  CHECK(blocks==(!fail_step||fail_step>=5));
  CHECK(restores==(!fail_step||fail_step==6));
  CHECK(!pthread_sigmask(SIG_SETMASK,0,&after));
  for(int s=1;s<NSIG;s++)CHECK(sigismember(&after,s)==sigismember(&chosen,s));
 }
 CHECK(!pthread_sigmask(SIG_SETMASK,&original,0));
 printf("payload_thread_test %s\n",failures?"FAILED":"passed");return failures?1:0;
}
