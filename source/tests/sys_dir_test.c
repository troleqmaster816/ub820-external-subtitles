/* Host test of the directory iterator (core/sys.c): an entry, the end and a
 * read error are three different results; an interrupted read is retried;
 * an error stays an error and the old ub_dir_next form reports it in err. */
#define _GNU_SOURCE
#include "sys.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

extern long (*ub_dir_test_dents)(int,char *,size_t);
static int failures,interrupts,fail_after,calls;
#define CHECK(c) do{if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c);failures++;}}while(0)
static long real(int fd,char *b,size_t n){return syscall(SYS_getdents64,fd,b,n);}
/* interrupts: that many EINTR first; fail_after: EIO once that many reads passed. */
static long fake(int fd,char *b,size_t n){
 calls++;
 if(interrupts){interrupts--;errno=EINTR;return -1;}
 if(fail_after&&calls>fail_after){errno=EIO;return -1;}
 return real(fd,b,n);
}
static long garbage(int fd,char *b,size_t n){(void)fd;memset(b,0,n);return 64;}
static long partial_fds(int fd,char *b,size_t n){
 (void)fd;(void)n;if(calls++){errno=EIO;return -1;}
 unsigned short size=24;memset(b,0,24);memcpy(b+16,&size,2);b[19]='.';return 24;
}

int main(void){
 struct ub_dir d;const char *name;int rc,dot=0,n=0;
 /* Normal listing: entries, then a plain end (0), and the end stays the end. */
 CHECK(!ub_dir_open(&d,"/proc/self/task"));
 while((rc=ub_dir_read(&d,&name))>0){n++;if(!strcmp(name,"."))dot=1;}
 CHECK(rc==0);CHECK(dot&&n>=3);CHECK(!d.err);CHECK(ub_dir_read(&d,&name)==0&&!name);ub_dir_close(&d);
 /* An interrupted read is repeated, not taken for the end. */
 ub_dir_test_dents=fake;interrupts=3;calls=0;dot=0;
 CHECK(!ub_dir_open(&d,"/proc/self/task"));
 while((rc=ub_dir_read(&d,&name))>0)if(!strcmp(name,"."))dot=1;
 CHECK(rc==0);CHECK(dot);CHECK(!interrupts);CHECK(!d.err);ub_dir_close(&d);
 /* A failed read is -1 with errno, never the end; it stays failed. A small
  * buffer chunk is forced by failing the second read (the first returned data). */
 interrupts=0;calls=0;fail_after=1;n=0;
 CHECK(!ub_dir_open(&d,"/proc/self/task"));
 while((rc=ub_dir_read(&d,&name))>0)n++;
 CHECK(rc==-1);CHECK(errno==EIO);CHECK(d.err==EIO);CHECK(n>=3);
 errno=0;CHECK(ub_dir_read(&d,&name)==-1&&errno==EIO&&!name);ub_dir_close(&d);
 /* The old form still ends its loop on an error, and err tells the two apart. */
 calls=0;fail_after=1;
 CHECK(!ub_dir_open(&d,"/proc/self/task"));
 while(ub_dir_next(&d)){}
 CHECK(d.err==EIO);ub_dir_close(&d);
 /* A malformed record is an error, not a silently shortened list. */
 ub_dir_test_dents=garbage;
 CHECK(!ub_dir_open(&d,"/proc/self/task"));
 CHECK(ub_dir_read(&d,&name)==-1&&d.err==EIO);ub_dir_close(&d);
 ub_dir_test_dents=0;fail_after=0;
 /* open failure is reported in err as well */
 CHECK(ub_dir_open(&d,"/nonexistent-ub820-dir")==-1&&d.err==ENOENT);
 /* ub_read_file: a failed read is an error, never an empty (or shortened) file. */
 {char text[64];ssize_t got;
  memset(text,'x',sizeof text);errno=0;got=ub_read_file("/proc/self",text,sizeof text);
  CHECK(got==-1&&errno==EISDIR&&!text[0]);
  got=ub_read_file("/proc/self/comm",text,sizeof text);CHECK(got>0&&text[got-1]=='\n'&&!text[got]);
  CHECK(ub_read_file("/nonexistent-ub820-file",text,sizeof text)==-1);}
 /* Ignored/blocked SIGCHLD must not auto-reap children after helper init. */
 struct sigaction ignored={0},after;sigset_t block,mask;
 ignored.sa_handler=SIG_IGN;ignored.sa_flags=SA_NOCLDWAIT;sigemptyset(&ignored.sa_mask);
 CHECK(!sigaction(SIGCHLD,&ignored,0));sigemptyset(&block);sigaddset(&block,SIGCHLD);CHECK(!sigprocmask(SIG_BLOCK,&block,0));
 CHECK(!ub_child_wait_init());CHECK(!sigaction(SIGCHLD,0,&after)&&after.sa_handler==SIG_DFL&&!(after.sa_flags&SA_NOCLDWAIT));
 CHECK(!sigprocmask(SIG_SETMASK,0,&mask)&&!sigismember(&mask,SIGCHLD));
 int child_status;pid_t child=fork();CHECK(child>=0);
 if(!child){
  int fd=open("/dev/null",O_RDONLY);if(fd<0||dup2(fd,42)!=42)_exit(1);if(fd!=42)close(fd);
  signal(SIGUSR1,SIG_IGN);sigemptyset(&block);sigaddset(&block,SIGUSR2);sigprocmask(SIG_BLOCK,&block,0);
  calls=0;ub_dir_test_dents=partial_fds;
  if(ub_child_sanitize(7)||calls<2||fcntl(42,F_GETFD)>=0)_exit(2);
  sigaction(SIGUSR1,0,&after);sigprocmask(SIG_SETMASK,0,&mask);
  _exit(after.sa_handler!=SIG_DFL||sigismember(&mask,SIGUSR2)?3:37);
 }
 CHECK(waitpid(child,&child_status,0)==child&&WIFEXITED(child_status)&&WEXITSTATUS(child_status)==37);
 printf("%s\n",failures?"sys_dir_test FAILED":"sys_dir_test passed");
 return failures!=0;
}
