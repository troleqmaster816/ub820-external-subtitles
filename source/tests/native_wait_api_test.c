#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
static char test_ready_library[1024];
#define UB_NATIVE_WAIT_DIR "/tmp/ub820-native-api-ready"
#define UB_NATIVE_WAIT_LIBRARY test_ready_library
static int fake_wait(uint32_t,void *);
static void *fake_dlsym(void *,const char *);
static _Thread_local uintptr_t test_caller;
#define __builtin_return_address(depth) ((void *)test_caller)
#define dlsym fake_dlsym
#include "../modules/native_events/wait.c"
#undef dlsym
#undef __builtin_return_address
static _Thread_local unsigned interruptions,calls,consumed,tag;
static _Thread_local int final_error,cancel_mode;
static atomic_uint cancel_calls,finish_wait;
static int resolve_failure;
static int fake_wait(uint32_t queue,void *out){
 calls++;
 if(cancel_mode&&!atomic_load(&finish_wait)){atomic_fetch_add(&cancel_calls,1);errno=EINTR;return EINTR;}
 if(interruptions){interruptions--;errno=EINTR;return EINTR;}
 if(final_error)return final_error;
 consumed++;*(uint32_t *)out=tag;errno=EINTR;(void)queue;return 0;
}
static void *fake_dlsym(void *scope,const char *name){assert(scope==RTLD_NEXT&&(!strcmp(name,"EV_wait_message")||!strcmp(name,"EV_wait_flag")));return resolve_failure==1?0:resolve_failure==2?(void *)native_wait_message:(void *)fake_wait;}
static void *run_wait(void *arg){
 uint32_t value=0;unsigned q=(unsigned)(uintptr_t)arg;
 interruptions=tag=q;calls=consumed=0;final_error=0;errno=EAGAIN;
 assert(!EV_wait_message(UB_NATIVE_DB_QUEUE,&value));assert(value==q&&calls==q+1&&consumed==1&&errno==EAGAIN);
 return 0;
}
static void *cancel_wait(void *arg){
 uint32_t value;int old;(void)arg;cancel_mode=1;
 assert(!EV_wait_message(UB_NATIVE_DB_QUEUE,&value));
 /* ioctl was never a cancellation point. The wrapper must not introduce one.
  * Disable the pending deferred cancellation before any later library call. */
 assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&old));return (void *)1;
}
int main(void){
 uint32_t out=0;pthread_t a,b;void *result;
 /* Exercise the actual publisher and verifier together on a native kernel.
  * No model of descriptor lifetime or atomic rename is substituted. */
 mkdir(UB_NATIVE_WAIT_DIR,0700);
 ssize_t len=readlink("/proc/self/exe",test_ready_library,sizeof test_ready_library-1);assert(len>0);test_ready_library[len]=0;
 assert(!native_wait_publish()&&ub_native_wait_ready((int)getpid()));
 assert(!native_wait_publish()&&ub_native_wait_ready((int)getpid()));
 close(native_ready_fd);native_ready_fd=-1;assert(!ub_native_wait_ready((int)getpid()));
 char ready_path[160];snprintf(ready_path,sizeof ready_path,UB_NATIVE_WAIT_DIR "/%d.ready",(int)getpid());unlink(ready_path);
 native_wait_enabled=1;
 run_wait((void *)3);final_error=2;interruptions=2;calls=consumed=0;
 assert(EV_wait_message(UB_NATIVE_DB_QUEUE,&out)==2&&calls==3&&!consumed);
 final_error=7;interruptions=0;calls=0;assert(EV_wait_message(UB_NATIVE_DB_QUEUE,&out)==7&&calls==1);
 /* A not-yet-enabled or unsupported executable preserves the original ABI. */
 native_wait_enabled=0;interruptions=1;calls=0;assert(EV_wait_message(UB_NATIVE_DB_QUEUE,&out)==4&&calls==1);
 resolve_failure=1;native_next_message=0;errno=0;assert(EV_wait_message(UB_NATIVE_DB_QUEUE,&out)==7&&errno==ENOSYS);
 resolve_failure=2;errno=0;assert(EV_wait_message(UB_NATIVE_DB_QUEUE,&out)==7&&errno==ENOSYS);
 resolve_failure=0;
 /* Exact Settings flag only. Destruction/errors stay visible. */
 native_wait_enabled=NATIVE_WAIT_SETTINGS;native_next_flag=0;
 final_error=0;interruptions=3;calls=consumed=0;errno=EAGAIN;
 assert(!EV_wait_flag(UB_NATIVE_SETTINGS_FLAG,&out)&&calls==4&&consumed==1&&errno==EAGAIN);
 interruptions=1;calls=0;assert(EV_wait_flag(UB_NATIVE_SETTINGS_FLAG+1,&out)==4&&calls==1);
 interruptions=2;calls=0;final_error=2;assert(EV_wait_flag(UB_NATIVE_SETTINGS_FLAG,&out)==2&&calls==3);
 final_error=7;interruptions=0;calls=0;assert(EV_wait_flag(UB_NATIVE_SETTINGS_FLAG,&out)==7&&calls==1);
 /* UFS queue IDs are dynamic: scope by a verified provider's four call sites,
  * including a relocated provider, and pass all other callers through. */
 native_wait_enabled=NATIVE_WAIT_UFS;native_ufs_base=0x31000000;
 const uintptr_t offsets[]={0x52be,0x1313c,0x5e81c,0x5f052};
 for(unsigned i=0;i<4;i++){
  test_caller=native_ufs_base+offsets[i]+1;final_error=0;interruptions=2;calls=consumed=0;
  assert(!EV_wait_message(0x90000009,&out)&&calls==3&&consumed==1);
 }
 test_caller=native_ufs_base+0x5e820;interruptions=2;calls=0;
 assert(EV_wait_message(0x90000009,&out)==4&&calls==1);
 assert(!native_ufs_wait_site(0x52be,0)&&!native_ufs_wait_site(8,16));
 test_caller=0;
 native_wait_enabled=1;
 interruptions=1;calls=0;assert(EV_wait_message(UB_NATIVE_DB_QUEUE+1,&out)==4&&calls==1);
 assert(!pthread_create(&a,0,run_wait,(void *)7));assert(!pthread_create(&b,0,run_wait,(void *)11));
 assert(!pthread_join(a,0)&&!pthread_join(b,0));
 native_wait_debug=1; /* Logging must not add a cancellation point either. */
 assert(!pthread_create(&a,0,cancel_wait,0));
 while(!atomic_load(&cancel_calls))usleep(100);
 assert(!pthread_cancel(a));atomic_store(&finish_wait,1);assert(!pthread_join(a,&result)&&result==(void *)1);
 puts("native_wait_api_test passed: scoped DB/Settings/UFS retry, error/errno ABI, one delivery, concurrent callers, live CLOEXEC receipt; no new cancellation point");return 0;
}
