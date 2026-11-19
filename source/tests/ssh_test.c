/* Host test of the ssh module:
 * - its self-contained $6$ SHA-512-crypt matches the standard
 *   (glibc/musl/openssl), which is what the patched dropbear verifies;
 *   vectors generated with `openssl passwd -6 -salt <salt> <pw>`;
 * - the session guard ends a runaway chain of processes below a stand-in
 *   for the dropbear master (one that keeps starting itself, like the shell
 *   chain that took the player down), leaves none behind, and keeps the
 *   master; a normal session is left alone;
 * - programs are started with no blocked or ignored signals and no inherited
 *   file descriptors (clean_exec), whatever the helper itself inherited;
 * - a PID counts as dropbear only while it runs our dropbear binary, an ended
 *   dropbear is reaped and logged, and Stop SSH ends it. */
#define _GNU_SOURCE
#include "ub.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../modules/ssh/module.c"

struct ub_module *const ub_modules[]={&ub_module_ssh,0};
/* storage stubs: the real tool is an ARM program for the player's libufs */
/* stick_killed: the tool was killed after writing <target>.ufs and even after
 * its rename, as a 20 s timeout can do; the plaintext must not survive. */
static int stick_killed;
int ub_stick_read(const char *stick_path,const char *tmp_path){
 (void)stick_path;
 if(stick_killed){
  char ufs[200];FILE *f;snprintf(ufs,sizeof ufs,"%s.ufs",tmp_path);
  if((f=fopen(ufs,"w"))){fputs("secret\n",f);fclose(f);}
  if((f=fopen(tmp_path,"w"))){fputs("secret\n",f);fclose(f);}
  return -2;
 }
 return 4;
}
int ub_stick_write(const char *tmp_path,const char *stick_path){(void)tmp_path;(void)stick_path;return 4;}
const char *ub_stick_error(int code){(void)code;return "test";}
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);failures++;}}while(0)

static int check(const char *pw,const char *salt,const char *want){
 char out[128];ub_sha512_crypt(pw,salt,out);
 int ok=!strcmp(out,want);
 printf("  %-18s %s\n    got  %s\n",pw,ok?"OK":"MISMATCH",out);
 if(!ok)printf("    want %s\n",want);
 return ok?0:1;
}

/* Below the master: "sh" processes, each starting the next after a pause,
 * up to limit deep (a bounded stand-in for the runaway). */
__attribute__((noreturn)) static void chain(int depth,int limit,unsigned pause_ms){
 prctl(PR_SET_NAME,"sh");
 if(depth<limit){ub_sleep_ms(pause_ms);if(!fork())chain(depth+1,limit,pause_ms);}
 for(;;)pause();
}
static pid_t master_with(int sessions,int limit,unsigned pause_ms){
 pid_t m=fork();
 if(!m){
  if(setpgid(0,0))_exit(1);
  prctl(PR_SET_NAME,".ub820_dropbear");
  signal(SIGCHLD,SIG_IGN); /* like dropbear, reap the sessions */
  for(int i=0;i<sessions;i++)if(!fork())chain(1,limit,pause_ms);
  for(;;)pause();
 }
 if(m>0&&setpgid(m,m)<0){
  kill(m,SIGKILL);while(waitpid(m,0,0)<0&&errno==EINTR){}
  return -1;
 }
 return m;
}
/* All synthetic sessions inherit the master's group. The test is their
 * subreaper, so cleanup also collects descendants orphaned during teardown. */
