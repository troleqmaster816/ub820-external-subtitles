#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "native_wait.h"
#include "../../modules/native_events/wait_scope.h"
extern int EV_create_flag(uint32_t),EV_wait_flag(uint32_t,void *);
static unsigned calls,interruptions,consumed,malformed;static int terminal;
static uint32_t active=UB_NATIVE_SETTINGS_FLAG;
static int entry_ready;
int fixture_open(const char *p,int flags,...) __asm__("open");
int fixture_open(const char *p,int flags,...){
 mode_t mode=0;if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=(mode_t)va_arg(ap,int);va_end(ap);}
 return openat(AT_FDCWD,!strcmp(p,"/dev/dtvrec/evk")?"/dev/null":p,flags,mode);
}
int ioctl(int fd,unsigned long req,...){
 va_list ap;uint32_t *p;(void)fd;va_start(ap,req);p=va_arg(ap,uint32_t *);va_end(ap);
 if(req!=0x890d)return 0;
 calls++;if(p[0]!=1||p[2]!=active||p[3]!=0){malformed++;errno=EINVAL;return -1;}
 p[0]=0;
 if(interruptions){interruptions--;errno=EINTR;return -1;}
 if(terminal){errno=terminal;return -1;}
 p[0]=1;p[3]=0x20656e67;consumed++;return 0;
}
void fixture_entry_check(void){
 char path[200];snprintf(path,sizeof path,UB_NATIVE_WAIT_DIR "/%d.ready",(int)getpid());entry_ready=!access(path,F_OK);
 if(getenv("FIXTURE_HOLD")){
  int n=snprintf(path,sizeof path,"ENTRY_PID %d\n",(int)getpid());char token;
  if(write(1,path,(size_t)n)!=n||read(0,&token,1)!=1)_exit(99);
 }
}
int main(int argc,char **argv){
 uint32_t out[2]={0xdeadbeef,0xdeadbeef};char ready[200];
 if(EV_create_flag(active))return 10;
 interruptions=3;int rc=EV_wait_flag(active,out);
 snprintf(ready,sizeof ready,UB_NATIVE_WAIT_DIR "/%d.ready",(int)getpid());
 if(argc>1){
  if(rc!=4||calls!=(!strcmp(argv[1],"shadow")?0u:1u)||consumed||malformed||out[0]!=0x80000000||out[1]!=0||!access(ready,F_OK)||entry_ready)return 11;
  puts("native_flag_client baseline passed: EINTR clears output; no readiness");return 0;
 }
 if(!entry_ready||access(ready,F_OK)||rc||calls!=4||consumed!=1||malformed||out[0]!=active||out[1]!=0x20656e67)return 12;
 interruptions=2;calls=0;terminal=ENOENT;if(EV_wait_flag(active,out)!=2||calls!=3)return 13;
 interruptions=1;calls=0;terminal=EIO;if(EV_wait_flag(active,out)!=7||calls!=2)return 14;
 active++;terminal=0;if(EV_create_flag(active))return 15;
 interruptions=2;calls=0;if(EV_wait_flag(active,out)!=4||calls!=1)return 16;
 puts("native_flag_client passed: real libev flag packet rebuild, one delivery, destruction/error and non-target pass-through");return 0;
}
