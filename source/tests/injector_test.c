/* Real host ptrace regression tests, using child processes only. Never attaches
 * to the player or exercises the ARM remote-syscall bootstrap on x86. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static int inject_signal,hold_wait,tracked_pid,fail_write,deny_poketext;
static int emulate_arm,arm_cont,arm_event,arm_timeout,arm_mask_fail;
static uint32_t arm_regs[18],arm_saved[18];static uint64_t arm_mask;
static long test_ptrace(enum __ptrace_request req,...){
 va_list ap;va_start(ap,req);int tid=va_arg(ap,int);void *addr=va_arg(ap,void *),*data=va_arg(ap,void *);va_end(ap);
 if(emulate_arm){
  if(req==PTRACE_GETREGS){memcpy(data,arm_regs,sizeof arm_regs);return 0;}
  if(req==PTRACE_SETREGS){memcpy(arm_regs,data,sizeof arm_regs);return 0;}
  if(req==PTRACE_GETSIGMASK){memcpy(data,&arm_mask,8);return 0;}
  if(req==PTRACE_SETSIGMASK){if(arm_mask_fail){errno=EIO;return -1;}memcpy(&arm_mask,data,8);return 0;}
  if(req==PTRACE_CONT){++arm_cont;arm_event=arm_timeout?0:1;if(arm_cont==2){arm_regs[15]+=2;arm_regs[0]=0x12345000;}return 0;}
  if(req==PTRACE_INTERRUPT){arm_event=1;arm_regs[15]=(arm_saved[15]&~3u)+4;return 0;}
 }
 if(req==PTRACE_POKETEXT&&deny_poketext){errno=EIO;return -1;}
 long r=syscall(SYS_ptrace,req,tid,addr,data);
 if(!r&&req==PTRACE_SEIZE&&inject_signal){syscall(SYS_tgkill,tid,tid,SIGUSR1);usleep(1000);}
 return r;
}
static pid_t test_waitpid(pid_t pid,int *status,int flags){if(emulate_arm&&pid==tracked_pid){if(!arm_event)return 0;arm_event=0;*status=(SIGTRAP<<8)|0x7f;return pid;}if(hold_wait&&pid==tracked_pid)return 0;return (pid_t)syscall(SYS_wait4,pid,status,flags,0);}
static ssize_t test_pwrite(int fd,const void *bytes,size_t n,off_t at){if(fail_write<0||(fail_write>0&&--fail_write==0)){errno=EIO;return -1;}return (ssize_t)syscall(SYS_pwrite64,fd,bytes,n,at);}
#define ptrace test_ptrace
#define waitpid test_waitpid
#define pwrite test_pwrite
#include "../core/injector.c"
#undef ptrace
#undef waitpid
#undef pwrite
static int failures;static volatile int *shared;
#define CHECK(c) do{if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c);failures++;}}while(0)
static void usr1(int sig){(void)sig;shared[0]++;}
static int child(void){
 shared[0]=0;shared[1]=0;int pid=fork();if(!pid){signal(SIGUSR1,usr1);shared[1]=1;for(;;)pause();}
 while(!shared[1])usleep(100);
 return pid;
}
static void end_child(int pid){kill(pid,SIGKILL);waitpid(pid,0,0);}
static char child_state(int pid){char p[64],b[4096];snprintf(p,sizeof p,"/proc/%d/status",pid);int fd=open(p,O_RDONLY);int n=read(fd,b,sizeof b-1);close(fd);if(n<0)return '?';b[n]=0;char *s=strstr(b,"State:");return s?s[7]:'?';}
static int failed_callback(int pid,int mem,void *arg){(void)pid;(void)mem;(void)arg;return -2;}
static unsigned char *vm;static int vm_maps,vm_moves,vm_fail_move,vm_fail_recovery;
static int vm_syscall(struct stopped *stop,int pid,uint32_t number,const uint32_t args[6],uint32_t *result){
 (void)stop;(void)pid;*result=0;
 if(number==192){*result=(uint32_t)(uintptr_t)(vm+4096*(++vm_maps));return 0;}
 if(number==125||number==91)return 0;
 if(number==163){
  ++vm_moves;
  /* Model the old kernel's destructive failure after do_munmap(target). The
   * shared child pages expose exactly the bytes seen by real /proc/pid/mem. */
  memset(vm,0,4096);
  if((vm_moves==1&&vm_fail_move)||(vm_moves==2&&vm_fail_recovery)){*result=(uint32_t)-ENOMEM;return 0;}
  memcpy(vm,(void *)(uintptr_t)args[0],args[1]);*result=args[4];return 0;
 }
 return -1;
}
int main(void){
 shared=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(shared!=MAP_FAILED);
 int pid=child();struct stopped s;inject_signal=1;
 CHECK(stop_threads(pid,&s)==0);inject_signal=0;CHECK(s.signal[0]==SIGUSR1);CHECK(shared[0]==0);
 if(s.mem>=0)close(s.mem);
 detach_all(&s);usleep(10000);CHECK(shared[0]==1);CHECK(child_state(pid)!='T');CHECK(!deferred.n);end_child(pid);
 pid=child();kill(pid,SIGSTOP);int status;waitpid(pid,&status,WUNTRACED);CHECK(WIFSTOPPED(status));
 CHECK(stop_threads(pid,&s)==0);if(s.mem>=0)close(s.mem);
 detach_all(&s);usleep(10000);CHECK(child_state(pid)=='T');kill(pid,SIGCONT);end_child(pid);
 pid=child();tracked_pid=pid;hold_wait=1;unsigned long long start=ub_now_ms();
 CHECK(stop_threads(pid,&s)==-1);CHECK(ub_now_ms()-start<1500);CHECK(deferred.n==1);
 hold_wait=0;ub_injector_poll();CHECK(!deferred.n);usleep(10000);CHECK(child_state(pid)!='T');end_child(pid);
 pid=child();CHECK(ub_while_stopped(pid,failed_callback,0)==-2);usleep(10000);CHECK(child_state(pid)=='T');end_child(pid);
 /* Real /proc child memory plus executable mapping. Multiple bytes including
  * an odd-address string; rejecting a later site must leave earlier ones alone. */
 unsigned char *code=mmap((void *)0x10000000,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(code!=MAP_FAILED);CHECK((uintptr_t)code<=UINT32_MAX);
 memset(code,0x11,4096);unsigned char old[6],next[6],bad[6];memset(old,0x11,6);memset(next,0x22,6);memset(bad,0x33,6);
 pid=child();struct ub_code_patch patches[2]={{(uint32_t)(uintptr_t)code+1,6,old,0,next},{(uint32_t)(uintptr_t)code+21,6,bad,0,next}};
 CHECK(ub_patch_code(pid,patches,2)==1);CHECK(strstr(ub_patch_stage(),"unknown bytes"));CHECK(!memcmp(code+1,old,6));
 patches[1].expected=old;CHECK(ub_patch_code(pid,patches,2)==0);CHECK(!memcmp(code+1,next,6));CHECK(!memcmp(code+21,next,6));CHECK(ub_patch_code(pid,patches,2)==0);
 patches[0].replacement=old;patches[1].replacement=old;patches[0].expected=next;patches[1].expected=next;
 fail_write=2;CHECK(ub_patch_code(pid,patches,2)==-1);CHECK(strstr(ub_patch_stage(),"rollback verified"));CHECK(!memcmp(code+1,next,6));CHECK(!memcmp(code+21,next,6));CHECK(child_state(pid)!='T');
 CHECK(ub_patch_code(pid,patches,2)==0);CHECK(!memcmp(code+1,old,6));
 patches[0].expected=old;patches[0].replacement=next;fail_write=-1;
 CHECK(ub_patch_code(pid,patches,1)==-1);CHECK(!memcmp(code+1,old,6));CHECK(code[0]==0x11&&code[7]==0x11);CHECK(child_state(pid)!='T');
 deny_poketext=1;CHECK(ub_patch_code(pid,patches,1)==-2);usleep(10000);CHECK(child_state(pid)=='T');fail_write=deny_poketext=0;end_child(pid);
 /* The child executes an x86 short branch inside the intended patch window.
  * The real architecture-aware PC read must reject without changing bytes. */
 code[0]=0xeb;code[1]=0xfe;unsigned char spinning[2]={0xeb,0xfe},nop[2]={0x90,0x90};
 pid=fork();if(!pid){((void (*)(void))code)();_exit(0);}usleep(10000);
 struct ub_code_patch busy={(uint32_t)(uintptr_t)code,2,spinning,0,nop};CHECK(ub_patch_code(pid,&busy,1)==-1);CHECK(strstr(ub_patch_stage(),"busy TID"));CHECK(!memcmp(code,spinning,2));end_child(pid);munmap(code,4096);
 /* Emulate only the ARM CPU/ptrace register transport, keeping actual child
  * memory and injector state machine. Cover sync BKPT, masks, and a final BKPT
  * racing the timeout interrupt; the private SIGTRAP must never be reinjected. */
 unsigned char *stubmap=mmap((void *)0x12000000,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(stubmap!=MAP_FAILED);memset(stubmap,0x66,4096);
 pid=child();CHECK(stop_threads(pid,&s)==0);tracked_pid=pid;memset(arm_regs,0,sizeof arm_regs);arm_regs[15]=(uint32_t)(uintptr_t)stubmap;arm_regs[7]=99;arm_regs[17]=123;memcpy(arm_saved,arm_regs,sizeof arm_regs);arm_mask=0x1234;arm_cont=arm_event=0;emulate_arm=1;
 uint32_t args[6]={1,2,3,4,5,6},result=0;CHECK(remote_syscall(&s,pid,192,args,&result)==0);CHECK(result==0x12345000);CHECK(!memcmp(arm_regs,arm_saved,sizeof arm_regs));CHECK(arm_mask==0x1234);CHECK(stubmap[0]==0x66&&stubmap[7]==0x66);
 arm_timeout=1;arm_cont=arm_event=0;s.until=ub_now_ms()+100;CHECK(remote_syscall(&s,pid,192,args,&result)==-1);CHECK(!s.quarantine);CHECK(s.signal[0]==0);CHECK(!memcmp(arm_regs,arm_saved,sizeof arm_regs));CHECK(arm_mask==0x1234);CHECK(stubmap[0]==0x66&&stubmap[7]==0x66);
 arm_timeout=0;arm_mask_fail=1;s.until=ub_now_ms()+500;CHECK(remote_syscall(&s,pid,192,args,&result)==-1);CHECK(stubmap[0]==0x66);CHECK(!s.quarantine);arm_mask_fail=0;emulate_arm=0;
 if(s.mem>=0)close(s.mem);
 detach_all(&s);end_child(pid);munmap(stubmap,4096);
 vm=mmap((void *)0x11000000,4096*3,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(vm!=MAP_FAILED);CHECK((uintptr_t)vm<=UINT32_MAX);
 unsigned char *orig=malloc(4096),*replacement=malloc(4096);memset(orig,0x44,4096);memset(replacement,0x55,4096);
 memcpy(vm,orig,4096);pid=child();remote_syscall_test=vm_syscall;vm_maps=vm_moves=0;vm_fail_move=1;
 CHECK(ub_overlay_pages(pid,(uint32_t)(uintptr_t)vm,orig,replacement,4096)==UB_OVERLAY_SWAP);CHECK(vm_moves==2);CHECK(!memcmp(vm,orig,4096));CHECK(child_state(pid)!='T');end_child(pid);
 memcpy(vm,orig,4096);pid=child();vm_maps=vm_moves=0;vm_fail_recovery=1;
 CHECK(ub_overlay_pages(pid,(uint32_t)(uintptr_t)vm,orig,replacement,4096)==UB_OVERLAY_RECOVER);usleep(10000);CHECK(child_state(pid)=='T');end_child(pid);
 memcpy(vm,orig,4096);pid=child();vm_maps=vm_moves=0;vm_fail_move=vm_fail_recovery=0;
 CHECK(ub_overlay_pages(pid,(uint32_t)(uintptr_t)vm,orig,replacement,4096)==0);CHECK(!memcmp(vm,replacement,4096));CHECK(child_state(pid)!='T');end_child(pid);
 remote_syscall_test=0;free(orig);free(replacement);munmap(vm,4096*3);
 puts(failures?"injector_test FAILED":"injector_test passed");return failures!=0;
}
