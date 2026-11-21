/* Native EV waits are indefinite APIs returning positive error numbers.
 * Readiness is process-instance specific and also requires the DSO mapping:
 * a stale file must not authorize stopping a process after exec/reuse. */
#ifndef UB_NATIVE_WAIT_H
#define UB_NATIVE_WAIT_H
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef UB_NATIVE_WAIT_DIR
#define UB_NATIVE_WAIT_DIR "/tmp/.ub820_native_events"
#endif
#ifndef UB_NATIVE_WAIT_LIBRARY
#define UB_NATIVE_WAIT_LIBRARY "/tmp/.ub820_region/libbd_preload.so"
#endif
#define UB_NATIVE_WAIT_ENV "UB820_NATIVE_WAIT"
#define UB_NATIVE_WAIT_VALUE "v2"
#define UB_NATIVE_DB_QUEUE 0x90c45000u
static inline int ub_native_wait_process(const char *name){
 static const char *const names[]={"apl_bdv","apl_setting","apl_meda_bdplay","apl_meda_dvdv"};
 for(unsigned i=0;i<sizeof names/sizeof *names;i++){
  size_t n=strlen(names[i]);
  if(!strncmp(name,names[i],n)&&(!name[n]||!strcmp(name+n," (deleted)")))return 1;
 }
 return 0;
}

static inline unsigned long long ub_native_wait_birth(const char *stat){
 const char *p=strrchr(stat,')');unsigned field=3;
 if(!p)return 0;
 for(p++;*p;){
  while(*p==' ')p++;
  if(!*p)break;
  if(field==22){
   unsigned long long n=0;if(*p<'0'||*p>'9')return 0;
   while(*p>='0'&&*p<='9'){unsigned d=(unsigned)(*p++-'0');if(n>(ULLONG_MAX-d)/10)return 0;n=10*n+d;}
   return *p==' '||*p=='\n'||!*p?n:0;
  }
  while(*p&&*p!=' ')p++;
  field++;
 }
 return 0;
}
static inline int ub_native_wait_read(const char *path,char *out,size_t capacity){
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);ssize_t n;size_t used=0;
 if(fd<0)return -1;
 while(used+1<capacity){n=read(fd,out+used,capacity-1-used);if(n<0&&errno==EINTR)continue;if(n<0){close(fd);return -1;}if(!n)break;used+=(size_t)n;}
 close(fd);out[used]=0;return (int)used;
}
static inline int ub_native_wait_number(const char **cursor,unsigned base,unsigned long long *value){
 const char *p=*cursor;unsigned long long n=0;int any=0;
 while(*p>='0'&&(unsigned)(*p-'0')<base){
  unsigned d=(unsigned)(*p++-'0');if(n>(ULLONG_MAX-d)/base)return -1;
  n=base*n+d;any=1;
 }
 if(!any)return -1;
 *cursor=p;*value=n;return 0;
}
static inline int ub_native_wait_ready(int pid){
 char path[160],lease[160],link[180],info[1024],stat[4096],ready[128],expected[128],maps[8192],carry[1024];
 unsigned long long born,stamp,owner,live,flags;int fd,n,wanted,overflow=0;size_t used=0;ssize_t got;const char *cursor;
 snprintf(path,sizeof path,"/proc/%d/stat",pid);
 if(ub_native_wait_read(path,stat,sizeof stat)<0||!(born=ub_native_wait_birth(stat)))return 0;
 snprintf(path,sizeof path,UB_NATIVE_WAIT_DIR "/%d.ready",pid);
 n=ub_native_wait_read(path,ready,sizeof ready);
 if(n<0||strncmp(ready,"EVW2 ",5))return 0;
 cursor=ready+5;
 if(ub_native_wait_number(&cursor,10,&owner)||*cursor++!=' '||owner!=(unsigned)pid||
    ub_native_wait_number(&cursor,10,&stamp)||*cursor++!=' '||stamp!=born||
    ub_native_wait_number(&cursor,10,&live)||live>INT_MAX||strcmp(cursor,"\n"))return 0;
 wanted=snprintf(expected,sizeof expected,"EVW2 %d %llu %d\n",pid,born,(int)live);
 if(n!=wanted||n<0||memcmp(ready,expected,(size_t)n))return 0;
 /* starttime survives exec. A retained CLOEXEC descriptor proves that this
  * receipt belongs to the current executable image, even after same-PID exec
  * with the same DSO at the same address. Old EVW1 files cannot attest this. */
 snprintf(lease,sizeof lease,"/proc/%d/fd/%d",pid,(int)live);
 got=readlink(lease,link,sizeof link-1);if(got<0||got==(ssize_t)sizeof link-1)return 0;
 link[got]=0;if(strcmp(link,path))return 0;
 snprintf(lease,sizeof lease,"/proc/%d/fdinfo/%d",pid,(int)live);
 if(ub_native_wait_read(lease,info,sizeof info)<0)return 0;
 cursor=strstr(info,"flags:");if(!cursor)return 0;
 cursor+=6;while(*cursor==' '||*cursor=='\t')cursor++;
 if(ub_native_wait_number(&cursor,8,&flags)||(*cursor!='\n'&&*cursor)||!(flags&O_CLOEXEC))return 0;
 snprintf(path,sizeof path,"/proc/%d/maps",pid);fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
 /* Parse whole lines across read boundaries; do not accept a truncated path
  * or a retired/deleted asset. Atomic asset replacement marks old maps deleted. */
 for(;;){
  got=read(fd,maps,sizeof maps);if(got<0&&errno==EINTR)continue;if(got<=0)break;
  for(ssize_t i=0;i<got;i++){
   if(maps[i]!='\n'){if(used+1<sizeof carry)carry[used++]=maps[i];else overflow=1;continue;}
   carry[used]=0;
   const char *name=strchr(carry,'/'),*perms=strchr(carry,' ');
   if(perms)while(*perms==' ')perms++;
   if(!overflow&&name&&perms&&!strncmp(perms,"r-xp ",5)&&!strcmp(name,UB_NATIVE_WAIT_LIBRARY)){close(fd);return 1;}
   used=0;overflow=0;
  }
 }
 close(fd);return 0;
}

struct ub_part;
/* One shared native-loader service, independent of region settings. */
int ub_native_bd_publish(int policy);
struct ub_part *ub_native_bd_part(void);
/* Remove receipts (and unfinished temporaries) of processes that ended or
 * whose PID now belongs to a newer process. Returns how many were removed. */
unsigned ub_native_wait_sweep(void);
#endif