static void end_fixture_group(pid_t master){
 CHECK(master>0);
 if(master<=0)return;
 CHECK(!kill(-master,SIGKILL));
 while(waitpid(-master,0,0)>0||errno==EINTR){}
 CHECK(errno==ECHILD);
}
static unsigned below(pid_t master,int *deepest){
 static struct tree_proc t[GUARD_PROCS];const char *comm;
 return tree_below(t,tree_read(t,GUARD_PROCS),master,deepest,&comm);
}
static void test_guard(void){
 int deepest,was_subreaper;pid_t m;unsigned i;
 if(prctl(PR_GET_CHILD_SUBREAPER,&was_subreaper)||prctl(PR_SET_CHILD_SUBREAPER,1)){
  CHECK(0);return;
 }
 ub_log_open();unlink(UB_LOG_URGENT);
 /* a normal session: left alone */
 m=master_with(2,3,1);CHECK(m>0);if(m<=0)goto done;
 ub_sleep_ms(300);
 CHECK(below(m,&deepest)==6);CHECK(deepest==3);
 CHECK(!guard_check(m));CHECK(below(m,&deepest)==6);
 CHECK(!guard_check(m)); /* each process is traced once */
 end_fixture_group(m);
 /* a chain that keeps growing (every 2 ms) until it is ended */
 m=master_with(1,400,2);CHECK(m>0);if(m<=0)goto done;
 for(i=0;i<100&&!guard_check(m);i++)ub_sleep_ms(GUARD_POLL_MS);
 CHECK(i<100);
 ub_sleep_ms(300);
 CHECK(below(m,&deepest)==0);           /* nothing left below the master */
 CHECK(!kill(m,0));                     /* the master still runs */
 /* Collect terminated orphans, then count only this fixture's group. A
  * surviving session still has that group even when it has a new parent. */
 while(waitpid(-m,0,WNOHANG)>0){}
 {static struct tree_proc t[GUARD_PROCS];unsigned n=tree_read(t,GUARD_PROCS),left=0;
  for(unsigned k=0;k<n;k++)if(t[k].pid!=m&&getpgid(t[k].pid)==m)left++;
  CHECK(left==0);}
 CHECK(!access(UB_LOG_URGENT,F_OK));
 end_fixture_group(m);
 {static char log[65536];int fd=open(UB_LOG_FILE,O_RDONLY);if(fd>=0){if(read(fd,log,sizeof log-1)<0){}close(fd);}
  CHECK(strstr(log,"an ssh session ran away (a chain "));CHECK(strstr(log,"of 'sh'"));
  CHECK(strstr(log,"exe '"));
  {unsigned traced=0;for(const char *q=log;(q=strstr(q,"session trace: PID"));q++)traced++;CHECK(traced>=6&&traced<=6+GUARD_TRACE_LINES);}
  CHECK(strstr(log,"parent PID"));CHECK(strstr(log,", depth 3, exe '"));CHECK(strstr(log,"state T"));CHECK(strstr(log,"SigBlk:"));CHECK(strstr(log,"ended "));}
done:
 CHECK(!prctl(PR_SET_CHILD_SUBREAPER,was_subreaper));
}

static char log_buf[1<<16];
static const char *log_now(void){int fd=open(UB_LOG_FILE,O_RDONLY);ssize_t n=fd>=0?read(fd,log_buf,sizeof log_buf-1):0;if(fd>=0)close(fd);log_buf[n>0?n:0]=0;return log_buf;}

