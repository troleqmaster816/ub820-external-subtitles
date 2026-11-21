#define _GNU_SOURCE
#define UB_NATIVE_WAIT_DIR "/tmp/ub820-native-wait-test"
static char native_test_library[1024];
#define UB_NATIVE_WAIT_LIBRARY native_test_library
#include "../core/native_wait.h"
#include <assert.h>
#include <sys/stat.h>
#include <sys/wait.h>
static void put(const char *path,const char *value){int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);size_t n=strlen(value);assert(write(fd,value,n)==(ssize_t)n);assert(!close(fd));}
int main(int argc,char **argv){
 char stat[4096],path[160],line[128];unsigned long long born;int pid=(int)getpid();ssize_t n;
 assert(ub_native_wait_read("/proc/self/stat",stat,sizeof stat)>0);
 born=ub_native_wait_birth(stat);assert(born);
 assert(!ub_native_wait_birth("truncated"));
 assert(ub_native_wait_process("apl_bdv")&&ub_native_wait_process("apl_setting")&&ub_native_wait_process("apl_meda_bdplay")&&ub_native_wait_process("apl_meda_dvdv (deleted)"));
 assert(!ub_native_wait_process("apl_medm")&&!ub_native_wait_process("apl_setting_other"));
 const char *prefix="1 (a name ) with parens) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 ";
 snprintf(stat,sizeof stat,"%s42 0",prefix);assert(ub_native_wait_birth(stat)==42);
 snprintf(stat,sizeof stat,"%s-1 0",prefix);assert(!ub_native_wait_birth(stat));
 snprintf(stat,sizeof stat,"%s18446744073709551616 0",prefix);assert(!ub_native_wait_birth(stat));
 snprintf(stat,sizeof stat,"%s42garbage 0",prefix);assert(!ub_native_wait_birth(stat));
 (void)argv;mkdir(UB_NATIVE_WAIT_DIR,0700);
 snprintf(path,sizeof path,UB_NATIVE_WAIT_DIR "/%d.ready",pid);if(argc==1)unlink(path);
 n=readlink("/proc/self/exe",native_test_library,sizeof native_test_library-1);assert(n>0);native_test_library[n]=0;
 assert(!ub_native_wait_ready(pid));
 if(argc>1){unlink(path);puts("same-PID exec cannot reuse a receipt without its CLOEXEC lease");return 0;}
 int lease=open(path,O_WRONLY|O_CREAT|O_CLOEXEC,0600);assert(lease>=0);
 snprintf(line,sizeof line,"EVW2 %d %llu %d\n",pid,born,lease);put(path,line);assert(ub_native_wait_ready(pid));
 snprintf(line,sizeof line,"EVW1 %d %llu\n",pid,born);put(path,line);assert(!ub_native_wait_ready(pid));
 snprintf(line,sizeof line,"EVW2 %d %llu %d\n",pid,born+1,lease);put(path,line);assert(!ub_native_wait_ready(pid));
 snprintf(line,sizeof line,"EVW2 %d %llu %d\nextra",pid,born,lease);put(path,line);assert(!ub_native_wait_ready(pid));
 snprintf(line,sizeof line,"EVW2 %d %llu 18446744073709551616\n",pid,born);put(path,line);assert(!ub_native_wait_ready(pid));
 snprintf(line,sizeof line,"EVW2 %d %llu -1\n",pid,born);put(path,line);assert(!ub_native_wait_ready(pid));
 snprintf(line,sizeof line,"EVW2 %d %llu %d\n",pid,born,lease);put(path,line);
 assert(!fcntl(lease,F_SETFD,0));assert(!ub_native_wait_ready(pid));assert(!fcntl(lease,F_SETFD,FD_CLOEXEC));
 int child=fork(),status;assert(child>=0);
 if(!child){
  close(lease);int child_pid=(int)getpid();
  assert(ub_native_wait_read("/proc/self/stat",stat,sizeof stat)>0);unsigned long long child_born=ub_native_wait_birth(stat);assert(child_born);
  snprintf(path,sizeof path,UB_NATIVE_WAIT_DIR "/%d.ready",child_pid);
  int child_lease=open(path,O_WRONLY|O_CREAT|O_CLOEXEC,0600);assert(child_lease>=0);
  snprintf(line,sizeof line,"EVW2 %d %llu %d\n",child_pid,child_born,child_lease);put(path,line);assert(ub_native_wait_ready(child_pid));
  execl("/proc/self/exe","native_wait_test","exec-check",(char *)0);_exit(99);
 }
 assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
 strcat(native_test_library," (deleted)");assert(!ub_native_wait_ready(pid));
 native_test_library[strlen(native_test_library)-strlen(" (deleted)")]=0;
 close(lease);assert(!ub_native_wait_ready(pid));
 unlink(path);assert(!symlink("/proc/self/stat",path));assert(!ub_native_wait_ready(pid));unlink(path);
 puts("native_wait_test passed: strict identity, stale instance, mapping and malformed readiness checks");return 0;
}
