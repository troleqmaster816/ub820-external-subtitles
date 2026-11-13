#ifndef UB_SUBTITLE_BRIGHTNESS_IO_H
#define UB_SUBTITLE_BRIGHTNESS_IO_H
#include "brightness_protocol.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>

/* Fixed small RAM records only. The legacy ARM stat64 call avoids statx/time64
 * probes on Linux4.4. Host fixtures use their ordinary native fstat ABI. */
static inline int ub_br_private_size(int fd,size_t expected){
 unsigned mode,uid;int64_t size;
#if defined(__arm__) && !defined(UB_HOST_TEST)
 unsigned char raw[104] __attribute__((aligned(8)));
 if(syscall(197,fd,raw))return -1;
 memcpy(&mode,raw+16,4);memcpy(&uid,raw+24,4);memcpy(&size,raw+48,8);
#else
 struct stat st;if(fstat(fd,&st))return -1;
 mode=(unsigned)st.st_mode;uid=(unsigned)st.st_uid;size=(int64_t)st.st_size;
#endif
 return S_ISREG(mode)&&!(mode&077)&&uid==(unsigned)geteuid()&&size==(int64_t)expected?0:-1;
}
/* 0 valid,1 definitely absent at open, -1 existing/unknown/invalid. Callers
 * must not treat a damaged persistence claim as permission for another save. */
static inline int ub_br_private_read(const char *path,void *output,size_t expected){
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK),rc=-1;size_t used=0;ssize_t n;unsigned char extra;
 if(fd<0)return errno==ENOENT?1:-1;
 if(ub_br_private_size(fd,expected))goto done;
 while(used<expected){n=read(fd,(char *)output+used,expected-used);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;used+=(size_t)n;}
 do{n=read(fd,&extra,1);}while(n<0&&errno==EINTR);
 if(!n)rc=0;
done:
 if(close(fd))rc=-1;
 return rc;
}
/* Atomic private-record writer. Ownership is fixed: backend COMMIT notice;
 * supervisor seed/claim/ACK. UI never writes either party's records. */
static inline int ub_br_private_write(const char *path,const void *input,size_t size){
 char temp[192];int fd,rc=0;size_t used=0;ssize_t n;
 int length=snprintf(temp,sizeof temp,"%s.tmp-XXXXXX",path);
 if(length<0||(size_t)length>=sizeof temp)return -1;
 fd=mkstemp(temp);if(fd<0)return -1;
 while(used<size){n=write(fd,(const char *)input+used,size-used);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-1;break;}used+=(size_t)n;}
 if(close(fd))rc=-1;
 if(!rc&&rename(temp,path))rc=-1;
 if(rc)unlink(temp);
 return rc;
}
static inline int ub_br_token_nonzero(const unsigned char token[16]){
 unsigned char any=0;for(unsigned i=0;i<16;i++)any|=token[i];return any!=0;
}
static inline int ub_br_seed_valid(const struct ub_brightness_seed *seed,unsigned pid,uint64_t born){
 return seed->magic==UB_BRIGHTNESS_SEED_MAGIC&&seed->version==UB_BRIGHTNESS_PROTOCOL&&
  seed->pid==pid&&(((uint64_t)seed->birth_hi<<32)|seed->birth_lo)==born&&
  seed->percent<=100&&ub_br_token_nonzero(seed->token);
}
static inline int ub_br_birth(unsigned pid,uint64_t *born){
 char path[64],text[2048],*p,*save;unsigned field=3;ssize_t n;int fd;
 if(!pid||pid>INT32_MAX||!born)return -1;
 snprintf(path,sizeof path,"/proc/%u/stat",pid);
 fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return -1;
 do{n=read(fd,text,sizeof text-1);}while(n<0&&errno==EINTR);
 close(fd);if(n<=0||n==(ssize_t)sizeof text-1)return -1;text[n]=0;
 p=strrchr(text,')');if(!p)return -1;
 for(p=strtok_r(p+1," ",&save);p;p=strtok_r(0," ",&save),field++){
  if(field==3&&(p[0]=='Z'||p[0]=='X'||p[0]=='x'))return -2;
  if(field==22){
  uint64_t value=0;if(!*p)return -1;
  for(const char *digit=p;*digit;digit++){
   unsigned d=(unsigned char)*digit-'0';
   if(d>9||value>UINT64_C(1844674407370955161)||
      (value==UINT64_C(1844674407370955161)&&d>5))return -1;
   value=value*10u+d;
  }
  if(!value)return -1;
  *born=value;return 0;
  }
 }
 return -1;
}
#endif