static void test_clean_exec(void){
 static const char out[]=UB_LOG_FILE ".clean";char text[2048];pid_t c;int status;sigset_t usr1;
 unlink(out);
 c=fork();
 if(!c){
  /* what a loader might leave behind */
  sigemptyset(&usr1);sigaddset(&usr1,SIGUSR1);sigaddset(&usr1,SIGCHLD);sigprocmask(SIG_BLOCK,&usr1,0);
  signal(SIGINT,SIG_IGN);signal(SIGCHLD,SIG_IGN);
  umask(000);
  /* Stress cleanup beyond the old 256-descriptor collection limit. */
  for(int i=0;i<600;i++)if(open("/dev/null",O_RDONLY)<0)_exit(100);
  dup2(open("/dev/null",O_RDONLY),9);
  char *argv[]={"/bin/sh","-c","grep -E 'SigBlk|SigIgn' /proc/self/status > " UB_LOG_FILE ".clean; ls /proc/$$/fd >> " UB_LOG_FILE ".clean; umask >> " UB_LOG_FILE ".clean",0};
  clean_exec(argv,0);
 }
 waitpid(c,&status,0);CHECK(WIFEXITED(status));
 {int fd=open(out,O_RDONLY);ssize_t n=fd>=0?read(fd,text,sizeof text-1):0;if(fd>=0)close(fd);text[n>0?n:0]=0;}
 CHECK(strstr(text,"SigBlk:\t0000000000000000"));CHECK(strstr(text,"SigIgn:\t0000000000000000"));
 CHECK(strstr(text,"\n0\n1\n")); /* 10 is the shell saving stdout for >> */CHECK(!strstr(text,"\n9\n"));
 CHECK(strstr(text,"\n0022\n"));
 for(char *line=text;line&&*line;){
  char *end;long fd=strtol(line,&end,10);
  if(end!=line&&(*end=='\n'||!*end)&&strcmp(line,"0022\n"))CHECK(fd<20);
  line=strchr(line,'\n');if(line)line++;
 }
}
static void test_dropbear_pid(void){
 char msg[200];pid_t c;
 /* a stand-in at the dropbear path: only that binary counts */
 {int in=open("/bin/sleep",O_RDONLY),o=open(DROPBEAR_BIN,O_WRONLY|O_CREAT|O_TRUNC,0700);char b[65536];ssize_t n;
  while(in>=0&&o>=0&&(n=read(in,b,sizeof b))>0)if(write(o,b,(size_t)n)!=n)break;
  if(in>=0)close(in);
  if(o>=0)close(o);}
 CHECK(!is_our_dropbear(getpid()));
 c=fork();
 if(!c){char *argv[]={DROPBEAR_BIN,"30",0};clean_exec(argv,1);}
 ub_sleep_ms(200);
 CHECK(is_our_dropbear(c));
 dropbear_pid=getpid();CHECK(!running_pid());        /* alive, but not dropbear */
 dropbear_pid=c;CHECK(running_pid()==c);
 kill(c,SIGKILL);ub_sleep_ms(200);
 CHECK(!running_pid());CHECK(!dropbear_pid);           /* reaped */
 CHECK(strstr(log_now(),"ended by signal 9"));
 c=fork();
 if(!c){char *argv[]={DROPBEAR_BIN,"30",0};clean_exec(argv,1);}
 ub_sleep_ms(200);dropbear_pid=c;
 stop_ssh(msg,sizeof msg);
 CHECK(!strcmp(msg,"stopped"));CHECK(!running_pid());CHECK(kill(c,0));
 stop_ssh(msg,sizeof msg);CHECK(!strcmp(msg,"not running"));
 unlink(DROPBEAR_BIN);
}
static void test_lost_child_ownership(void){
 struct sigaction ignored,old;unsigned long long started;char *argv[]={"/bin/true",0};
 memset(&ignored,0,sizeof ignored);ignored.sa_handler=SIG_IGN;sigemptyset(&ignored.sa_mask);
 CHECK(!sigaction(SIGCHLD,&ignored,&old));started=ub_now_ms();
 CHECK(run(argv,2)==-1);CHECK(ub_now_ms()-started<1000);
 CHECK(!sigaction(SIGCHLD,&old,0));
 /* A stale non-child PID is never a guard the helper owns or may signal. */
 guard_pid=getpid();guard_stop();CHECK(!guard_pid);CHECK(!kill(getpid(),0));
}

