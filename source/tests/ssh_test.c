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
int ub_stick_read(const char *stick_path,const char *tmp_path){(void)stick_path;(void)tmp_path;return 4;}
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
  prctl(PR_SET_NAME,".ub820_dropbear");
  signal(SIGCHLD,SIG_IGN); /* like dropbear, reap the sessions */
  for(int i=0;i<sessions;i++)if(!fork())chain(1,limit,pause_ms);
  for(;;)pause();
 }
 return m;
}
static unsigned below(pid_t master,int *deepest){
 static struct tree_proc t[GUARD_PROCS];const char *comm;
 return tree_below(t,tree_read(t,GUARD_PROCS),master,deepest,&comm);
}
static void test_guard(void){
 int deepest,status;pid_t m;unsigned i;
 ub_log_open();unlink(UB_LOG_URGENT);
 /* a normal session: left alone */
 m=master_with(2,3,1);ub_sleep_ms(300);
 CHECK(below(m,&deepest)==6);CHECK(deepest==3);
 CHECK(!guard_check(m));CHECK(below(m,&deepest)==6);
 CHECK(!guard_check(m)); /* each process is traced once */
 kill(m,SIGKILL);waitpid(m,0,0);
 /* a chain that keeps growing (every 2 ms) until it is ended */
 m=master_with(1,400,2);
 for(i=0;i<100&&!guard_check(m);i++)ub_sleep_ms(GUARD_POLL_MS);
 CHECK(i<100);
 ub_sleep_ms(300);
 CHECK(below(m,&deepest)==0);           /* nothing left below the master */
 CHECK(!kill(m,0));                     /* the master still runs */
 {static struct tree_proc t[GUARD_PROCS];unsigned n=tree_read(t,GUARD_PROCS),left=0;
  for(unsigned k=0;k<n;k++)if(!strcmp(t[k].comm,"sh")&&t[k].ppid==1)left++;
  CHECK(left==0);}                      /* and no orphan escaped */
 CHECK(!access(UB_LOG_URGENT,F_OK));
 kill(m,SIGKILL);waitpid(m,&status,0);
 {static char log[65536];int fd=open(UB_LOG_FILE,O_RDONLY);if(fd>=0){if(read(fd,log,sizeof log-1)<0){}close(fd);}
  CHECK(strstr(log,"an ssh session ran away (a chain "));CHECK(strstr(log,"of 'sh'"));
  CHECK(strstr(log,"exe '"));
  {unsigned traced=0;for(const char *q=log;(q=strstr(q,"session trace: PID"));q++)traced++;CHECK(traced>=6&&traced<=6+GUARD_TRACE_LINES);}
  CHECK(strstr(log,"parent PID"));CHECK(strstr(log,", depth 3, exe '"));CHECK(strstr(log,"state T"));CHECK(strstr(log,"SigBlk:"));CHECK(strstr(log,"ended "));}
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
  dup2(open("/dev/null",O_RDONLY),9);
  char *argv[]={"/bin/sh","-c","grep -E 'SigBlk|SigIgn' /proc/self/status > " UB_LOG_FILE ".clean; ls /proc/$$/fd >> " UB_LOG_FILE ".clean",0};
  clean_exec(argv,0);
 }
 waitpid(c,&status,0);CHECK(WIFEXITED(status));
 {int fd=open(out,O_RDONLY);ssize_t n=fd>=0?read(fd,text,sizeof text-1):0;if(fd>=0)close(fd);text[n>0?n:0]=0;}
 CHECK(strstr(text,"SigBlk:\t0000000000000000"));CHECK(strstr(text,"SigIgn:\t0000000000000000"));
 CHECK(strstr(text,"\n0\n1\n")); /* 10 is the shell saving stdout for >> */CHECK(!strstr(text,"\n9\n"));
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
 printf("%s (%d failures)\n",failures?"ssh_test FAILED":"ssh_test passed",failures);
 return failures!=0;
}
