#define _GNU_SOURCE
#define UB_BD_RAM_DIR "/tmp/ub820-mgr-launch-test"
#define UB_NATIVE_MGR_LAUNCH
#define execve mgr_test_execve
#define fork mgr_test_fork
#include "../modules/region/launch.c"
#undef execve
#undef fork
#include <assert.h>
#include <sys/stat.h>
#include <sys/wait.h>
extern pid_t fork(void);
static unsigned delegated,started,waited;static uint32_t notified;
static int fork_error;
static int original(uintptr_t *d){(void)d;delegated++;return 42;}
static void did_start(uint32_t id){started++;notified=id;}
static void did_wait(uint32_t id){waited++;notified=id;}
pid_t mgr_test_fork(void){if(fork_error){errno=EAGAIN;return -1;}return fork();}
int mgr_test_execve(const char *p,char *const *argv,char *const *env){
 int preload=0,policy=0,wait=0;
 if(strcmp(p,"/usr/dtvrec/bin/apl_setting")||strcmp(argv[0],"apl_setting"))_exit(91);
 for(;*env;env++){
  if(!strncmp(*env,"LD_PRELOAD=",11)&&strstr(*env,UB_BD_PRELOAD))preload++;
  if(!strcmp(*env,"UB820_BD_POLICY=v1:0"))policy++;
  if(!strcmp(*env,UB_NATIVE_WAIT_ENV "=" UB_NATIVE_WAIT_VALUE))wait++;
 }
 if(preload!=1||policy!=1||wait!=1||write(50,"OK",2)!=2)_exit(92);
 errno=ENOENT;return -1;
}
int main(void){
 uintptr_t d[8]={0};char *argv[]={"apl_setting",0};struct ub_bd_launch_config c={UB_BD_LAUNCH_MAGIC,1,7,0};
 int fd,p[2],status;char token[2];
 mkdir(UB_BD_RAM_DIR,0700);unlink(UB_BD_LAUNCH_CONFIG);unsetenv("LD_PRELOAD");
 ub_original_launch=original;ub_debug_name="debug";ub_mgr_started=did_start;ub_mgr_started_wait=did_wait;
 d[1]=0xc353;d[3]=(uintptr_t)"/usr/dtvrec/bin/apl_setting";d[4]=(uintptr_t)argv;d[5]=(uintptr_t)"normal";
 assert(ub_hook_launch(0)==-1);assert(ub_hook_launch(d)==42);
 fd=open(UB_BD_LAUNCH_CONFIG,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0&&write(fd,&c,sizeof c)==sizeof c);close(fd);
 d[7]=3;assert(ub_hook_launch(d)==42);d[7]=1;assert(!ub_hook_launch(d));d[7]=2;assert(!ub_hook_launch(d));d[7]=0;
 d[5]=(uintptr_t)"debug";assert(ub_hook_launch(d)==42);d[5]=(uintptr_t)"normal";
 d[3]=(uintptr_t)"/usr/dtvrec/bin/apl_dispsrv";assert(ub_hook_launch(d)==42);d[3]=(uintptr_t)"/usr/dtvrec/bin/apl_setting";
 fork_error=1;assert(ub_hook_launch(d)==-1&&d[6]==(uintptr_t)-1&&!d[7]&&!started&&!waited);fork_error=0;
 for(unsigned i=0;i<2;i++){
  d[0]=i?0x100:2;d[7]=0;assert(!pipe(p));assert(dup2(p[1],50)==50);close(p[1]);
  assert(!ub_hook_launch(d)&&d[7]==(i?2u:1u));close(50);
  assert(waitpid((pid_t)d[6],&status,0)==(pid_t)d[6]&&WIFEXITED(status)&&!WEXITSTATUS(status));
  assert(read(p[0],token,2)==2&&!memcmp(token,"OK",2));close(p[0]);
  assert(notified==0xc353&&started==1&&waited==i);
  assert(!getenv("LD_PRELOAD"));
 }
 assert(delegated==4);unlink(UB_BD_LAUNCH_CONFIG);
 puts("mgr_launch_test passed: Settings only, state/debug fallback, fork/exec environment and both native parent notifications");return 0;
}
