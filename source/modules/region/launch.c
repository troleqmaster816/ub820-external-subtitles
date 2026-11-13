/* apl_medm's native process launcher, intercepted after its null/state guards.
 * Only the verified BD executable uses this branch. All preparation occurs
 * before fork; the child performs the same close range, exec and _exit as stock.
 * No global environment is changed in the multithreaded media manager. */
#define _GNU_SOURCE
#include "bd_launch.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int (*ub_original_launch)(uintptr_t *);
const char *ub_debug_name;
extern char **environ;

static void launch_log(const char *s){
 int fd=open("/tmp/ub820_submenu.log",O_WRONLY|O_APPEND|O_CLOEXEC);size_t n=strlen(s);
 if(fd>=0){ssize_t r;do{r=write(fd,s,n);}while(r<0&&errno==EINTR);close(fd);}
}
static int launch_config(struct ub_bd_launch_config *c){
 unsigned char *p=(unsigned char *)c;size_t used=0;ssize_t n;char extra;int fd=open(UB_BD_LAUNCH_CONFIG,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
 if(fd<0)return -1;
 while(used<sizeof *c){n=read(fd,p+used,sizeof *c-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
 do{n=read(fd,&extra,1);}while(n<0&&errno==EINTR);
 close(fd);
 return used==sizeof *c&&!n&&c->magic==UB_BD_LAUNCH_MAGIC&&c->version==1&&!c->reserved&&ub_bd_valid_policy(c->policy)?0:-1;
}
/* The allocation contains pointers and our two strings. Original environment
 * strings remain unchanged and survive in the child's fork copy. */
static char **launch_environment(int policy){
 size_t count=0,i,n=0,oldlen,bytes,capacity,cleanlen=0;char **env,*old="",*preload,*previous,*clean;
 const char *choice=policy==-1?"UB820_BD_POLICY=v1:auto":policy==1?"UB820_BD_POLICY=v1:1":policy==2?"UB820_BD_POLICY=v1:2":policy==4?"UB820_BD_POLICY=v1:4":"UB820_BD_POLICY=v1:7";
 if(environ)while(environ[count]){if(!strncmp(environ[count],"LD_PRELOAD=",11))old=environ[count]+11;count++;}
 oldlen=strlen(old);
 if(count>(SIZE_MAX/sizeof(char *))-5||oldlen>(SIZE_MAX-2*sizeof(UB_BD_PRELOAD)-128)/3)return 0;
 bytes=(count+5)*sizeof(char *);
 capacity=3*oldlen+2*sizeof(UB_BD_PRELOAD)+128;
 if(bytes>SIZE_MAX-capacity)return 0;
 env=malloc(bytes+capacity);if(!env)return 0;
 preload=(char *)env+bytes;previous=preload+oldlen+sizeof("LD_PRELOAD=" UB_BD_PRELOAD)+1;
 clean=previous+oldlen+sizeof("UB820_BD_PRELOAD_OLD=");
 /* Remove only our exact token if a preexisting environment already contains
  * it. Preserve every unrelated preload and its order, using normal loader
  * whitespace/colon token semantics. */
 for(i=0;i<oldlen;){
  size_t start,length;
  while(i<oldlen&&(old[i]==':'||old[i]==' '||old[i]=='\t'))i++;
  start=i;while(i<oldlen&&old[i]!=':'&&old[i]!=' '&&old[i]!='\t')i++;length=i-start;
  if(!length||(length==sizeof(UB_BD_PRELOAD)-1&&!memcmp(old+start,UB_BD_PRELOAD,length)))continue;
  if(cleanlen)clean[cleanlen++]=' ';
  memcpy(clean+cleanlen,old+start,length);cleanlen+=length;
 }
 clean[cleanlen]=0;
 memcpy(preload,"LD_PRELOAD=",11);memmove(preload+11,clean,cleanlen);
 i=11+cleanlen;if(cleanlen)preload[i++]=' ';
 memcpy(preload+i,UB_BD_PRELOAD,sizeof(UB_BD_PRELOAD));
 memcpy(previous,"UB820_BD_PRELOAD_OLD=",21);memmove(previous+21,clean,cleanlen+1);
 for(i=0;i<count;i++)if(strncmp(environ[i],"LD_PRELOAD=",11)&&strncmp(environ[i],"UB820_BD_POLICY=",16)&&strncmp(environ[i],"UB820_BD_PRELOAD_OLD=",21))env[n++]=environ[i];
 env[n++]=preload;env[n++]=previous;env[n++]=(char *)choice;env[n]=0;
 return env;
}
int ub_hook_launch(uintptr_t *desc){
 struct ub_bd_launch_config c;char **env;pid_t pid;int saved;
 /* The native prelude precedes this hook, so desc is non-NULL and state is
  * neither 1 nor 2. Keep guards here as well for isolated tests and clarity. */
 if(!desc||desc[7]==1||desc[7]==2)return desc?0:-1;
 if(!desc[3]||strcmp((const char *)desc[3],"/usr/dtvrec/bin/apl_bdv")||
    !desc[2]||!ub_debug_name||!strcmp((const char *)desc[2],ub_debug_name))return ub_original_launch(desc);
 if(launch_config(&c)){
  launch_log("region: BD pre-main configuration unavailable; native launch retained\n");return ub_original_launch(desc);
 }
 if(!c.policy)return ub_original_launch(desc);
 env=launch_environment(c.policy);
 if(!env){launch_log("region: BD pre-main environment allocation failed; native launch retained\n");return ub_original_launch(desc);}
 pid=fork();saved=errno;desc[6]=(uintptr_t)pid;
 if(pid==0){
  int fd;for(fd=3;fd<50;fd++)close(fd);
  execve((const char *)desc[3],(char *const *)desc[4],env);
  _exit(0); /* identical to the native launcher's exec failure behavior */
 }
 free(env);errno=saved;
 if(pid<0){launch_log("region: BD pre-main launch fork failed\n");return -1;}
 desc[7]=1;return 0;
}
