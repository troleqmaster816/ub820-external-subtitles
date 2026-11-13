/* Native-clock adapter tests. No live target APIs, threads or sockets. */
#ifndef UB_HOST_TEST
#define UB_HOST_TEST 1
#endif
#define UB_PRECISION_SOURCE_TEST
#define pthread_create precision_test_pthread_create
#define pthread_detach precision_test_pthread_detach
#define eventfd precision_test_eventfd
#define write precision_test_write
#include "../modules/external_subtitles/precision_source.c"
#undef pthread_create
#undef pthread_detach
#undef eventfd
#undef write
#include <stdio.h>

static int failures,get_calls,get_rc,change_id,change_state,original_calls,original_active;
static int create_calls,create_failure,detach_calls,serve_calls;
static int eventfd_calls,eventfd_failure,event_writes,event_eintr,event_eagain,notify_calls;
static uint32_t mm_id=0x1234,mm_state=3,pce_state=6;
static void *(*created_entry)(void *);
static void *created_arg;
#define CHECK(c) do{if(!(c)){printf("FAIL subtitle_precision_source_test:%d: %s\n",__LINE__,#c);failures++;}}while(0)

static int native_get(uint32_t id,uint32_t *out){
 get_calls++;CHECK(!original_active&&id==0x1234);
 out[0]=0x11223344;out[1]=0x55667788; /* Writes even on error, like stock. */
 if(change_id)mm_id=0x5678;
 if(change_state)mm_state=(uint32_t)change_state;
 return get_rc;
}
static void original_callback(const uint32_t *info){
 original_active=1;original_calls++;CHECK(info&&info[0]==77);original_active=0;
}
int precision_test_pthread_create(pthread_t *thread,const pthread_attr_t *attr,void *(*fn)(void *),void *arg){
 int inherit,policy;struct sched_param sched;sigset_t mask;
 create_calls++;CHECK(attr);
 CHECK(!pthread_attr_getinheritsched(attr,&inherit)&&inherit==PTHREAD_EXPLICIT_SCHED);
 CHECK(!pthread_attr_getschedpolicy(attr,&policy)&&policy==SCHED_OTHER);
 CHECK(!pthread_attr_getschedparam(attr,&sched)&&sched.sched_priority==0);
 CHECK(!pthread_sigmask(SIG_SETMASK,0,&mask)&&sigismember(&mask,SIGUSR1)==1&&sigismember(&mask,SIGUSR2)==1);
 CHECK(!sigismember(&mask,SIGSEGV)&&!sigismember(&mask,SIGBUS));
 CHECK(!sigismember(&mask,SIGILL)&&!sigismember(&mask,SIGFPE));
 CHECK(!sigismember(&mask,SIGTRAP)&&!sigismember(&mask,SIGABRT));
 /* This stub saves the entry for explicit execution AFTER the native hook
  * returns; it cannot accidentally run a synchronous query in the callback. */
 CHECK(!original_active);
 if(create_failure)return 11;
 *thread=123;created_entry=fn;created_arg=arg;return 0;
}
int precision_test_pthread_detach(pthread_t thread){CHECK(thread==123);detach_calls++;return 0;}
int precision_test_eventfd(unsigned int value,int flags){
 eventfd_calls++;CHECK(value==0&&flags==(EFD_NONBLOCK|EFD_CLOEXEC));
 if(eventfd_failure){errno=EMFILE;return -1;}return 1234;
}
ssize_t precision_test_write(int fd,const void *p,size_t size){
 const uint64_t *one=p;event_writes++;CHECK(fd==1234&&size==8&&*one==1);
 if(event_eintr){event_eintr=0;errno=EINTR;return -1;}
 if(event_eagain){errno=EAGAIN;return -1;}
 return (ssize_t)size;
}
static void original_notify(const uint32_t *info){
 notify_calls++;CHECK(info&&info[0]==2&&info[4]==1);errno=EDOM;
}
void ub_precision_source_serve(int (*get)(uint32_t[2]),int (*paused)(uint32_t *),int fd){
 uint32_t generation=0;CHECK(paused&&paused(&generation)==0&&generation!=0);
 CHECK(fd==(eventfd_failure?-1:1234));
 uint32_t out[2]={0,0};serve_calls++;CHECK(!original_active);CHECK(!get(out));
 CHECK(out[0]==0x11223344&&out[1]==0x55667788);
}
static void reset_get(void){
 mm_id=0x1234;mm_state=3;pce_state=6;get_calls=get_rc=change_id=change_state=0;
 ub_native_get_media_time=native_get;ub_precision_mm_id=&mm_id;ub_precision_mm_state=&mm_state;
 ub_precision_pce_state=&pce_state;
}
static void unchanged(const uint32_t *v){CHECK(v[0]==0xaaaaaaaau&&v[1]==0xbbbbbbbbu);}
int main(void){
 uint32_t out[2],info[6]={77,0,0,0,0,0};
 reset_get();ub_original_clock_callback=original_callback;
 out[0]=0;out[1]=0;CHECK(!precision_native_time(out));CHECK(get_calls==1&&out[0]==0x11223344&&out[1]==0x55667788);
 for(unsigned state=0;state<=15;state++){
  int admissible=state>=2&&state<=11&&state!=9&&state!=11;
  reset_get();mm_state=state;out[0]=0xaaaaaaaa;out[1]=0xbbbbbbbb;
  CHECK((precision_native_time(out)==0)==admissible);CHECK(get_calls==admissible);
  if(!admissible)unchanged(out);
 }
 reset_get();mm_id=0;out[0]=0xaaaaaaaa;out[1]=0xbbbbbbbb;CHECK(precision_native_time(out)<0&&!get_calls);unchanged(out);
 reset_get();ub_native_get_media_time=0;CHECK(precision_native_time(out)<0&&!get_calls);unchanged(out);
 reset_get();ub_precision_mm_id=0;CHECK(precision_native_time(out)<0&&!get_calls);unchanged(out);
 reset_get();ub_precision_mm_state=0;CHECK(precision_native_time(out)<0&&!get_calls);unchanged(out);
 reset_get();CHECK(precision_native_time(0)<0&&!get_calls);
 for(int rc=-1;rc<=1;rc+=2){reset_get();get_rc=rc;out[0]=0xaaaaaaaa;out[1]=0xbbbbbbbb;CHECK(precision_native_time(out)<0&&get_calls==1);unchanged(out);}
 reset_get();change_id=1;out[0]=0xaaaaaaaa;out[1]=0xbbbbbbbb;CHECK(precision_native_time(out)<0&&get_calls==1);unchanged(out);
 for(int state=9;state<=11;state+=2){reset_get();change_state=state;CHECK(precision_native_time(out)<0);unchanged(out);}
 reset_get();change_state=3;CHECK(!precision_native_time(out)); /* A valid state transition is not a fabricated clock error. */
 /* Original native behavior always runs, even while the precision source is
  * not ready. Creation failure retries on a later natural invocation. */
 reset_get();mm_state=9;source_started=0;ub_hook_clock_callback(info);CHECK(original_calls==1&&!create_calls&&!get_calls&&!source_started);
 reset_get();create_failure=1;ub_hook_clock_callback(info);CHECK(original_calls==2&&create_calls==1&&!source_started&&!get_calls);
 create_failure=0;ub_hook_clock_callback(info);CHECK(original_calls==3&&create_calls==2&&source_started==1&&detach_calls==1&&!get_calls);
 ub_hook_clock_callback(info);CHECK(original_calls==4&&create_calls==2&&!get_calls);
 CHECK(created_entry);if(created_entry)CHECK(!created_entry(created_arg));
 CHECK(source_started==2&&serve_calls==1&&get_calls==1);
 ub_hook_clock_callback(info);CHECK(original_calls==5&&create_calls==2&&serve_calls==1&&get_calls==1);
 CHECK(info[0]==77&&info[1]==0); /* Borrowed callback storage was not changed. */
 /* The source uses the exact native pause predicate. STILL and transition
  * states must not be silently treated as pause; no clock query is needed. */
 {
  uint32_t generation=0,old_generation;int before=get_calls;
  for(uint32_t state=0;state<=0x24;state++){
   pce_state=state;CHECK(precision_native_paused(&generation)==(state==7));
  }
  CHECK(get_calls==before);pce_state=7;mm_state=9;
  CHECK(precision_native_paused(&generation)==-1);mm_state=2;
  CHECK(precision_native_paused(&generation)==-1);mm_state=3;
  ub_precision_pce_state=0;CHECK(precision_native_paused(&generation)==-1);
  ub_precision_pce_state=&pce_state;CHECK(precision_native_paused(0)==-1);
  /* A native pause/resume/title notification only wakes. Original behavior
   * is preserved first, including errno, and borrowed words stay intact. */
  uint32_t notification[5]={2,0,0,0,1};int writes=event_writes;
  ub_original_notify_callback=original_notify;old_generation=source_generation;
  event_eintr=1;errno=0;ub_hook_notify_callback(notification);
  CHECK(notify_calls==1&&errno==EDOM&&event_writes==writes+2);
  CHECK(source_generation==old_generation+1&&get_calls==before);
  CHECK(notification[0]==2&&notification[4]==1);
  ub_original_notify_callback=0;notification[0]=0;old_generation=source_generation;
  errno=ERANGE;ub_hook_notify_callback(notification);
  CHECK(source_generation==old_generation&&errno==ERANGE);
  notification[0]=0x1000000;event_eagain=1;ub_hook_notify_callback(notification);
  CHECK(source_generation==old_generation+1&&errno==ERANGE&&get_calls==before);
  event_eagain=0;
  /* An unchanged EXT position never causes a paused periodic wake; an exact
   * changed position (including high word) wakes, without becoming a sample. */
  old_generation=source_generation;writes=event_writes;ub_hook_clock_callback(info);
  CHECK(source_generation==old_generation&&event_writes==writes);
  info[1]=1;ub_hook_clock_callback(info);
  CHECK(source_generation==old_generation+1&&event_writes==writes+1&&get_calls==before);
  /* Missing eventfd is handed to transport explicitly: it must keep polling.
   * The worker never publishes or closes an invalid descriptor. */
  pce_state=6;source_event_fd=-1;eventfd_failure=1;
  CHECK(!precision_source_thread(0));CHECK(eventfd_calls==2&&source_event_fd==-1);
 }
 printf("subtitle_precision_source_test %s\n",failures?"FAILED":"passed");return failures?1:0;
}
