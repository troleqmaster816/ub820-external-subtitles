/* Real host ptrace regression tests, using child processes only. Never attaches
 * to the player or exercises the ARM remote-syscall bootstrap on x86. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/sem.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static int inject_signal,hold_wait,tracked_pid,fail_write,deny_poketext,seizes;
static int emulate_arm,arm_cont,arm_event,arm_timeout,arm_mask_fail;
static uint32_t arm_regs[18],arm_saved[18];static uint64_t arm_mask;
/* Files the injector opens and bytes it reads, to show what it never touches. */
static char opened[16][160];static unsigned open_count;static long file_bytes;
static int test_open(const char *p,int flags,...){
 int mode=0;if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap);}
 snprintf(opened[open_count++%16],sizeof opened[0],"%s",p);return (int)syscall(SYS_openat,AT_FDCWD,p,flags,mode);
}
static ssize_t test_read(int fd,void *b,size_t n){ssize_t k=(ssize_t)syscall(SYS_read,fd,b,n);if(k>0)file_bytes+=k;return k;}
static ssize_t test_pread(int fd,void *b,size_t n,off_t at){ssize_t k=(ssize_t)syscall(SYS_pread64,fd,b,n,at);if(k>0)file_bytes+=k;return k;}
static int was_opened(const char *p){for(unsigned i=0;i<open_count&&i<16;i++)if(!strcmp(opened[i],p))return 1;return 0;}
/* A directory read that fails at the end of every pass (dir_fail). */
struct ub_dir;
static int test_dir_read(struct ub_dir *d,const char **name);
static long test_ptrace(enum __ptrace_request req,...){
 va_list ap;va_start(ap,req);int tid=va_arg(ap,int);void *addr=va_arg(ap,void *),*data=va_arg(ap,void *);va_end(ap);
 if(req==PTRACE_SEIZE)seizes++;
 if(emulate_arm){
  if(req==PTRACE_GETREGS){memcpy(data,arm_regs,sizeof arm_regs);return 0;}
  if(req==PTRACE_SETREGS){memcpy(arm_regs,data,sizeof arm_regs);return 0;}
  if(req==PTRACE_GETSIGMASK){memcpy(data,&arm_mask,8);return 0;}
  if(req==PTRACE_SETSIGMASK){if(arm_mask_fail){errno=EIO;return -1;}memcpy(&arm_mask,data,8);return 0;}
  if(req==PTRACE_CONT){++arm_cont;arm_event=arm_timeout?0:1;if(arm_cont==2){arm_regs[15]+=2;arm_regs[0]=0x12345000;}return 0;}
  if(req==PTRACE_INTERRUPT){arm_event=1;arm_regs[15]=(arm_saved[15]&~3u)+4;return 0;}
 }
 if(req==PTRACE_POKETEXT&&deny_poketext){errno=EIO;return -1;}
 /* libc's PEEK interface returns the word; the raw Linux syscall writes it
  * through data. Model that transport for the read-only stop diagnostic. */
 if(req==PTRACE_PEEKTEXT){long word;if(syscall(SYS_ptrace,req,tid,addr,&word)<0)return -1;return word;}
 long r=syscall(SYS_ptrace,req,tid,addr,data);
 if(!r&&req==PTRACE_SEIZE&&inject_signal){syscall(SYS_tgkill,tid,tid,SIGUSR1);usleep(1000);}
 return r;
}
static pid_t test_waitpid(pid_t pid,int *status,int flags){if(emulate_arm&&pid==tracked_pid){if(!arm_event)return 0;arm_event=0;*status=(SIGTRAP<<8)|0x7f;return pid;}if(hold_wait&&pid==tracked_pid)return 0;return (pid_t)syscall(SYS_wait4,pid,status,flags,0);}
static ssize_t test_pwrite(int fd,const void *bytes,size_t n,off_t at){if(fail_write<0||(fail_write>0&&--fail_write==0)){errno=EIO;return -1;}return (ssize_t)syscall(SYS_pwrite64,fd,bytes,n,at);}
#define ptrace test_ptrace
#define waitpid test_waitpid
#define pwrite test_pwrite
#define open test_open
#define read test_read
#define pread test_pread
#define ub_dir_read test_dir_read
#include "../core/injector.c"
#undef ptrace
#undef waitpid
#undef pwrite
#undef open
#undef read
#undef pread
#undef ub_dir_read
int ub_dir_read(struct ub_dir *d,const char **name);
static int dir_fail;
static int test_dir_read(struct ub_dir *d,const char **name){int rc=ub_dir_read(d,name);if(!rc&&dir_fail){d->err=errno=EIO;return -1;}return rc;}
static int failures;static volatile int *shared;
#define CHECK(c) do{if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c);failures++;}}while(0)
static void usr1(int sig){(void)sig;shared[0]++;}
static int child(void){
 shared[0]=0;shared[1]=0;int pid=fork();if(!pid){signal(SIGUSR1,usr1);shared[1]=1;for(;;)pause();}
 while(!shared[1])usleep(100);
 return pid;
}
static int unprotected_bd_child(const char *name){
 shared[0]=shared[1]=0;int pid=fork();
 if(!pid){prctl(PR_SET_NAME,name);shared[1]=1;for(;;)pause();}
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
/* A child sleeping in semop on sem (count 0); shared[2] gets 1 on success or
 * -errno. With a SIGUSR1 handler when handler is set. */
static int sem_child(int sem,int handler){
 shared[0]=0;shared[1]=0;shared[2]=0;int pid=fork();
 if(!pid){struct sembuf b={0,-1,0};if(handler)signal(SIGUSR1,usr1);shared[1]=1;int r=(int)syscall(SYS_semop,sem,&b,1);shared[2]=r?-errno:1;for(;;)pause();}
 while(!shared[1])usleep(100);
 /* until the kernel reports it inside semop/semtimedop */
 for(int i=0;i<2000;i++){char p[64],b[64];snprintf(p,sizeof p,"/proc/%d/syscall",pid);int fd=open(p,O_RDONLY),n=fd<0?-1:(int)read(fd,b,sizeof b-1);if(fd>=0)close(fd);if(n>0){b[n]=0;long nr=atol(b);if(nr==SYS_semop||nr==SYS_semtimedop)break;}usleep(500);}
 usleep(2000);return pid;
}
static void sem_post_one(int sem){struct sembuf b={0,1,0};semop(sem,&b,1);}
static int wait_shared(int i){for(int k=0;k<2000&&!shared[i];k++)usleep(500);return shared[i];}
/* Minimal ARM ET_DYN file: one PT_LOAD at offset 0, .dynsym with the given
 * defined functions. Enough for load_file/module_list/resolve. */
static int fake_lib(const char *path,const char *const *names,const uint32_t *values,unsigned n){
 static unsigned char f[4096];memset(f,0,sizeof f);
 Elf32_Ehdr *eh=(Elf32_Ehdr *)f;Elf32_Phdr *ph=(Elf32_Phdr *)(f+52);char *str=(char *)f+128;Elf32_Sym *sym=(Elf32_Sym *)(f+640);Elf32_Shdr *sh=(Elf32_Shdr *)(f+1024);unsigned so=1;
 memcpy(eh->e_ident,ELFMAG,4);eh->e_ident[EI_CLASS]=ELFCLASS32;eh->e_ident[EI_DATA]=ELFDATA2LSB;eh->e_ident[EI_VERSION]=EV_CURRENT;
 eh->e_type=ET_DYN;eh->e_machine=EM_ARM;eh->e_version=EV_CURRENT;eh->e_phoff=52;eh->e_shoff=1024;eh->e_ehsize=52;eh->e_phentsize=sizeof *ph;eh->e_phnum=1;eh->e_shentsize=sizeof *sh;eh->e_shnum=3;
 ph->p_type=PT_LOAD;ph->p_filesz=ph->p_memsz=sizeof f;ph->p_flags=PF_R|PF_X;ph->p_align=4096;
 for(unsigned i=0;i<n;i++){size_t l=strlen(names[i])+1;memcpy(str+so,names[i],l);sym[i+1].st_name=so;so+=(unsigned)l;sym[i+1].st_value=values[i];sym[i+1].st_info=ELF32_ST_INFO(STB_GLOBAL,STT_FUNC);sym[i+1].st_shndx=1;}
 sh[1].sh_type=SHT_DYNSYM;sh[1].sh_flags=SHF_ALLOC;sh[1].sh_offset=640;sh[1].sh_size=(n+1)*sizeof *sym;sh[1].sh_link=2;sh[1].sh_entsize=sizeof *sym;
 sh[2].sh_type=SHT_STRTAB;sh[2].sh_offset=128;sh[2].sh_size=so;
 int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -1;int ok=write(fd,f,sizeof f)==(ssize_t)sizeof f;close(fd);return ok?0:-1;
}
static void *map_file(const char *path,uintptr_t at){int fd=open(path,O_RDONLY);void *m=mmap((void *)at,4096,PROT_READ,MAP_PRIVATE,fd,0);close(fd);return m;}
static int fd_count(void){int n=0;DIR *d=opendir("/proc/self/fd");struct dirent *e;while(d&&(e=readdir(d)))n++;if(d)closedir(d);return n;}
int main(void){
 shared=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(shared!=MAP_FAILED);
 /* All callers share this precondition; refusing an unprotected BD process
  * must happen before even SEIZE, including transactions that would fail. */
 {const char *names[]={"apl_bdv","apl_setting","apl_meda_bdplay","apl_meda_dvdv"};for(unsigned i=0;i<4;i++){
  int unprotected=unprotected_bd_child(names[i]),before=seizes;struct stopped rejected;
  CHECK(stop_threads(unprotected,&rejected)==-1&&errno==EAGAIN&&seizes==before);
  CHECK(ub_while_stopped(unprotected,failed_callback,0)==-1&&seizes==before);
  CHECK(child_state(unprotected)!='T');end_child(unprotected);}}
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
 deny_poketext=1;CHECK(ub_patch_code(pid,patches,1)==-1);usleep(10000);CHECK(child_state(pid)!='T');CHECK(!memcmp(code+1,old,6));fail_write=deny_poketext=0;end_child(pid);
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

 /* Thread scan: a pass ending in a read error is incomplete, even when it adds
  * no thread. The transaction is deferred and nothing stays stopped. */
 pid=child();dir_fail=1;errno=0;
 CHECK(stop_threads(pid,&s)==-1);CHECK(errno==EIO);dir_fail=0;usleep(10000);CHECK(child_state(pid)!='T');CHECK(!deferred.n);
 CHECK(stop_threads(pid,&s)==0);if(s.mem>=0)close(s.mem);detach_all(&s);end_child(pid);

 /* semop interrupted by our own stop repeats on resume: the child keeps
  * waiting and later gets the semaphore instead of EINTR. */
 CHECK(repeat_call(NR_SEMOP,-EINTR,1)&&!repeat_call(NR_SEMTIMEDOP,-EINTR,1));
 CHECK(!repeat_call(NR_SEMOP,-EINTR,0)&&!repeat_call(NR_SEMOP,0,1)&&!repeat_call(NR_SEMOP,-512,1)&&!repeat_call(SYS_read,-EINTR,1));
 int sem=semget(IPC_PRIVATE,1,IPC_CREAT|0600);CHECK(sem>=0);CHECK(!semctl(sem,0,SETVAL,0));
 pid=sem_child(sem,0);
 CHECK(stop_threads(pid,&s)==0);
 {struct user_regs_struct r;CHECK(!ptrace(PTRACE_GETREGS,pid,0,&r));CHECK(r.rax==r.orig_rax&&(r.orig_rax==SYS_semop||r.orig_rax==SYS_semtimedop));}
 if(s.mem>=0){close(s.mem);}detach_all(&s);usleep(50000);
 CHECK(shared[2]==0);CHECK(child_state(pid)!='T');
 /* and again while it is still waiting: the rewound call is an ordinary wait */
 CHECK(stop_threads(pid,&s)==0);if(s.mem>=0)close(s.mem);detach_all(&s);usleep(20000);CHECK(shared[2]==0);
 sem_post_one(sem);CHECK(wait_shared(2)==1);end_child(pid);
 /* A foreign signal that interrupted the wait keeps its EINTR. */
 CHECK(!semctl(sem,0,SETVAL,0));pid=sem_child(sem,1);inject_signal=1;
 CHECK(stop_threads(pid,&s)==0);inject_signal=0;CHECK(s.signal[0]==SIGUSR1);
 if(s.mem>=0){close(s.mem);}detach_all(&s);
 CHECK(wait_shared(2)==-EINTR);CHECK(shared[0]==1);end_child(pid);
 /* Finite semtimedop must retain EINTR instead of restarting its full relative
  * deadline. Exercise the real host syscall and inspect the stopped result. */
 CHECK(!semctl(sem,0,SETVAL,0));shared[1]=shared[2]=0;pid=fork();
 if(!pid){struct sembuf b={0,-1,0};struct timespec timeout={20,0};shared[1]=1;
  int r=(int)syscall(SYS_semtimedop,sem,&b,1,&timeout);shared[2]=r?-errno:1;for(;;)pause();}
 while(!shared[1])usleep(100);
 usleep(10000);CHECK(stop_threads(pid,&s)==0);
 {struct user_regs_struct r;CHECK(!ptrace(PTRACE_GETREGS,pid,0,&r));CHECK((long)r.rax==-EINTR&&r.orig_rax==SYS_semtimedop);}
 if(s.mem>=0){close(s.mem);}detach_all(&s);CHECK(wait_shared(2)==-EINTR);end_child(pid);
 semctl(sem,0,IPC_RMID);

 /* load_file: a device node is never opened; a large non-ELF regular file is
  * rejected from its header, not read whole. */
 char dir[]="/tmp/ub820-injector-XXXXXX",path[200];CHECK(mkdtemp(dir)!=0);
 struct elf e;open_count=0;
 CHECK(load_file("/dev/zero",&e)==-1);CHECK(!was_opened("/dev/zero"));
 snprintf(path,sizeof path,"%s/big.dat",dir);
 {int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);static char junk[1<<20];memset(junk,'x',sizeof junk);CHECK(write(fd,junk,sizeof junk)==(ssize_t)sizeof junk);close(fd);}
 file_bytes=0;CHECK(load_file(path,&e)==-1);CHECK(file_bytes<=(long)sizeof(Elf32_Ehdr));
 unlink(path);

 /* module_list with nothing usable (an x86 process) closes its mem fd. */
 pid=child();{int before=fd_count();struct modules ms;CHECK(module_list(pid,&ms)==-1);CHECK(fd_count()==before);}end_child(pid);

 /* resolve: ld.so below libc (as on the player) never supplies malloc/free;
  * pthread names come from libpthread; ld.so-only names still resolve.
  * Each file is loaded once by module_list, never again per import. */
 static const char *const ld_names[]={"malloc","free","__tls_get_addr"},*const c_names[]={"malloc","free","pthread_mutex_lock","memcpy"},*const pt_names[]={"pthread_mutex_lock"};
 static const uint32_t ld_values[]={0x101,0x111,0x121},c_values[]={0x201,0x211,0x221,0x231},pt_values[]={0x301};
 char ld[200],libc[200],pt[200];
 snprintf(ld,sizeof ld,"%s/ld-2.20-test.so",dir);snprintf(libc,sizeof libc,"%s/libc-test.so",dir);snprintf(pt,sizeof pt,"%s/libpthread-test.so",dir);
 CHECK(!fake_lib(ld,ld_names,ld_values,3)&&!fake_lib(libc,c_names,c_values,4)&&!fake_lib(pt,pt_names,pt_values,1));
 void *mld=map_file(ld,0x14000000),*mc=map_file(libc,0x14100000),*mpt=map_file(pt,0x14200000);
 void *dev=mmap((void *)0x14300000,4096,PROT_READ,MAP_PRIVATE,open("/dev/zero",O_RDONLY),0);
 CHECK(mld==(void *)0x14000000&&mc==(void *)0x14100000&&mpt==(void *)0x14200000&&dev!=MAP_FAILED);
 pid=child();
 {struct modules ms;open_count=0;CHECK(module_list(pid,&ms)==0);CHECK(ms.n==3);CHECK(!was_opened("/dev/zero"));
  open_count=0;
  CHECK(resolve(&ms,"malloc")==0x14100201);CHECK(resolve(&ms,"free")==0x14100211);CHECK(resolve(&ms,"memcpy")==0x14100231);
  CHECK(resolve(&ms,"pthread_mutex_lock")==0x14200301);CHECK(resolve(&ms,"__tls_get_addr")==0x14000121);CHECK(!resolve(&ms,"absent"));
  CHECK(!open_count);
  module_free(&ms);CHECK(ms.mem==-1);}
 end_child(pid);munmap(mld,4096);munmap(mc,4096);munmap(mpt,4096);munmap(dev,4096);unlink(ld);unlink(libc);unlink(pt);rmdir(dir);

 /* thread-busy: a thread that exited while being stopped (tid 0) is skipped,
  * not a failed register read that costs an install attempt. */
 {static const unsigned char x[4]={1,2,3,4};struct ub_hook_spec h={0x10000,4,"r","o",x,x,0};struct ub_payload_variant v={"1.82","",&h,1,0,0,0};
  pid=child();CHECK(stop_threads(pid,&s)==0);s.tids[s.n]=0;s.n++;CHECK(prefix_busy(&s,&v)==0);s.n--;
  if(s.mem>=0){close(s.mem);}detach_all(&s);end_child(pid);}

 /* Hook sites that are not stock are refused before any stop: no SEIZE. */
 {char sha[65];CHECK(!ub_hash_file("/proc/self/exe",sha));
  unsigned char *site=mmap((void *)0x15000000,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(site==(void *)0x15000000);
  memcpy(site,"\x2d\xe9\xf0\x41",4);
  static const unsigned char stock[4]={0x2d,0xe9,0xf0,0x41},other[4]={0xdf,0xf8,0x00,0xf0};
  struct ub_hook_spec h={0x15000000,4,"r","o",other,other,0},hu={0x16000000,4,"r","o",stock,stock,0};
  struct ub_payload_variant v={"1.82",sha,&h,1,0,0,0},vu={"1.82",sha,&hu,1,0,0,0};
  struct ub_payload p={"test",sha,0,0,&v,1},pu={"test",sha,0,0,&vu,1};
  pid=child();seizes=0;
  CHECK(ub_hook_sites(pid,&p,sha)==1);CHECK(strstr(ub_install_stage(),"not stock"));
  CHECK(ub_install_payload(pid,&p)==1);CHECK(!seizes);CHECK(child_state(pid)!='T');
  CHECK(ub_hook_sites(pid,&pu,sha)==-1);CHECK(strstr(ub_install_stage(),"unreadable"));
  CHECK(ub_install_payload(pid,&pu)==-1);CHECK(!seizes);
  h.expected=stock;CHECK(ub_hook_sites(pid,&p,sha)==0);
  end_child(pid);munmap(site,4096);}
 puts(failures?"injector_test FAILED":"injector_test passed");return failures!=0;
}
