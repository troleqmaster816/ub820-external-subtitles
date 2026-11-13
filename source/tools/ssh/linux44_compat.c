/* Static musl ARM tools on the UB820's ARM64 Linux 4.4 compat ABI.
 * Keep musl's public time_t (64-bit) while using existing time32 syscalls.
 * Newer syscall probes are noisy on this vendor kernel. No firmware writes.
 * Include/link this file once before libc; no dynamic interposition needed. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__arm__) && __SIZEOF_LONG__ == 4
struct ub_time32 { int32_t sec,nsec; };
int __clock_gettime(clockid_t,struct timespec *);
int __clock_settime64(clockid_t,const struct timespec *);
int __clock_nanosleep_time64(clockid_t,int,const struct timespec *,struct timespec *);
int __nanosleep_time64(const struct timespec *,struct timespec *);
static int ub_time32_from(struct ub_time32 *out,const struct timespec *in){
 if(in->tv_sec < INT32_MIN || in->tv_sec > INT32_MAX){errno=EOVERFLOW;return -1;}
 out->sec=(int32_t)in->tv_sec;out->nsec=(int32_t)in->tv_nsec;return 0;
}
int __clock_gettime(clockid_t clock,struct timespec *out){
 struct ub_time32 t;int rc=(int)syscall(263,clock,&t);
 if(!rc){out->tv_sec=t.sec;out->tv_nsec=t.nsec;}return rc;
}
int __clock_gettime64(clockid_t,struct timespec *) __attribute__((alias("__clock_gettime")));
int __clock_settime64(clockid_t clock,const struct timespec *in){
 struct ub_time32 t;if(ub_time32_from(&t,in))return -1;return (int)syscall(262,clock,&t);
}
int __clock_settime(clockid_t,const struct timespec *) __attribute__((alias("__clock_settime64")));
int __clock_nanosleep_time64(clockid_t clock,int flags,const struct timespec *in,struct timespec *out){
 struct ub_time32 req,rem;int rc,saved=errno;
 if(ub_time32_from(&req,in)){rc=errno;errno=saved;return rc;}
 rc=(int)syscall(265,clock,flags,&req,&rem);rc=rc<0?errno:0;
 if(rc==EINTR && out && !(flags&TIMER_ABSTIME)){out->tv_sec=rem.sec;out->tv_nsec=rem.nsec;}
 errno=saved;return rc;
}
int __clock_nanosleep(clockid_t,int,const struct timespec *,struct timespec *) __attribute__((alias("__clock_nanosleep_time64")));
int __nanosleep_time64(const struct timespec *in,struct timespec *out){
 int rc=__clock_nanosleep_time64(CLOCK_REALTIME,0,in,out);if(rc){errno=rc;return -1;}return 0;
}
/* The first 104 bytes of musl ARM struct stat retain the kernel stat64
 * layout; musl appends time64 values. Populate those instead of statx(397). */
_Static_assert(offsetof(struct stat,st_ino)==96,"ARM stat64 inode ABI");
_Static_assert(offsetof(struct stat,st_atim)==104,"musl time64 stat ABI");
static int ub_stat_result(long rc,const unsigned char raw[104],struct stat *out){
 int32_t sec;uint32_t ns;
 if(rc)return -1;
 memset(out,0,sizeof(*out));memcpy(out,raw,104);
 memcpy(&sec,raw+72,4);memcpy(&ns,raw+76,4);out->st_atim=(struct timespec){sec,ns};
 memcpy(&sec,raw+80,4);memcpy(&ns,raw+84,4);out->st_mtim=(struct timespec){sec,ns};
 memcpy(&sec,raw+88,4);memcpy(&ns,raw+92,4);out->st_ctim=(struct timespec){sec,ns};
 return 0;
}
int stat(const char *path,struct stat *out){unsigned char k[104] __attribute__((aligned(8)));return ub_stat_result(syscall(195,path,k),k,out);}
int lstat(const char *path,struct stat *out){unsigned char k[104] __attribute__((aligned(8)));return ub_stat_result(syscall(196,path,k),k,out);}
int fstat(int fd,struct stat *out){unsigned char k[104] __attribute__((aligned(8)));return ub_stat_result(syscall(197,fd,k),k,out);}
int fstatat(int fd,const char *path,struct stat *out,int flags){unsigned char k[104] __attribute__((aligned(8)));return ub_stat_result(syscall(327,fd,path,k,flags),k,out);}
int utimensat(int fd,const char *path,const struct timespec times[2],int flags){
 struct ub_time32 t[2];
 if(times)for(int i=0;i<2;i++){
  if(times[i].tv_nsec==UTIME_NOW || times[i].tv_nsec==UTIME_OMIT){t[i].sec=0;t[i].nsec=(int32_t)times[i].tv_nsec;}
  else if(ub_time32_from(&t[i],times+i))return -1;
 }
 return (int)syscall(348,fd,path,times?t:0,flags);
}
int futimens(int fd,const struct timespec times[2]){return utimensat(fd,0,times,0);}
#endif
