#define _GNU_SOURCE
#include "sys.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>

#ifdef __arm__
/* ARM EABI numbers of the syscalls Linux 4.4 has. */
#define NR_NANOSLEEP 162
#define NR_STAT64 195
#define NR_FSTAT64 197
#define NR_GETDENTS64 217
#define NR_CLOCK_GETTIME 263
struct time32 { int32_t sec,nsec; };
void ub_now(struct timespec *t){
 struct time32 k={0,0};
 syscall(NR_CLOCK_GETTIME,CLOCK_MONOTONIC,&k);
 t->tv_sec=k.sec;t->tv_nsec=k.nsec;
}
void ub_sleep_ms(unsigned ms){
 struct time32 k={(int32_t)(ms/1000),(int32_t)(ms%1000)*1000000},rest;
 while(syscall(NR_NANOSLEEP,&k,&rest)&&errno==EINTR)k=rest;
}
/* struct stat64 of the ARM EABI kernel ABI (104 bytes), read by offset. */
static int stat_fill(long rc,const unsigned char *k,struct ub_stat *s){
 uint32_t mtime,mode;
 if(rc)return -1;
 memcpy(&s->dev,k,8);memcpy(&mode,k+16,4);memcpy(&s->size,k+48,8);memcpy(&mtime,k+80,4);memcpy(&s->ino,k+96,8);
 s->mode=mode;s->mtime=mtime;memcpy(&s->uid,k+24,4);memcpy(&s->gid,k+28,4);return 0;
}
int ub_stat(const char *path,struct ub_stat *s){unsigned char k[104] __attribute__((aligned(8)));return stat_fill(syscall(NR_STAT64,path,k),k,s);}
int ub_fstat(int fd,struct ub_stat *s){unsigned char k[104] __attribute__((aligned(8)));return stat_fill(syscall(NR_FSTAT64,fd,k),k,s);}
static long dents(int fd,char *buf,size_t size){return syscall(NR_GETDENTS64,fd,buf,size);}
#else
/* Host tests: the C library is fine there. */
#include <sys/stat.h>
void ub_now(struct timespec *t){clock_gettime(CLOCK_MONOTONIC,t);}
void ub_sleep_ms(unsigned ms){struct timespec k={(time_t)(ms/1000),(long)(ms%1000)*1000000},rest;while(nanosleep(&k,&rest)&&errno==EINTR)k=rest;}
static void stat_copy(const struct stat *st,struct ub_stat *s){s->dev=st->st_dev;s->ino=st->st_ino;s->size=st->st_size;s->mtime=st->st_mtime;s->mode=st->st_mode;s->uid=st->st_uid;s->gid=st->st_gid;}
int ub_stat(const char *path,struct ub_stat *s){struct stat st;if(stat(path,&st))return -1;stat_copy(&st,s);return 0;}
int ub_fstat(int fd,struct ub_stat *s){struct stat st;if(fstat(fd,&st))return -1;stat_copy(&st,s);return 0;}
static long dents(int fd,char *buf,size_t size){return syscall(SYS_getdents64,fd,buf,size);}
#endif

unsigned long long ub_now_ms(void){struct timespec t;ub_now(&t);return (unsigned long long)t.tv_sec*1000+(unsigned long long)t.tv_nsec/1000000;}

#ifdef UB_HOST_TEST
/* Tests: replaces the getdents64 results (interrupted and failed reads). */
long (*ub_dir_test_dents)(int,char *,size_t);
#define DENTS(f,b,n) (ub_dir_test_dents?ub_dir_test_dents(f,b,n):dents(f,b,n))
#else
#define DENTS dents
#endif
/* linux_dirent64: d_ino 8, d_off 8, d_reclen 2, d_type 1, d_name */
int ub_dir_open(struct ub_dir *d,const char *path){d->pos=d->len=d->err=0;d->fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(d->fd<0){d->err=errno;return -1;}return 0;}
/* End of data and a failed read used to look the same, so a scan cut short
 * passed for a complete one. A malformed record is an error, not the end. */
int ub_dir_read(struct ub_dir *d,const char **name){
 *name=0;
 if(d->err){errno=d->err;return -1;}
 for(;;){
  if(d->pos>=d->len){
   long n=DENTS(d->fd,d->buf,sizeof d->buf);
   if(n<0&&errno==EINTR)continue;
   if(n<0){d->err=errno?errno:EIO;errno=d->err;return -1;}
   if(!n)return 0;
   d->len=(int)n;d->pos=0;
  }
  {
   char *e=d->buf+d->pos;unsigned short reclen;
   memcpy(&reclen,e+16,2);
   if(reclen<20||d->pos+reclen>d->len){d->pos=d->len;d->err=errno=EIO;return -1;}
   d->pos+=reclen;
   *name=e+19;return 1;
  }
 }
}
const char *ub_dir_next(struct ub_dir *d){const char *name;return ub_dir_read(d,&name)>0?name:0;}
void ub_dir_close(struct ub_dir *d){if(d->fd>=0)close(d->fd);d->fd=-1;}

ssize_t ub_read_file(const char *path,char *buf,size_t size){
 int fd=open(path,O_RDONLY|O_CLOEXEC);ssize_t n,got=0;
 if(fd<0||!size){if(fd>=0)close(fd);return -1;}
 /* A failed read is not the end of the file: a truncated /proc/modules or
  * maps would read as "absent". Interrupted reads are repeated. */
 while((size_t)got<size-1){
  n=read(fd,buf+got,size-1-(size_t)got);
  if(n<0&&errno==EINTR)continue;
  if(n<0){int saved=errno;close(fd);buf[0]=0;errno=saved;return -1;}
  if(!n)break;
  got+=n;
 }
 close(fd);buf[got]=0;return got;
}

int ub_child_wait_init(void){
 struct sigaction action;sigset_t child;
 memset(&action,0,sizeof action);action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);
 sigemptyset(&child);sigaddset(&child,SIGCHLD);
 return sigaction(SIGCHLD,&action,0)||sigprocmask(SIG_UNBLOCK,&child,0)?-1:0;
}
int ub_close_inherited(void){
 struct ub_dir d;const char *name;int incomplete=1;
 if(!ub_dir_open(&d,"/proc/self/fd")){
  while((name=ub_dir_next(&d)))if(name[0]>='0'&&name[0]<='9'){
   int fd=atoi(name);if(fd>2&&fd!=d.fd)close(fd);
  }
  incomplete=d.err!=0;ub_dir_close(&d);
 }
 if(incomplete){
  struct rlimit lim;
  if(getrlimit(RLIMIT_NOFILE,&lim)||lim.rlim_max==RLIM_INFINITY||lim.rlim_max>1048576)return -1;
  for(rlim_t fd=3;fd<lim.rlim_max;fd++)close((int)fd);
 }
 return 0;
}
int ub_child_sanitize(unsigned keep_stdio){
 struct sigaction action;sigset_t none;int nul;
 memset(&action,0,sizeof action);action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);
 for(int sig=1;sig<NSIG;sig++)sigaction(sig,&action,0);
 sigemptyset(&none);if(sigprocmask(SIG_SETMASK,&none,0)||ub_close_inherited())return -1;
 nul=open("/dev/null",O_RDWR);if(nul<0)return -1;
 for(int fd=0;fd<3;fd++)if(!(keep_stdio&(1u<<fd))||fcntl(fd,F_GETFD)<0){
  if(fd!=nul&&dup2(nul,fd)<0){if(nul>2)close(nul);return -1;}
 }
 if(nul>2)close(nul);
 return 0;
}
