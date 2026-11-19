/* Ordinary, indefinite native API retry. The kernel may consume the request
 * type before returning EINTR; calling libev again constructs a fresh request.
 * Never replay an ioctl or alter target registers/packets here. */
#include "native_wait.h"
#include <dlfcn.h>
#include <sys/syscall.h>
#include "../region/bd_sha256.h"
#include "wait_scope.h"
#ifndef UB_NATIVE_UFS_HASH_182
#define UB_NATIVE_UFS_HASH_182 "52bc9da9a053ee61ce72d36fba2c3c85d542f2b3b387a8763f547b4d53a25ae1"
#endif
extern long syscall(long,...);
#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1L)
#endif
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)0)
#endif
#ifdef __USE_GNU
typedef Dl_info native_dl_info;
#else
typedef struct {const char *dli_fname;void *dli_fbase;const char *dli_sname;void *dli_saddr;} native_dl_info;
extern int dladdr(const void *,native_dl_info *);
#endif

typedef int (*native_wait_fn)(uint32_t,void *);
static native_wait_fn native_next_message;
static native_wait_fn native_next_flag;
static uintptr_t native_ufs_base;
static int native_wait_enabled,native_wait_debug;
static int native_wait_message(uint32_t,void *);
static int native_wait_flag(uint32_t,void *);
int EV_wait_message(uint32_t,void *) __attribute__((alias("native_wait_message")));
int EV_wait_flag(uint32_t,void *) __attribute__((alias("native_wait_flag")));
static native_wait_fn native_wait_resolve(void){
 native_wait_fn fn=__atomic_load_n(&native_next_message,__ATOMIC_ACQUIRE);
 if(!fn){
  fn=(native_wait_fn)dlsym(RTLD_NEXT,"EV_wait_message");
  if(!fn||fn==native_wait_message)return 0;
  __atomic_store_n(&native_next_message,fn,__ATOMIC_RELEASE);
 }
 return fn;
}
static void native_wait_log(uint32_t queue){
 char line[160];int saved=errno,fd,n;
 n=snprintf(line,sizeof line,"native_events: EV_wait_message PID%d TID%ld queue=%08x: EINTR; repeating public wait API\n",(int)getpid(),syscall(SYS_gettid),queue);
 /* libc open/write/close are cancellation points. Diagnostic I/O must not
  * turn this otherwise non-cancellable native wait into a new one. */
 fd=(int)syscall(SYS_openat,AT_FDCWD,"/tmp/ub820_submenu.log",O_WRONLY|O_APPEND|O_CLOEXEC|O_NONBLOCK,0);
 if(fd>=0){if(n>0&&n<(int)sizeof line){long r;do{r=syscall(SYS_write,fd,line,(size_t)n);}while(r<0&&errno==EINTR);}syscall(SYS_close,fd);}
 errno=saved;
}
static int native_message_scope(uint32_t queue,uintptr_t caller){
 int kind=__atomic_load_n(&native_wait_enabled,__ATOMIC_ACQUIRE);
 return (kind==NATIVE_WAIT_DB&&queue==UB_NATIVE_DB_QUEUE)||
        (kind==NATIVE_WAIT_UFS&&native_ufs_wait_site(caller,native_ufs_base));
}
__attribute__((noinline)) static int native_wait_message(uint32_t queue,void *message){
 int rc,saved=errno;native_wait_fn next=native_wait_resolve();
 if(!next){errno=ENOSYS;return 7;}
 /* BD keeps the existing setting-DB-only policy. Playback children retry only
  * at the four audited libufs sites after full provider verification. Every
  * other caller keeps its caught-signal semantics. Destruction returns 2;
  * ordinary shutdown is a message. This API is not a cancellation point. */
 if(!native_message_scope(queue,(uintptr_t)__builtin_return_address(0))){errno=saved;return next(queue,message);}
 do {
  errno=saved;rc=next(queue,message);
  if(rc!=EINTR){if(!rc)errno=saved;return rc;}
  if(native_wait_debug)native_wait_log(queue);
  /* No cancellation point, finite timeout or arbitrary delay is introduced.
   * The public API reconstructs its own request on the next call. */
 }while(1);
}
__attribute__((noinline)) static int native_wait_flag(uint32_t id,void *out){
 int saved=errno,rc;native_wait_fn next=__atomic_load_n(&native_next_flag,__ATOMIC_ACQUIRE);
 if(!next){
  next=(native_wait_fn)dlsym(RTLD_NEXT,"EV_wait_flag");
  if(!next||next==native_wait_flag){errno=ENOSYS;return 7;}
  __atomic_store_n(&native_next_flag,next,__ATOMIC_RELEASE);
 }
 if(__atomic_load_n(&native_wait_enabled,__ATOMIC_ACQUIRE)!=NATIVE_WAIT_SETTINGS||id!=UB_NATIVE_SETTINGS_FLAG){errno=saved;return next(id,out);}
 do{errno=saved;rc=next(id,out);}while(rc==EINTR);
 if(!rc)errno=saved;
 return rc;
}
static int native_ready_fd=-1;
static int native_wait_publish(void){
 char stat[4096],path[160],temp[180],line[128];unsigned long long born;
 int fd,n,rc=-1;ssize_t wrote;
 if(ub_native_wait_read("/proc/self/stat",stat,sizeof stat)<0||!(born=ub_native_wait_birth(stat)))return -1;
 snprintf(path,sizeof path,UB_NATIVE_WAIT_DIR "/%d.ready",(int)getpid());
 snprintf(temp,sizeof temp,"%s.tmp",path);
 if(native_ready_fd>=0){close(native_ready_fd);native_ready_fd=-1;}
 fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);if(fd<0)return -1;
 n=snprintf(line,sizeof line,"EVW2 %d %llu %d\n",(int)getpid(),born,fd);
 do{wrote=write(fd,line,(size_t)n);}while(wrote<0&&errno==EINTR);
 if(wrote==n)rc=0;
 if(!rc&&rename(temp,path))rc=-1;
 if(rc){close(fd);unlink(temp);}else native_ready_fd=fd;
 return rc;
}
static int native_provider(const void *symbol,const char *first,const char *second,uintptr_t *base){
 native_dl_info info;
 struct hash h;unsigned char buf[8192];char digest[65];int fd;ssize_t n;
 if(!dladdr(symbol,&info)||!info.dli_fname)return -3;
 fd=open(info.dli_fname,O_RDONLY|O_CLOEXEC);if(fd<0)return -4;
 hash_init(&h);
 for(;;){n=read(fd,buf,sizeof buf);if(n<0&&errno==EINTR)continue;if(n<=0)break;hash_feed(&h,buf,(size_t)n);}
 close(fd);if(n<0)return -4;hash_end(&h,digest);
 if(strcmp(digest,first)&&strcmp(digest,second))return -5;
 if(base)*base=(uintptr_t)info.dli_fbase;
 return 0;
}
static int native_wait_enable_kind(int kind){
 native_wait_fn next=native_wait_resolve();
 int rc;
 /* A user preload ahead of us may own this symbol. Do not attest a handler
  * that the executable would bypass. The local alias cannot be interposed. */
 if(!next)return -1;
 if(dlsym(RTLD_DEFAULT,"EV_wait_message")!=(void *)native_wait_message)return -2;
 /* .text is byte-identical in these two verified library cohorts. A provider
  * outside them has no verified non-consumption/return-ABI contract here. */
 rc=native_provider((const void *)next,"e5391f9728f1b6e3e30f61e6b1672eef9f43ee692694b8b6b513feaf23969c8d",
    "d48123aecb85995a14cb762b7e1ba6e43a6ae49f973ebd5b6307648f3584844d",0);
 if(rc)return rc;
 if(kind==NATIVE_WAIT_SETTINGS){
  native_wait_fn flag=(native_wait_fn)dlsym(RTLD_NEXT,"EV_wait_flag");
  if(!flag||flag==native_wait_flag||dlsym(RTLD_DEFAULT,"EV_wait_flag")!=(void *)native_wait_flag)return -2;
  rc=native_provider((const void *)flag,"e5391f9728f1b6e3e30f61e6b1672eef9f43ee692694b8b6b513feaf23969c8d",
     "d48123aecb85995a14cb762b7e1ba6e43a6ae49f973ebd5b6307648f3584844d",0);
  if(rc)return rc;
  __atomic_store_n(&native_next_flag,flag,__ATOMIC_RELEASE);
 }else if(kind==NATIVE_WAIT_UFS){
  void *ufs=dlsym(RTLD_DEFAULT,"UFS_Open");
  if(!ufs)return -7;
  rc=native_provider(ufs,UB_NATIVE_UFS_HASH_182,
     "c79b3d230b5107de7c6cdba9737e1192b2ddc258e0b3162cb638af68b9582b5c",&native_ufs_base);
  if(rc||!native_ufs_base)return -7;
 }else if(kind!=NATIVE_WAIT_DB)return -8;
 native_wait_debug=getenv("EV_DEBUG")!=0;
 __atomic_store_n(&native_wait_enabled,kind,__ATOMIC_RELEASE);
 return native_wait_publish()?-6:0;
}
