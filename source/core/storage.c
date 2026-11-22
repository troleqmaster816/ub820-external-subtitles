#define _GNU_SOURCE
#include "storage.h"
#include "log.h"
#include "sys.h"
#include "tools.h"
#include "injector.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The tool is dynamically linked against the player's own libc and libufs,
 * so it is written to /tmp and run as a child process. It inherits stderr,
 * so its own one-line messages land in the RAM log. */
#ifndef UB_TOOL_PATH
#define UB_TOOL_PATH "/tmp/.ub820_ufs"
#endif
#ifndef UB_TOOL_SECONDS
#define UB_TOOL_SECONDS 10
#endif
#define UB_TOOL_STUCK 4

static int ready;
static int tool_ready(void){
 int fd;size_t done=0;ssize_t n;
 if(ready)return 0;
 unlink(UB_TOOL_PATH);
 fd=open(UB_TOOL_PATH,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0700);if(fd<0)return -1;
 while(done<ub_tool_ufs_size&&(n=write(fd,ub_tool_ufs+done,ub_tool_ufs_size-done))>0)done+=(size_t)n;
 if(close(fd)||done!=ub_tool_ufs_size){unlink(UB_TOOL_PATH);return -1;}
 ready=1;return 0;
}
/* An overdue tool retains ownership of USB until it exits. Never kill a
 * libufs lock holder, including a reader. Reap later and remove its RAM
 * copies: a writer's private input, a reader's private output and temp. */
static struct { pid_t pid;char temp[300],output[300]; } stuck[UB_TOOL_STUCK];
int ub_stick_reap(void){
 int left=0,status;
 for(unsigned i=0;i<UB_TOOL_STUCK;i++){
  pid_t r;
  if(stuck[i].pid<=0)continue;
  r=waitpid(stuck[i].pid,&status,WNOHANG);
  if(r==stuck[i].pid||(r<0&&errno==ECHILD)){
   if(stuck[i].temp[0])unlink(stuck[i].temp);
   if(stuck[i].output[0])unlink(stuck[i].output);
   stuck[i].pid=0;
  }
  else left++;
 }
 return left;
}
/* Child side: nothing the helper inherited from the player (signal mask,
 * ignored signals, open files such as its single-instance lock) reaches the
 * tool; fd 0-2 are always open so no file the tool opens becomes one. */
static void child_setup(int quiet){
 if(ub_child_sanitize(quiet?3u:7u))_exit(127);
}
static int run_transfer(const char *op,const char *from,const char *to,int quiet){
 char *const argv[]={(char *)UB_TOOL_PATH,(char *)op,(char *)from,(char *)to,0};
 char *const envp[]={(char *)"LD_LIBRARY_PATH=/usr/X11R6/lib:/usr/dtvrec/lib:/lib:/usr/lib:/usr/local/lib",0};
 int status,i,attempt;pid_t pid,r;
 if(ub_stick_reap())return UB_STICK_BUSY;
 for(attempt=0;attempt<2;attempt++){
  if(tool_ready())return UB_STICK_NO_TOOL;
  pid=fork();if(pid<0)return UB_STICK_NO_TOOL;
  if(!pid){child_setup(quiet);execve(UB_TOOL_PATH,argv,envp);_exit(127);}
  for(i=0;i<UB_TOOL_SECONDS*10;i++){
   ub_injector_poll();
   r=waitpid(pid,&status,WNOHANG);
   if(r==pid)break;
   if(r<0&&errno!=EINTR)return UB_STICK_NO_TOOL;
   ub_sleep_ms(100);
  }
  /* One last look after the last sleep: a tool that ended in it is done. */
  if(i==UB_TOOL_SECONDS*10&&waitpid(pid,&status,WNOHANG)==pid)i=0;
  if(i==UB_TOOL_SECONDS*10)break;
  if(!WIFEXITED(status))return UB_STICK_NO_TOOL;
  if(WEXITSTATUS(status)!=127)return WEXITSTATUS(status);
  ready=0; /* not runnable (e.g. /tmp copy gone): written again, once */
 }
 if(attempt==2)return UB_STICK_NO_TOOL;
 {
  /* Deadline: the read's RAM copy may be partial (and may hold a secret). */
  char temp[300];unsigned k;
  if(!strcmp(op,"write"))snprintf(temp,sizeof temp,"%s",from);
  else if(snprintf(temp,sizeof temp,"%s.ufs",to)<(int)sizeof temp)unlink(temp);
  else temp[0]=0;
  r=waitpid(pid,&status,WNOHANG);
  /* It may have ended on its own since the last look: its result holds. */
  if(r==pid&&WIFEXITED(status)&&WEXITSTATUS(status)!=127)return WEXITSTATUS(status);
  if(r==pid||(r<0&&errno!=EINTR)){if(temp[0])unlink(temp);return UB_STICK_TIMEOUT;}
  for(k=0;k<UB_TOOL_STUCK&&stuck[k].pid>0;k++){}
  if(k<UB_TOOL_STUCK){
   stuck[k].pid=pid;snprintf(stuck[k].temp,sizeof stuck[k].temp,"%s",temp);
   snprintf(stuck[k].output,sizeof stuck[k].output,"%s",strcmp(op,"write")?to:"");
  }
  ub_log("storage",UB_ALWAYS,"USB tool PID%d exceeded its deadline; retained until completion, USB transfers wait",(int)pid);
  return UB_STICK_TIMEOUT;
 }
}
/* A new 0600 file under a name no other caller uses. glibc's mkstemp probes
 * a time64 syscall absent from the player kernel. */
