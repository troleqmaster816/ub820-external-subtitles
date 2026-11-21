/* Host test of core/storage.c with a shell script in place of the UFS tool:
 * the tool starts with default signals, an empty signal mask, no inherited
 * descriptors above 2 and fd 0-2 open; a deadline gives a positive code,
 * removes the read's RAM copy, and never waits for a tool that does not end
 * (reaped later); a tool copy that cannot run is written again. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static char tool_path[96];
#define UB_TOOL_PATH tool_path
#define UB_TOOL_SECONDS 1
static int logged,polls,kills;
void ub_injector_poll(void){polls++;}
static int fake_kill(pid_t pid,int sig){(void)pid;(void)sig;kills++;return 0;}
#define kill fake_kill
#include "../core/storage.c"
#undef kill

void ub_log(const char *module,int level,const char *format,...){(void)module;(void)level;(void)format;logged++;}
const unsigned char ub_tool_ufs[]=
 "#!/bin/sh\n"
 "mode=$2; if [ \"$1\" = write ]; then mode=$(cat \"$2\"); fi\n"
 "case \"$mode\" in\n"
 " env) { grep -E '^Sig(Blk|Ign)' /proc/$$/status; for f in 0 1 2 42; do if [ -e /proc/$$/fd/$f ]; then echo \"fd$f $(readlink /proc/$$/fd/$f)\"; else echo \"fd$f closed\"; fi; done; } > \"$3\"; exit 0;;\n"
 " hang) echo secret > \"$3.ufs\"; exec sleep 3;;\n"
 " hangwrite) sleep 3; cat \"$2\" > \"$3\"; exit 0;;\n"
 " fail) exit 6;;\n"
 "esac\n"
 "exit 0\n";
const size_t ub_tool_ufs_size=sizeof ub_tool_ufs-1;

static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL storage_test:%d: %s\n",__LINE__,#c);failures++;}}while(0)
static char report[4096];
static void slurp(const char *path){FILE *f=fopen(path,"r");size_t n=f?fread(report,1,sizeof report-1,f):0;if(f)fclose(f);report[n]=0;}
static unsigned long long field(const char *name){const char *p=strstr(report,name);return p?strtoull(p+strlen(name),0,16):~0ULL;}
static unsigned long long ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000+(unsigned long long)t.tv_nsec/1000000;}

int main(void){
 char dir[]="/tmp/ub820-storage-test-XXXXXX",out[128],temp[160],input[128];sigset_t block;int saved0,rc;unsigned long long t;
 if(!mkdtemp(dir))return 1;
 snprintf(tool_path,sizeof tool_path,"%s/tool",dir);snprintf(out,sizeof out,"%s/out",dir);snprintf(temp,sizeof temp,"%s.ufs",out);
 /* What the player gives the helper: a blocked and an ignored signal, an
  * extra open file, and no stdin. */
 sigemptyset(&block);sigaddset(&block,SIGUSR1);sigprocmask(SIG_BLOCK,&block,0);signal(SIGUSR2,SIG_IGN);
 {int fd=open("/dev/null",O_RDONLY);CHECK(fd>=0&&dup2(fd,42)==42);close(fd);}
 saved0=dup(0);close(0);
 rc=ub_stick_read("env",out);CHECK(rc==0);slurp(out);
 CHECK(!(field("SigBlk:")&(1ULL<<(SIGUSR1-1))));CHECK(!(field("SigIgn:")&(1ULL<<(SIGUSR2-1))));
 CHECK(strstr(report,"fd42 closed"));CHECK(strstr(report,"fd0 /dev/null"));CHECK(!strstr(report,"fd1 closed"));CHECK(!strstr(report,"fd2 closed"));
 rc=ub_stick_read_quiet("env",out);CHECK(rc==0);slurp(out);CHECK(strstr(report,"fd2 /dev/null"));
 dup2(saved0,0);close(saved0);
 snprintf(input,sizeof input,"%s/input",dir);
 FILE *f=fopen(input,"w");CHECK(f);if(f){fputs("fail",f);fclose(f);}
 /* The tool's own codes pass through. */
 CHECK(ub_stick_write(input,out)==6);
 /* Overdue readers and writers are never killed, and deferred detach is polled. */
 polls=0;logged=0;t=ms();rc=ub_stick_read("hang",out);
 CHECK(rc==UB_STICK_TIMEOUT&&access(temp,F_OK)&&ms()-t<1800&&polls>=10&&logged==1);
 CHECK(ub_stick_reap()==1&&ub_stick_read("env",out)==UB_STICK_BUSY);
 for(int i=0;i<40&&ub_stick_reap();i++)usleep(100000);
 CHECK(!ub_stick_reap()&&access(temp,F_OK));
 f=fopen(input,"w");CHECK(f);if(f){fputs("hangwrite",f);fclose(f);}
 rc=ub_stick_write(input,out);CHECK(rc==UB_STICK_TIMEOUT&&ub_stick_reap()==1);
 char held[300];snprintf(held,sizeof held,"%s",stuck[0].temp);CHECK(!access(held,F_OK));
 f=fopen(input,"w");CHECK(f);if(f){fputs("changed",f);fclose(f);}
 CHECK(ub_stick_write(input,out)==UB_STICK_BUSY);
 for(int i=0;i<40&&ub_stick_reap();i++)usleep(100000);
 CHECK(!ub_stick_reap()&&access(held,F_OK));slurp(out);CHECK(!strcmp(report,"hangwrite"));
 CHECK(ub_stick_read("env",out)==0);
 CHECK(!kills);
 /* The /tmp copy of the tool vanished: written again, and the call works. */
 unlink(tool_path);CHECK(ub_stick_read("env",out)==0);
 CHECK(strcmp(ub_stick_error(UB_STICK_NO_TOOL),"unexpected error")&&strcmp(ub_stick_error(UB_STICK_ABSENT),"unexpected error"));
 {char cmd[96];snprintf(cmd,sizeof cmd,"rm -rf %s",dir);if(system(cmd)){}}
 printf("storage_test %s (%d failures)\n",failures?"FAILED":"passed",failures);return failures!=0;
}