static int startup_calls,startup_failure;
static int startup_server(char *msg,size_t cap){
 startup_calls++;CHECK(auth==AUTH_KEY);CHECK(port==23456);
 if(startup_failure){snprintf(msg,cap,"fixture startup failed");return -1;}
 dropbear_pid=fork();
 if(!dropbear_pid){char *argv[]={DROPBEAR_BIN,"30",0};clean_exec(argv,1);}
 CHECK(dropbear_pid>0);ub_sleep_ms(100);
 snprintf(msg,cap,"fixture server started");return 0;
}
static void startup_action(const char *action){
 int fd=open(UB_SSH_REQUEST,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);
 if(fd>=0){CHECK(write(fd,action,strlen(action))==(ssize_t)strlen(action));close(fd);}
 step(0,0);
}
static void test_autostart(void){
 char text[256];int fd;ssize_t n;
 CHECK(!autostart);guard=0;auth=AUTH_KEY;port=23456;
 unlink(UB_SSH_REQUEST);unlink(UB_SSH_STATUS);
 ssh_start_test=startup_server;
 start(&ub_module_ssh);CHECK(startup_calls==0);CHECK(access(UB_SSH_STATUS,F_OK));
 autostart=1;startup_failure=1;
 start(&ub_module_ssh);CHECK(startup_calls==1);CHECK(!running_pid());
 fd=open(UB_SSH_STATUS,O_RDONLY);n=fd>=0?read(fd,text,sizeof text-1):0;if(fd>=0)close(fd);text[n>0?n:0]=0;
 CHECK(strstr(text,"0 -1 fixture startup failed"));
 CHECK(strstr(log_now(),"ssh autostart failed: fixture startup failed"));
 /* Retrying manually after an automatic failure uses the same settings and
  * normal Stop must remain stopped even though autostart is still enabled. */
 {int in=open("/bin/sleep",O_RDONLY),out=open(DROPBEAR_BIN,O_WRONLY|O_CREAT|O_TRUNC,0700);char b[65536];
  CHECK(in>=0&&out>=0);
  while(in>=0&&out>=0&&(n=read(in,b,sizeof b))>0)CHECK(write(out,b,(size_t)n)==n);
  if(in>=0)close(in);
  if(out>=0)close(out);}
 startup_failure=0;startup_action("start");CHECK(startup_calls==2);CHECK(running_pid()>0);
 fd=open(UB_SSH_STATUS,O_RDONLY);n=fd>=0?read(fd,text,sizeof text-1):0;if(fd>=0)close(fd);text[n>0?n:0]=0;
 CHECK(strstr(text,"1 0 fixture server started"));
 startup_action("stop");CHECK(!running_pid());
 for(int i=0;i<4;i++)step(0,0);
 CHECK(startup_calls==2);CHECK(!running_pid());
 /* A successful automatic start publishes the same running state. */
 start(&ub_module_ssh);CHECK(startup_calls==3);CHECK(running_pid()>0);
 fd=open(UB_SSH_STATUS,O_RDONLY);n=fd>=0?read(fd,text,sizeof text-1):0;if(fd>=0)close(fd);text[n>0?n:0]=0;
 CHECK(strstr(text,"1 0 fixture server started"));
 startup_action("stop");
 for(int i=0;i<4;i++)step(0,0);
 CHECK(startup_calls==3);CHECK(!running_pid());
 ssh_start_test=0;autostart=0;port=2222;
 unlink(DROPBEAR_BIN);unlink(UB_SSH_ENABLED);unlink(UB_SSH_STATUS);unlink(UB_SSH_REQUEST);
}

static void fake_dropbear_binary(void){
 int in=open("/bin/sleep",O_RDONLY),o=open(DROPBEAR_BIN,O_WRONLY|O_CREAT|O_TRUNC,0700);char b[65536];ssize_t n;
 while(in>=0&&o>=0&&(n=read(in,b,sizeof b))>0)if(write(o,b,(size_t)n)!=n)break;
 if(in>=0)close(in);
 if(o>=0)close(o);
}
static pid_t fake_dropbear(void){
 pid_t c=fork();
 if(!c){char *argv[]={DROPBEAR_BIN,"30",0};clean_exec(argv,1);}
 ub_sleep_ms(200);return c;
}
/* The guard is forked without exec: it must not keep the helper's
 * single-instance lock (or any other descriptor above stderr). */
