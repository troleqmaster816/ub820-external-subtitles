/* Real launcher environment/fork branch; no player executable is launched. */
#define _GNU_SOURCE
#define UB_BD_RAM_DIR "/tmp/ub820-bd-launch-test"
#define execve bd_test_execve
#define fork bd_test_fork
#include "../modules/region/launch.c"
#undef execve
#undef fork
#include <sys/stat.h>
#include <sys/wait.h>
extern pid_t fork(void);
static int failures,original_calls,fork_fail;
#define CHECK(c) do{if(!(c)){printf("FAIL bd_launch_test:%d: %s\n",__LINE__,#c);failures++;}}while(0)
static int original(uintptr_t *d){(void)d;original_calls++;return 42;}
static const char *envvalue(char *const *e,const char *key){size_t n=strlen(key);for(;*e;e++)if(!strncmp(*e,key,n)&&(*e)[n]=='=')return *e+n+1;return 0;}
pid_t bd_test_fork(void){if(fork_fail){errno=EAGAIN;return -1;}return fork();}
int bd_test_execve(const char *p,char *const *argv,char *const *env){
 const char *policy=envvalue(env,"UB820_BD_POLICY"),*preload=envvalue(env,"LD_PRELOAD");
 if(strcmp(p,"/usr/dtvrec/bin/apl_bdv")||strcmp(argv[0],"apl_bdv")||!policy||strcmp(policy,"v1:7")||!preload||!strstr(preload,UB_BD_PRELOAD))_exit(91);
 if(write(50,"EXEC_OK",7)!=7)_exit(92);
 errno=ENOENT;return -1;
}
static void config(int policy,int extra){
 struct ub_bd_launch_config c={UB_BD_LAUNCH_MAGIC,1,policy,0};int fd=open(UB_BD_LAUNCH_CONFIG,O_WRONLY|O_CREAT|O_TRUNC,0600);
 CHECK(fd>=0);if(fd>=0){CHECK(write(fd,&c,sizeof c)==sizeof c);if(extra)CHECK(write(fd,"x",1)==1);close(fd);}
}
int main(void){
 char **e;char *argv[]={"apl_bdv","50002","0",0};uintptr_t desc[8]={0};int p[2],status;char b[16]={0};
 mkdir(UB_BD_RAM_DIR,0700);ub_original_launch=original;ub_debug_name="debug";
 desc[2]=(uintptr_t)"normal";desc[3]=(uintptr_t)"/usr/dtvrec/bin/apl_bdv";desc[4]=(uintptr_t)argv;
 unsetenv("LD_PRELOAD");e=launch_environment(7);CHECK(e&&envvalue(e,"LD_PRELOAD")&&!strcmp(envvalue(e,"UB820_BD_PRELOAD_OLD"),""));free(e);CHECK(!getenv("LD_PRELOAD"));
 setenv("LD_PRELOAD","user-first.so:" UB_BD_PRELOAD " user-second.so",1);
 e=launch_environment(1);CHECK(e&&!strcmp(envvalue(e,"UB820_BD_PRELOAD_OLD"),"user-first.so user-second.so"));
 CHECK(!strcmp(envvalue(e,"LD_PRELOAD"),"user-first.so user-second.so " UB_BD_PRELOAD));CHECK(!strcmp(envvalue(e,"UB820_BD_POLICY"),"v1:1"));free(e);
 CHECK(!strcmp(getenv("LD_PRELOAD"),"user-first.so:" UB_BD_PRELOAD " user-second.so"));
 /* Existing private variables are replaced once; large ordinary environments
  * are preserved rather than truncated to a fixed pointer limit. */
 setenv("UB820_BD_POLICY","stale",1);setenv("UB820_BD_PRELOAD_OLD","stale",1);
 for(int i=0;i<1200;i++){char key[32];snprintf(key,sizeof key,"BD_TEST_VAR_%d",i);setenv(key,"value",1);}
 e=launch_environment(-1);CHECK(e&&!strcmp(envvalue(e,"BD_TEST_VAR_1199"),"value")&&!strcmp(envvalue(e,"UB820_BD_POLICY"),"v1:auto"));free(e);
 CHECK(ub_hook_launch(0)==-1);
 desc[7]=1;CHECK(ub_hook_launch(desc)==0&&!original_calls);desc[7]=0;
 desc[3]=(uintptr_t)"/usr/dtvrec/bin/apl_meda_bdplay";CHECK(ub_hook_launch(desc)==42);desc[3]=(uintptr_t)"/usr/dtvrec/bin/apl_bdv";
 desc[2]=(uintptr_t)"debug";CHECK(ub_hook_launch(desc)==42);desc[2]=(uintptr_t)"normal";
 unlink(UB_BD_LAUNCH_CONFIG);CHECK(ub_hook_launch(desc)==42);
 {int fd=open(UB_BD_LAUNCH_CONFIG,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);if(fd>=0){CHECK(write(fd,"BRD1",4)==4);close(fd);}CHECK(ub_hook_launch(desc)==42);}
 unlink(UB_BD_LAUNCH_CONFIG);CHECK(!mkfifo(UB_BD_LAUNCH_CONFIG,0600));CHECK(ub_hook_launch(desc)==42);unlink(UB_BD_LAUNCH_CONFIG);
 config(7,1);CHECK(ub_hook_launch(desc)==42);config(3,0);CHECK(ub_hook_launch(desc)==42);
 config(0,0);CHECK(ub_hook_launch(desc)==42);
 config(7,0);fork_fail=1;CHECK(ub_hook_launch(desc)==-1&&desc[6]==(uintptr_t)-1&&desc[7]==0);fork_fail=0;
 CHECK(!pipe(p));CHECK(dup2(p[1],50)==50);close(p[1]);
 CHECK(ub_hook_launch(desc)==0&&desc[7]==1&&(pid_t)desc[6]>0);close(50);
 CHECK(waitpid((pid_t)desc[6],&status,0)==(pid_t)desc[6]&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 CHECK(read(p[0],b,sizeof b)==7&&!strcmp(b,"EXEC_OK"));close(p[0]);
 CHECK(!strcmp(getenv("UB820_BD_POLICY"),"stale"));
 unsetenv("LD_PRELOAD");unsetenv("UB820_BD_POLICY");unsetenv("UB820_BD_PRELOAD_OLD");
 printf("bd_launch_test %s\n",failures?"FAILED":"passed");return failures?1:0;
}
