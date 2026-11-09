/* Exercise the actual musl/ARM ABI shim under qemu-arm on the host. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static void caught(int signal){(void)signal;}
int main(void){
 char dir[]="/tmp/ub820-compat-XXXXXX",path[128],link[128];struct stat a,b;struct timespec now,req={0,1000000},rest;
 assert(mkdtemp(dir));snprintf(path,sizeof path,"%s/file",dir);snprintf(link,sizeof link,"%s/link",dir);
 int fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);assert(fd>=0);assert(write(fd,"test",4)==4);assert(symlink("file",link)==0);
 assert(!stat(path,&a)&&S_ISREG(a.st_mode)&&a.st_size==4&&a.st_nlink==1);
 assert(a.st_uid==getuid()&&a.st_gid==getgid()&&(a.st_mode&0777)==0600&&a.st_blksize>0);
 assert(!fstat(fd,&b)&&a.st_ino==b.st_ino&&a.st_dev==b.st_dev&&a.st_size==b.st_size);
 int d=open(dir,O_RDONLY|O_DIRECTORY);assert(d>=0);assert(!fstatat(d,"file",&b,0)&&b.st_ino==a.st_ino);
 assert(!lstat(link,&b)&&S_ISLNK(b.st_mode));assert(!fstatat(d,"link",&b,AT_SYMLINK_NOFOLLOW)&&S_ISLNK(b.st_mode));
 struct timespec times[2]={{1600000000,123456789},{1600000001,234567890}};
 assert(!utimensat(d,"file",times,0));assert(!stat(path,&a));assert(a.st_atim.tv_sec==times[0].tv_sec&&a.st_atim.tv_nsec==times[0].tv_nsec);assert(a.st_mtim.tv_sec==times[1].tv_sec&&a.st_mtim.tv_nsec==times[1].tv_nsec);
 times[0].tv_nsec=UTIME_OMIT;times[1].tv_nsec=UTIME_NOW;assert(!futimens(fd,times));assert(!fstat(fd,&b)&&b.st_atim.tv_sec==a.st_atim.tv_sec&&b.st_atim.tv_nsec==a.st_atim.tv_nsec);
 assert(!ftruncate(fd,((off_t)1<<33)+17));assert(!fstat(fd,&b)&&b.st_size==((off_t)1<<33)+17);
 assert(!clock_gettime(CLOCK_MONOTONIC,&now)&&now.tv_nsec>=0&&now.tv_nsec<1000000000);assert(!nanosleep(&req,0));
 errno=E2BIG;req.tv_sec=(time_t)INT32_MAX+1;req.tv_nsec=0;assert(clock_nanosleep(CLOCK_MONOTONIC,0,&req,0)==EOVERFLOW&&errno==E2BIG);
 assert(nanosleep(&req,0)==-1&&errno==EOVERFLOW);
 struct sigaction action={0};action.sa_handler=caught;sigemptyset(&action.sa_mask);assert(!sigaction(SIGUSR1,&action,0));
 pid_t child=fork();assert(child>=0);if(!child){struct timespec delay={0,10000000};nanosleep(&delay,0);kill(getppid(),SIGUSR1);_exit(0);}
 req.tv_sec=1;req.tv_nsec=0;errno=E2BIG;assert(clock_nanosleep(CLOCK_REALTIME,0,&req,&rest)==EINTR&&errno==E2BIG);assert(rest.tv_sec>=0&&rest.tv_sec<=1);assert(waitpid(child,0,0)==child);
 close(fd);close(d);unlink(link);unlink(path);rmdir(dir);
 puts("linux44_compat_test passed: ARM stat64/time32 layout; large files; timestamps; sleep/errors/EINTR");return 0;
}