static int private_file(char *name,size_t size,const char *kind){
 static unsigned serial;int fd=-1;
 for(unsigned attempt=0;attempt<16;attempt++){
  snprintf(name,size,"/tmp/ub820-ufs-%s-%d-%u",kind,(int)getpid(),++serial);
  fd=open(name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
  if(fd>=0||errno!=EEXIST)break;
 }
 return fd;
}
static int retained(const char *name){
 for(unsigned i=0;i<UB_TOOL_STUCK;i++)
  if(stuck[i].pid>0&&(!strcmp(stuck[i].temp,name)||!strcmp(stuck[i].output,name)))return 1;
 return 0;
}
/* A reader that misses its deadline may still finish much later. It writes
 * only a private name, never the caller's file: a late result must neither
 * replace a newer copy (the menu settings) nor leave a secret (SSH) under a
 * known name. Only a clean, timely result is moved into place. */
static int run_read(const char *op,const char *from,const char *to,int quiet){
 char output[96],temp[100];int fd,rc;
 fd=private_file(output,sizeof output,"output");if(fd<0)return 5;
 close(fd);
 rc=run_transfer(op,from,output,quiet);
 if(retained(output))return rc;
 if(!rc&&rename(output,to))rc=8;
 snprintf(temp,sizeof temp,"%s.ufs",output);unlink(temp);
 if(rc)unlink(output);
 return rc;
}
static int run_tool(const char *op,const char *from,const char *to,int quiet){
 char input[96],buf[8192];int in,out=-1,rc=0;ssize_t n;
 if(ub_stick_reap())return UB_STICK_BUSY;
 if(strcmp(op,"write"))return run_read(op,from,to,quiet);
 /* A timed-out writer can finish later. Give it an immutable RAM source:
  * another setting/log save must not truncate the input it still owns. */
 in=open(from,O_RDONLY|O_CLOEXEC);if(in<0)return 4;
 out=private_file(input,sizeof input,"input");
 if(out<0){close(in);return 5;}
 for(;;){
  do{n=read(in,buf,sizeof buf);}while(n<0&&errno==EINTR);
  if(n<=0){if(n<0)rc=6;break;}
  size_t used=0;
  while(used<(size_t)n){ssize_t w=write(out,buf+used,(size_t)n-used);if(w<0&&errno==EINTR)continue;if(w<=0){rc=7;break;}used+=(size_t)w;}
  if(rc)break;
 }
 close(in);if(close(out))rc=7;
 if(!rc)rc=run_transfer(op,input,to,quiet);
 if(!retained(input))unlink(input);
 return rc;
}
int ub_stick_read(const char *stick_path,const char *tmp_path){return run_tool("read",stick_path,tmp_path,0);}
/* For a file that may be absent: the tool's own message is not logged. */
int ub_stick_read_quiet(const char *stick_path,const char *tmp_path){return run_tool("read",stick_path,tmp_path,1);}
/* A file the helper writes itself: an interrupted replacement is read from
 * the previous copy the tool keeps (see tools/ufs/ufs.c). */
int ub_stick_load(const char *stick_path,const char *tmp_path,int quiet){return run_tool("load",stick_path,tmp_path,quiet);}
int ub_stick_write(const char *tmp_path,const char *stick_path){return run_tool("write",tmp_path,stick_path,0);}
/* Retries: a repeated failure must not add lines to the log it is saving. */
int ub_stick_write_quiet(const char *tmp_path,const char *stick_path){return run_tool("write",tmp_path,stick_path,1);}
const char *ub_stick_error(int code){
 switch(code){
 case 0:return "done";
 case 3:return "player file layer did not start";
 case 4:return "source could not be opened";
 case 5:return "destination cannot be created";
 case 6:return "read failed";
 case 7:return "write failed";
 case 8:return "could not finish the file";
 case 9:return "file too large";
 case UB_STICK_ABSENT:return "not on the stick";
 case UB_STICK_NO_TOOL:return "storage tool could not run";
 case UB_STICK_TIMEOUT:return "storage tool timed out";
 case UB_STICK_BUSY:return "an earlier USB transfer has not ended";
 case UB_STICK_UNREAD:return "the stick's saved choices could not be read; kept until they can";
 default:return "unexpected error";
 }
}
