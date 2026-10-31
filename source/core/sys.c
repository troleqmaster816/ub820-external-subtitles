#define _GNU_SOURCE
#include "sys.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

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
 s->mode=mode;s->mtime=mtime;return 0;
}
int ub_stat(const char *path,struct ub_stat *s){unsigned char k[104] __attribute__((aligned(8)));return stat_fill(syscall(NR_STAT64,path,k),k,s);}
int ub_fstat(int fd,struct ub_stat *s){unsigned char k[104] __attribute__((aligned(8)));return stat_fill(syscall(NR_FSTAT64,fd,k),k,s);}
static long dents(int fd,char *buf,size_t size){return syscall(NR_GETDENTS64,fd,buf,size);}
#else
/* Host tests: the C library is fine there. */
#include <sys/stat.h>
void ub_now(struct timespec *t){clock_gettime(CLOCK_MONOTONIC,t);}
void ub_sleep_ms(unsigned ms){struct timespec k={(time_t)(ms/1000),(long)(ms%1000)*1000000},rest;while(nanosleep(&k,&rest)&&errno==EINTR)k=rest;}
static void stat_copy(const struct stat *st,struct ub_stat *s){s->dev=st->st_dev;s->ino=st->st_ino;s->size=st->st_size;s->mtime=st->st_mtime;s->mode=st->st_mode;}
int ub_stat(const char *path,struct ub_stat *s){struct stat st;if(stat(path,&st))return -1;stat_copy(&st,s);return 0;}
int ub_fstat(int fd,struct ub_stat *s){struct stat st;if(fstat(fd,&st))return -1;stat_copy(&st,s);return 0;}
static long dents(int fd,char *buf,size_t size){return syscall(SYS_getdents64,fd,buf,size);}
#endif

unsigned long long ub_now_ms(void){struct timespec t;ub_now(&t);return (unsigned long long)t.tv_sec*1000+(unsigned long long)t.tv_nsec/1000000;}

/* linux_dirent64: d_ino 8, d_off 8, d_reclen 2, d_type 1, d_name */
int ub_dir_open(struct ub_dir *d,const char *path){d->pos=d->len=0;d->fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);return d->fd<0?-1:0;}
const char *ub_dir_next(struct ub_dir *d){
 for(;;){
  if(d->pos>=d->len){
   long n=dents(d->fd,d->buf,sizeof d->buf);
   if(n<=0)return 0;
   d->len=(int)n;d->pos=0;
  }
  {
   char *e=d->buf+d->pos;unsigned short reclen;
   memcpy(&reclen,e+16,2);
   if(reclen<20||d->pos+reclen>d->len){d->pos=d->len;continue;}
   d->pos+=reclen;
   return e+19;
  }
 }
}
void ub_dir_close(struct ub_dir *d){if(d->fd>=0)close(d->fd);d->fd=-1;}

ssize_t ub_read_file(const char *path,char *buf,size_t size){
 int fd=open(path,O_RDONLY|O_CLOEXEC);ssize_t n,got=0;
 if(fd<0||!size){if(fd>=0)close(fd);return -1;}
 while((size_t)got<size-1&&(n=read(fd,buf+got,size-1-(size_t)got))>0)got+=n;
 close(fd);buf[got]=0;return got;
}