/* 1 when pid holds a descriptor for path (by target: freed numbers get reused). */
static int holds(pid_t pid,const char *path){
 char dir[48],link[320],target[512],canonical[1024];struct dirent *e;int found=0;ssize_t n;
 if(!realpath(path,canonical))return -1;
 snprintf(dir,sizeof dir,"/proc/%d/fd",(int)pid);
 __typeof__(opendir(dir)) d=opendir(dir); /* DIR is the module's directory macro here */
 if(!d)return -1;
 while((e=readdir(d))){
  if(e->d_name[0]=='.')continue;
  snprintf(link,sizeof link,"%s/%s",dir,e->d_name);
  if((n=readlink(link,target,sizeof target-1))>0){target[n]=0;if(!strcmp(target,canonical))found=1;}
 }
 closedir(d);return found;
}
static void test_guard_descriptors(void){
 char path[64],target[256];int lock=open(UB_LOG_FILE ".lock",O_RDWR|O_CREAT,0600);pid_t c;
 CHECK(lock>2);CHECK(holds(getpid(),UB_LOG_FILE ".lock")==1);fake_dropbear_binary();c=fake_dropbear();
 guard_pid=0;guard_start(c);CHECK(guard_pid>0);ub_sleep_ms(200);
 CHECK(holds(guard_pid,UB_LOG_FILE ".lock")==0);
 snprintf(path,sizeof path,"/proc/%d/fd/2",(int)guard_pid);CHECK(readlink(path,target,sizeof target)>0); /* the log stays */
 guard_stop();kill(c,SIGKILL);waitpid(c,0,0);close(lock);unlink(UB_LOG_FILE ".lock");
 /* A dead server that is not reaped yet (a zombie) is not a server to guard. */
 c=fake_dropbear();kill(c,SIGKILL);ub_sleep_ms(100);
 {pid_t r=0;unsigned i;guard_start(c);CHECK(guard_pid>0);
  for(i=0;i<40&&!(r=waitpid(guard_pid,0,WNOHANG));i++)ub_sleep_ms(50);
  CHECK(r==guard_pid);if(r!=guard_pid)guard_stop();guard_pid=0;}
 waitpid(c,0,0);unlink(DROPBEAR_BIN);
}
/* A server that ends on its own is reaped and the status follows on the
 * next tick, without a menu request. */
static void test_dead_server_status(void){
 char text[256];int fd;ssize_t n;pid_t c;
 fake_dropbear_binary();c=fake_dropbear();dropbear_pid=c;action_reply[0]=0;guard=0;
 unlink(UB_SSH_REQUEST);answer(1,0,"fixture running");
 kill(c,SIGKILL);ub_sleep_ms(100);step(0,0);
 CHECK(!dropbear_pid);
 fd=open(UB_SSH_STATUS,O_RDONLY);n=fd>=0?read(fd,text,sizeof text-1):0;if(fd>=0)close(fd);text[n>0?n:0]=0;
 CHECK(text[0]=='0');CHECK(strstr(text,"dropbear ended"));
 unlink(UB_SSH_STATUS);unlink(DROPBEAR_BIN);
}
/* The plaintext password copy and the tool's partial copy are always removed. */
static void test_password_cleanup(void){
 char err[160];
 mkdir(DIR,0700);auth=AUTH_PASSWORD;stick_killed=1;
 CHECK(prepare_secret(err,sizeof err));
 CHECK(access(DIR "/pw.plain",F_OK));CHECK(access(DIR "/pw.plain.ufs",F_OK));
 stick_killed=0;auth=AUTH_KEY;
}

int main(void){
 int f=0;
 f+=check("testpass123","ub820salt",
   "$6$ub820salt$WE84zVU4TfFhcDFp.9u4knweIvM.viScKK.uysS27L.Xu8IP7VULSi8KXfmWOkgpk9s2W3Yjdx9kDVZszbd3h.");
 f+=check("x","saltsaltsaltsa16",
   "$6$saltsaltsaltsa16$092U8cCE0tpzYFFaDTSj4X4b4t2PKvl1CaQ86hcMg1eCmBTtzmDQxzIkRYdHU9SXhhS95w.Bfl.PBlsS3xJcK1");
 failures+=f;
 test_guard();
 test_clean_exec();
 test_dropbear_pid();
 test_lost_child_ownership();
 test_autostart();
 test_guard_descriptors();
 test_dead_server_status();
 test_password_cleanup();
 printf("%s (%d failures)\n",failures?"ssh_test FAILED":"ssh_test passed",failures);
 return failures!=0;
}
