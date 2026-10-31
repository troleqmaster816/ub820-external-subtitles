#define _GNU_SOURCE
#include "storage.h"
#include "log.h"
#include "sys.h"
#include "tools.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The tool is dynamically linked against the player's own libc and libufs,
 * so it is written to /tmp and run as a child process. It inherits stderr,
 * so its own one-line messages land in the RAM log. */
#define UB_TOOL_PATH "/tmp/.ub820_ufs"
#define UB_TOOL_SECONDS 20

static int tool_ready(void){
 static int ready;int fd;size_t done=0;ssize_t n;
 if(ready)return 0;
 unlink(UB_TOOL_PATH);
 fd=open(UB_TOOL_PATH,O_WRONLY|O_CREAT|O_TRUNC,0700);if(fd<0)return -1;
 while(done<ub_tool_ufs_size&&(n=write(fd,ub_tool_ufs+done,ub_tool_ufs_size-done))>0)done+=(size_t)n;
 if(close(fd)||done!=ub_tool_ufs_size){unlink(UB_TOOL_PATH);return -1;}
 ready=1;return 0;
}
static int run_tool(const char *op,const char *from,const char *to,int quiet){
 char *const argv[]={(char *)UB_TOOL_PATH,(char *)op,(char *)from,(char *)to,0};
 char *const envp[]={(char *)"LD_LIBRARY_PATH=/usr/X11R6/lib:/usr/dtvrec/lib:/lib:/usr/lib:/usr/local/lib",0};
 int status,i;pid_t pid,r;
 if(tool_ready())return -1;
 pid=fork();if(pid<0)return -1;
 if(!pid){if(quiet){int nul=open("/dev/null",O_WRONLY);if(nul>=0){dup2(nul,2);close(nul);}}execve(UB_TOOL_PATH,argv,envp);_exit(127);}
 for(i=0;i<UB_TOOL_SECONDS*10;i++){
  r=waitpid(pid,&status,WNOHANG);
  if(r==pid)return WIFEXITED(status)?(WEXITSTATUS(status)==127?-1:WEXITSTATUS(status)):-1;
  if(r<0&&errno!=EINTR)return -1;
  ub_sleep_ms(100);
 }
 kill(pid,SIGKILL);waitpid(pid,&status,0);return -2;
}
int ub_stick_read(const char *stick_path,const char *tmp_path){return run_tool("read",stick_path,tmp_path,0);}
/* For a file that may be absent: the tool's own message is not logged. */
int ub_stick_read_quiet(const char *stick_path,const char *tmp_path){return run_tool("read",stick_path,tmp_path,1);}
int ub_stick_write(const char *tmp_path,const char *stick_path){return run_tool("write",tmp_path,stick_path,0);}
const char *ub_stick_error(int code){
 switch(code){
 case 0:return "done";
 case -1:return "storage tool could not run";
 case -2:return "storage tool timed out";
 case 3:return "player file layer did not start";
 case 4:return "source not found";
 case 5:return "destination cannot be created";
 case 6:return "read failed";
 case 7:return "write failed";
 case 8:return "could not finish the file";
 case 9:return "file too large";
 default:return "unexpected error";
 }
}
