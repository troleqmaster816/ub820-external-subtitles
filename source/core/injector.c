/* Version-pinned ARM32 native payload loader. No flash/filesystem firmware writes.
 * Payloads have no constructors. All UI/native calls occur on subsequent natural
 * hooked calls, never inside the ptrace bootstrap. Only mmap/cacheflush syscalls
 * execute while all enumerated process threads are stopped.
 */
#define _GNU_SOURCE
#include "injector.h"
#include "native_wait.h"
#include "log.h"
#include "sys.h"
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <sys/user.h>
#endif
#ifndef PTRACE_GETSIGMASK
#define PTRACE_GETSIGMASK 0x420a
#endif
#ifndef PTRACE_SETSIGMASK
#define PTRACE_SETSIGMASK 0x420b
#endif

/* Small self-contained SHA256; hashes the immutable executable file, not RAM. */
#include "sha256.h"
/* Firmware 1.85 files the helper accepts for a 1.82 pin, in two kinds.
 * CODE: .text/.rodata/.data are byte for byte those of 1.82 (only prelink
 * metadata differ; apl_dispsrv also has text IDs in 60 functions that no
 * payload hooks, calls or reads). A payload's code may be injected into
 * these, and every hook and patch site was compared.
 * DATA: the early code is the same but data/BSS addresses shifted, so only a
 * module that reads and writes data by version (no injected payload) may use
 * them. apl_bdv 1.85: its code grew, but FUN_000a3e14/FUN_0004aea0 keep their
 * addresses and the region slots moved +0xF0 (PSR20 0xb35b6c->0xb35c5c, drive
 * region 0xaeaec0->0xaeafb0), which the region module handles.
 * apl_mgr and apl_setting changed in 1.85: their payloads carry their own
 * 1.85 addresses (manifest "variants", ub_payload_variant), so they are not
 * listed here. apl_lang.dat changed and is refused. */
static const char *const firmware_185_code[][2]={
 {"3ca762ac044acba426c45be240075f2644fc9f645a0fbbb1aabc4c8656dcec1d","d5b3a79dcaf3fbc9248131a5758db4edd8cc93139eef5c25c562f37ee823122b"}, /* apl_dispsrv */
 {"a07b82bc4a3a50594ee97ce9ad6937f8f63b129da0779c15576691e06da9789b","62dab1143d4c65f7a099130f58a19b72b48ed676c0bf4122d294904f91756573"}, /* apl_meda_bdplay */
 {"8ab24aac32c99775c2452d87cd5864b25f66e092cdebef3411100735df75835e","9942a855ebec8d85a12353f27d25b8bc8d7b29ac24bb86442c2544282e0b072e"}, /* apl_meda_dvdv */
 {"a7692f0fa0e7e3dfb3cd90c69a7e9120204ab2c9b55ae3eea9b22e6425b6fbc3","fefee7cc5b3a1e4d1ebed3248cb554c9ff434f1667d5e571d12b030aafe5d89e"}, /* libnavi_api.so */
};
static const char *const firmware_185_data[][2]={
 {"0eed5038db1c678b14cf666752c1c02db6c884ec1229329b9867f4342544714d","07f150cf640cf63a91a25994197ae26fb91f755e38b8a8313ee9c00a9280899f"}, /* apl_bdv */
};
static int in_table(const char *const t[][2],unsigned n,const char *pinned,const char *have){
 unsigned i;for(i=0;i<n;i++)if(!strcmp(pinned,t[i][0])&&!strcmp(have,t[i][1]))return 1;return 0;
}
int ub_firmware_match(const char *have,const char *pinned){
 if(!strcmp(have,pinned))return 182;
 if(in_table(firmware_185_code,sizeof firmware_185_code/sizeof *firmware_185_code,pinned,have))return 185;
 if(in_table(firmware_185_data,sizeof firmware_185_data/sizeof *firmware_185_data,pinned,have))return 185;
 return 0;
}
int ub_firmware_injectable(const char *have,const char *pinned){
 if(!strcmp(have,pinned))return 1;
 return in_table(firmware_185_code,sizeof firmware_185_code/sizeof *firmware_185_code,pinned,have);
}
const struct ub_payload_variant *ub_payload_variant(const struct ub_payload *p,const char *sha){
 unsigned i;
 if(!p||!p->variant_count)return 0;
 for(i=0;i<p->variant_count;i++)if(!strcmp(sha,p->variants[i].sha256))return &p->variants[i];
 return ub_firmware_injectable(sha,p->variants[0].sha256)?&p->variants[0]:0;
}
void ub_hash_bytes(const void *data,size_t size,char out[65]){struct ub_sha256 h;ub_sha256_init(&h);ub_sha256_feed(&h,data,size);ub_sha256_end(&h,out);}
int ub_hash_file(const char *p,char out[65]){int fd=open(p,O_RDONLY);ssize_t n;unsigned char buf[8192];struct ub_sha256 h;if(fd<0)return -1;ub_sha256_init(&h);while((n=read(fd,buf,sizeof buf))>0)ub_sha256_feed(&h,buf,(size_t)n);close(fd);if(n<0)return -1;ub_sha256_end(&h,out);return 0;}

struct elf {unsigned char *raw;size_t n;Elf32_Ehdr *eh;Elf32_Shdr *sh;};
static int range(size_t n,size_t a,size_t z){return a<=n&&z<=n-a;}
static int view(struct elf *e,const void *raw,size_t n){
 unsigned i;e->raw=(unsigned char *)raw;e->n=n;e->eh=(Elf32_Ehdr *)raw;
 /* ELF structures require word alignment. Embedded byte arrays need it too:
  * ARM LDRD faults even where ordinary unaligned word loads are tolerated. */
 if((uintptr_t)raw%_Alignof(Elf32_Ehdr))return -1;
 if(n<sizeof(Elf32_Ehdr)||memcmp(raw,ELFMAG,4)||e->eh->e_ident[EI_CLASS]!=ELFCLASS32||e->eh->e_ident[EI_DATA]!=ELFDATA2LSB||e->eh->e_machine!=EM_ARM||e->eh->e_shentsize!=sizeof(Elf32_Shdr)||!range(n,e->eh->e_shoff,(size_t)e->eh->e_shnum*sizeof(Elf32_Shdr)))return -1;
 if(e->eh->e_shoff%_Alignof(Elf32_Shdr))return -1;
 if(e->eh->e_phnum&&(e->eh->e_phentsize!=sizeof(Elf32_Phdr)||
    e->eh->e_phoff%_Alignof(Elf32_Phdr)||!range(n,e->eh->e_phoff,(size_t)e->eh->e_phnum*sizeof(Elf32_Phdr))))return -1;
 e->sh=(Elf32_Shdr *)(e->raw+e->eh->e_shoff);
 for(i=0;i<e->eh->e_shnum;i++)if(e->sh[i].sh_type!=SHT_NOBITS&&!range(n,e->sh[i].sh_offset,e->sh[i].sh_size))return -1;
 for(i=0;i<e->eh->e_shnum;i++){
  Elf32_Shdr *s=&e->sh[i];size_t entry=0;
  switch(s->sh_type){
  case SHT_SYMTAB:case SHT_DYNSYM:
   entry=sizeof(Elf32_Sym);
   if(s->sh_link>=e->eh->e_shnum||e->sh[s->sh_link].sh_type!=SHT_STRTAB)return -1;
   break;
  case SHT_REL:entry=sizeof(Elf32_Rel);break;
  case SHT_RELA:entry=sizeof(Elf32_Rela);break;
  case SHT_DYNAMIC:entry=sizeof(Elf32_Dyn);break;
  case SHT_HASH:entry=sizeof(Elf32_Word);break;
  }
  if(entry&&(s->sh_offset%4||s->sh_size%entry||s->sh_entsize!=entry))return -1;
 }
 if(e->eh->e_shstrndx!=SHN_UNDEF){
  if(e->eh->e_shstrndx>=e->eh->e_shnum)return -1;
  Elf32_Shdr *strings=&e->sh[e->eh->e_shstrndx];
  if(strings->sh_type!=SHT_STRTAB)return -1;
  for(i=0;i<e->eh->e_shnum;i++)if(e->sh[i].sh_name>=strings->sh_size||
   !memchr(e->raw+strings->sh_offset+e->sh[i].sh_name,0,strings->sh_size-e->sh[i].sh_name))return -1;
 }
 return 0;
}
/* A mapped file as a read-only private mapping (unload_file), not a copy. */
static int load_file(const char *p,struct elf *e){
 const char *name=strrchr(p,'/');
 /* Media containers are never symbol providers. Exclude them before stat or
  * open, including any capitalization, even if they appear in a process's maps. */
 name=name?name+1:p;size_t length=strlen(name);
 if(length>=4&&!strcasecmp(name+length-4,".mkv")){errno=EINVAL;return -1;}
 /* Only regular files are opened: never device nodes such as /dev/mali0 or
  * /dev/mem, whose drivers run open/release. A non-ELF file (a font,
  * apl_lang.dat) is rejected from its header, never read whole. */
 struct ub_stat st,now;Elf32_Ehdr h;int fd;void *m;
 if(ub_stat(p,&st)||!S_ISREG(st.mode)||st.size<(long long)sizeof h||st.size>64*1024*1024){errno=EINVAL;return -1;}
 fd=open(p,O_RDONLY|O_CLOEXEC|O_NONBLOCK);if(fd<0)return -1;
 if(ub_fstat(fd,&now)||!S_ISREG(now.mode)||now.dev!=st.dev||now.ino!=st.ino||now.size!=st.size||pread(fd,&h,sizeof h,0)!=(ssize_t)sizeof h||
    memcmp(h.e_ident,ELFMAG,4)||h.e_ident[EI_CLASS]!=ELFCLASS32||h.e_ident[EI_DATA]!=ELFDATA2LSB||h.e_machine!=EM_ARM){close(fd);errno=ENOEXEC;return -1;}
 m=mmap(0,(size_t)st.size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);if(m==MAP_FAILED)return -1;
 if(view(e,m,(size_t)st.size)){munmap(m,(size_t)st.size);errno=ENOEXEC;return -1;}
 return 0;
}
static void unload_file(struct elf *e){if(e->raw)munmap(e->raw,e->n);e->raw=0;}
static const char *symname(struct elf *e,Elf32_Shdr *syms,Elf32_Sym *s){Elf32_Shdr *str;if(syms->sh_link>=e->eh->e_shnum)return 0;str=&e->sh[syms->sh_link];if(s->st_name>=str->sh_size)return 0;const char *p=(char *)e->raw+str->sh_offset+s->st_name;return memchr(p,0,str->sh_size-s->st_name)?p:0;}
static Elf32_Sym *findsym(struct elf *e,const char *name){unsigned i,j;for(i=0;i<e->eh->e_shnum;i++){Elf32_Shdr *s=&e->sh[i];if(s->sh_type!=SHT_DYNSYM||s->sh_entsize!=sizeof(Elf32_Sym))continue;Elf32_Sym *a=(Elf32_Sym *)(e->raw+s->sh_offset);for(j=0;j<s->sh_size/sizeof *a;j++){const char *n=symname(e,s,a+j);if(n&&!strcmp(n,name)&&a[j].st_shndx!=SHN_UNDEF)return a+j;}}return 0;}
/* Each mapped ELF is loaded and parsed once per installation (module_list),
 * then shared by every import lookup; module_free unmaps them all. */
struct module {char path[512];uint32_t start,offset,bias;struct elf e;};
struct modules {struct module m[160];unsigned n;int pid,mem;};
static uint32_t arm_immediate(uint32_t instruction){unsigned n=((instruction>>8)&15)*2;uint32_t v=instruction&255;return n?(v>>n|v<<(32-n)):v;}
static uint32_t plt_for_got(struct elf *e,Elf32_Shdr *plt,uint32_t bias,uint32_t got){
 for(uint32_t off=0;off+12<=plt->sh_size;off+=4){uint32_t a,b,c;memcpy(&a,e->raw+plt->sh_offset+off,4);memcpy(&b,e->raw+plt->sh_offset+off+4,4);memcpy(&c,e->raw+plt->sh_offset+off+8,4);
  if((a&0xfffff000u)!=0xe28fc000u||(b&0xfffff000u)!=0xe28cc000u||(c&0xfffff000u)!=0xe5bcf000u)continue;
  uint32_t slot=bias+plt->sh_addr+off+8+arm_immediate(a)+arm_immediate(b)+(c&4095);
  if(slot==got)return bias+plt->sh_addr+off;
 }
 return 0;
}
static void module_free(struct modules *ms){for(unsigned i=0;i<ms->n;i++)unload_file(&ms->m[i].e);ms->n=0;if(ms->mem>=0)close(ms->mem);ms->mem=-1;}
static int module_list(int pid,struct modules *ms){static char text[256*1024];char p[64],*line,*save,name[600],perm[8];unsigned long start,end,off;memset(ms,0,sizeof *ms);ms->pid=pid;snprintf(p,sizeof p,"/proc/%d/mem",pid);ms->mem=open(p,O_RDONLY|O_CLOEXEC);if(ms->mem<0)return -1;snprintf(p,sizeof p,"/proc/%d/maps",pid);if(ub_read_file(p,text,sizeof text)<0){module_free(ms);return -1;}for(line=strtok_r(text,"\n",&save);line;line=strtok_r(0,"\n",&save)){name[0]=0;if(sscanf(line,"%lx-%lx %7s %lx %*s %*s %599[^\n]",&start,&end,perm,&off,name)!=5||name[0]!='/'||strstr(name,"(deleted)"))continue;unsigned i;for(i=0;i<ms->n;i++)if(!strcmp(ms->m[i].path,name))break;if(i<ms->n||ms->n==160||strlen(name)>=sizeof ms->m[0].path)continue;struct elf e;if(load_file(name,&e))continue;uint32_t bias=0;unsigned j;int matched=0;if(e.eh->e_phentsize==sizeof(Elf32_Phdr)&&range(e.n,e.eh->e_phoff,(size_t)e.eh->e_phnum*sizeof(Elf32_Phdr))){Elf32_Phdr *ph=(Elf32_Phdr *)(e.raw+e.eh->e_phoff);for(j=0;j<e.eh->e_phnum;j++)if(ph[j].p_type==PT_LOAD&&(ph[j].p_offset&~4095u)==off){bias=(uint32_t)start-(ph[j].p_vaddr&~4095u);matched=1;break;}}if(!matched){unload_file(&e);continue;}struct module *m=&ms->m[ms->n++];memcpy(m->path,name,strlen(name)+1);m->start=(uint32_t)start;m->offset=(uint32_t)off;m->bias=bias;m->e=e;}if(!ms->n){module_free(ms);return -1;}return 0;}
/* IFUNCs are resolved through an existing target PLT entry/GOT, not by calling
 * an IFUNC resolver as a normal function. Plain exported functions retain their
 * ARM/Thumb symbol bit. */
static uint32_t plt_symbol(struct modules *ms,const char *name){unsigned i,j,k;for(i=0;i<ms->n;i++){struct elf e=ms->m[i].e;Elf32_Shdr *plt=0;if(e.eh->e_shstrndx<e.eh->e_shnum){Elf32_Shdr *s=&e.sh[e.eh->e_shstrndx];for(j=0;j<e.eh->e_shnum;j++)if(e.sh[j].sh_name<s->sh_size&&!strcmp((char *)e.raw+s->sh_offset+e.sh[j].sh_name,".plt")){plt=&e.sh[j];break;}}for(j=0;j<e.eh->e_shnum&&plt;j++){Elf32_Shdr *r=&e.sh[j];if(r->sh_type!=SHT_REL||r->sh_link>=e.eh->e_shnum||r->sh_entsize!=sizeof(Elf32_Rel))continue;Elf32_Shdr *s=&e.sh[r->sh_link];if(s->sh_type!=SHT_DYNSYM)continue;Elf32_Rel *a=(Elf32_Rel *)(e.raw+r->sh_offset);Elf32_Sym *sy=(Elf32_Sym *)(e.raw+s->sh_offset);for(k=0;k<r->sh_size/sizeof *a;k++){if(ELF32_R_TYPE(a[k].r_info)!=R_ARM_JUMP_SLOT)continue;unsigned ix=ELF32_R_SYM(a[k].r_info);if(ix>=s->sh_size/sizeof *sy)continue;const char *n=symname(&e,s,sy+ix);if(n&&!strcmp(n,name)){uint32_t result=0;if(pread(ms->mem,&result,4,(off_t)(ms->m[i].bias+a[k].r_offset))!=4)result=0;uint32_t first=ms->m[i].bias+plt->sh_addr,last=first+plt->sh_size;if(result&&!(result>=first&&result<last))return result;result=plt_for_got(&e,plt,ms->m[i].bias,ms->m[i].bias+a[k].r_offset);if(result)return result;}}}}return 0;}
static int dynamic_linker(const char *path){const char *b=strrchr(path,'/');b=b?b+1:path;return !strncmp(b,"ld-",3)&&strstr(b,".so")!=0;}
static uint32_t resolve(struct modules *ms,const char *name){
 /* /proc/maps is address order, not the dynamic linker's global scope order.
  * glibc2.20 also exports pthread mutex stubs. Bind pthread APIs to the real
  * loaded libpthread before considering libc's compatibility definitions.
  * The dynamic linker (ld-2.20, mapped below libc) is consulted last, as in
  * its own global scope after libc: its minimal malloc/free must never stand
  * in for libc's (that free only rewinds its own last block). */
 unsigned i,pass;
 for(pass=0;pass<3;pass++){
  if(!pass&&strncmp(name,"pthread_",8))continue;
  for(i=0;i<ms->n;i++){
   int ld=dynamic_linker(ms->m[i].path);
   if(pass==0?!strstr(ms->m[i].path,"libpthread"):pass==1?ld:!ld)continue;
   Elf32_Sym *s=findsym(&ms->m[i].e,name);uint32_t v=s?ms->m[i].bias+s->st_value:0;
   unsigned type=s?ELF32_ST_TYPE(s->st_info):0;
   if(v){if(type==STT_GNU_IFUNC)return plt_symbol(ms,name);return v;}
  }
 }
 return 0;
}

struct regs {uint32_t r[18];};
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif
#ifndef UB_PTRACE_PAUSE_MS
#define UB_PTRACE_PAUSE_MS 500
#endif
struct stopped {
 int tids[256],signal[256];unsigned char waiting[256],group_stop[256],delivery[256];
 unsigned n;int pid,mem,quarantine;unsigned long long until;uint32_t stub_pc;
};
/* A task in uninterruptible I/O need not acknowledge INTERRUPT by our deadline.
 * No memory has been touched in that case. Keep ownership and detach as soon
 * as its stop arrives; never pretend a running tracee was successfully detached.
 * One batch at a time bounds storage and blocks further injections meanwhile. */
static struct stopped deferred;
static void observe_interrupted_call(int tid);
static int trace_index(struct stopped *s,int tid){for(unsigned i=0;i<s->n;i++)if(s->tids[i]==tid)return (int)i;return -1;}
/* delivery[i]: the thread is in the signal-delivery-stop that dequeued
 * signal[i]. Only that stop lets PTRACE_DETACH deliver it with its original
 * siginfo; Linux ignores a detach signal in an event-stop. */
static void record_stop(struct stopped *s,unsigned i,int status){
 s->waiting[i]=0;s->delivery[i]=(unsigned)status>>16==0;
 if((unsigned)status>>16==PTRACE_EVENT_STOP){
  if(WSTOPSIG(status)!=SIGTRAP)s->group_stop[i]=1;
  else observe_interrupted_call(s->tids[i]);
 }else {
  int sig=WSTOPSIG(status);
  if(s->signal[i]&&s->signal[i]!=sig)syscall(SYS_tgkill,s->pid,s->tids[i],s->signal[i]);
  s->signal[i]=sig;
 }
}
/* 0 stopped, 1 exited, -1 deadline/error. Does not resume or suppress signals. */
static int wait_stop(struct stopped *s,unsigned i,unsigned long long until,int *observed){
 for(;;){
  int status;pid_t r=waitpid(s->tids[i],&status,__WALL|WNOHANG);
  if(r>0){if(WIFSTOPPED(status)){if(observed){*observed=status;s->waiting[i]=0;}else record_stop(s,i,status);return 0;}s->tids[i]=0;return 1;}
  if(r<0&&errno!=EINTR){if(errno==ECHILD||errno==ESRCH){s->tids[i]=0;return 1;}return -1;}
  if(ub_now_ms()>=until){errno=ETIMEDOUT;return -1;}
  ub_sleep_ms(1);
 }
}
static void detach_all(struct stopped *s){
 struct stopped remaining;memset(&remaining,0,sizeof remaining);remaining.mem=-1;remaining.pid=s->pid;remaining.quarantine=s->quarantine;
 if(s->quarantine&&s->pid>0){
  /* Do not signal a stale process number after a tracee exit. INTERRUPT
   * verifies tracer ownership; tgkill additionally verifies the TID/TGID pair. */
  for(unsigned i=0;i<s->n;i++)if(s->tids[i]&&!ptrace(PTRACE_INTERRUPT,s->tids[i],0,0)){
   if(!syscall(SYS_tgkill,s->pid,s->tids[i],SIGSTOP))break;
  }
 }
 for(unsigned i=0;i<s->n;i++){
  int tid=s->tids[i];if(!tid)continue;
  if(s->waiting[i]){
   int status;pid_t r=waitpid(tid,&status,__WALL|WNOHANG);
   if(r>0){if(!WIFSTOPPED(status))continue;record_stop(s,i,status);}
   else if(r<0&&(errno==ECHILD||errno==ESRCH))continue;
  }
  if(!s->waiting[i]){
   int sig=s->quarantine||s->group_stop[i]?SIGSTOP:s->signal[i];
   /* A genuine signal already removed from the pending queue must survive
    * quarantine/group-stop too; detach can carry only one signal, and only
    * out of its own delivery-stop. Otherwise it is queued again. */
   if(s->signal[i]&&(sig!=s->signal[i]||!s->delivery[i])){
    syscall(SYS_tgkill,s->pid,tid,s->signal[i]);
    if(sig==s->signal[i])sig=0;
   }
   if(!ptrace(PTRACE_DETACH,tid,0,(void *)(uintptr_t)sig)||errno==ESRCH)continue;
   ub_log("injector",UB_ALWAYS,"detach %d deferred: %s\n",tid,strerror(errno));
  }else ptrace(PTRACE_INTERRUPT,tid,0,0);
  unsigned j=remaining.n++;remaining.tids[j]=tid;remaining.signal[j]=s->signal[i];remaining.waiting[j]=s->waiting[i];remaining.group_stop[j]=s->group_stop[i];remaining.delivery[j]=s->delivery[i];
 }
 s->n=0;deferred=remaining;
}
void ub_injector_poll(void){if(deferred.n){struct stopped pending=deferred;deferred.n=0;detach_all(&pending);}}
static int repeat_semop(int tid,int mem);
/* SEIZE never manufactures SIGSTOP. INTERRUPT gives an identifiable event-stop,
 * while a real signal arriving first is retained and reinjected on detach. */
static int stop_threads(int pid,struct stopped *s){
 char comm_path[64],comm[64],exe[600];const char *name;ssize_t exe_length;
 memset(s,0,sizeof *s);s->pid=pid;s->mem=-1;ub_injector_poll();
 snprintf(comm_path,sizeof comm_path,"/proc/%d/comm",pid);
 if(ub_native_wait_read(comm_path,comm,sizeof comm)<0)return -1;
 comm[strcspn(comm,"\r\n")]=0;
 snprintf(comm_path,sizeof comm_path,"/proc/%d/exe",pid);exe_length=readlink(comm_path,exe,sizeof exe-1);
 if(exe_length<0)return -1;
 if(exe_length==(ssize_t)sizeof exe-1){errno=ENAMETOOLONG;return -1;}
 exe[exe_length]=0;name=strrchr(exe,'/');name=name?name+1:exe;
 /* Every protected child stop path, including code/region transactions and probes, must
  * have the public-API handler already resident. Never attempt a first stop
  * to install the very handler needed to survive that stop. */
 if((ub_native_wait_process(comm)||ub_native_wait_process(name))&&!ub_native_wait_ready(pid)){
  /* Callers retry (1 s loops, the 20 ms DVD guard): report each process once. */
  static int refused_pid;
  ub_log("injector",refused_pid==pid?UB_INFO:UB_ALWAYS,"%s PID%d has no current pre-main EV wait handler; no threads stopped (a protected native launch is required)\n",name,pid);
  refused_pid=pid;
  errno=EAGAIN;return -1;
 }
 unsigned long long until=ub_now_ms()+UB_PTRACE_PAUSE_MS;
 s->until=until+1500;
 if(deferred.n){errno=EBUSY;return -1;}
 for(unsigned pass=0;pass<64;pass++){
  char path[64];struct ub_dir dir;const char *de;unsigned added=0;int more;
  snprintf(path,sizeof path,"/proc/%d/task",pid);if(ub_dir_open(&dir,path))goto fail;
  while((more=ub_dir_read(&dir,&de))>0){
   int tid=atoi(de);if(tid<=0||trace_index(s,tid)>=0)continue;
   if(s->n==256||ub_now_ms()>=until){ub_dir_close(&dir);errno=ETIMEDOUT;goto fail;}
   if(ptrace(PTRACE_SEIZE,tid,0,0)){if(errno==ESRCH)continue;ub_dir_close(&dir);goto fail;}
   unsigned i=s->n++;s->tids[i]=tid;s->waiting[i]=1;
   if(ptrace(PTRACE_INTERRUPT,tid,0,0)){ub_dir_close(&dir);goto fail;}
   int rc=wait_stop(s,i,until,0);if(rc<0){ub_dir_close(&dir);goto fail;}if(!rc)added++;
  }
  ub_dir_close(&dir);
  /* A listing cut short by a read error is not a complete thread set, even if
   * it added nothing new: defer the whole transaction (caller retries later). */
  if(more<0){errno=dir.err;goto fail;}
  if(!added&&s->n){
   unsigned repeated=0;
   snprintf(path,sizeof path,"/proc/%d/mem",pid);s->mem=open(path,O_RDWR);if(s->mem<0)goto fail;
   /* Only event-stops of our own INTERRUPT: a group-stop or a foreign signal
    * keeps its EINTR semantics. */
   for(unsigned i=0;i<s->n;i++)if(s->tids[i]&&!s->waiting[i]&&!s->signal[i]&&!s->group_stop[i])repeated+=(unsigned)repeat_semop(s->tids[i],s->mem);
   if(repeated)ub_log("injector",UB_ALWAYS,"PID%d: our stop interrupted semop in %u thread(s); they repeat the call when resumed\n",pid,repeated);
   return 0;
  }
 }
 errno=ETIMEDOUT;
 fail:{int saved=errno;detach_all(s);errno=saved;}return -1;
}
/* Target addresses reach 0xffffffff: a 32-bit off_t would turn them negative. */
_Static_assert(sizeof(off_t)>=8,"build with _FILE_OFFSET_BITS=64");
static int memio(int fd,uint32_t a,void *buf,size_t n,int writing){
 size_t done=0;
 while(done<n){
  ssize_t k=writing?pwrite(fd,(char *)buf+done,n-done,(off_t)(a+done)):pread(fd,(char *)buf+done,n-done,(off_t)(a+done));
  if(k<0&&errno==EINTR)continue;
  if(k<=0){if(!k)errno=EIO;return -1;}done+=(size_t)k;
 }
 return 0;
}
static int restore_memory(struct stopped *s,int tid,uint32_t a,const void *bytes,size_t n){
 unsigned char verify[32];if(!n||n>sizeof verify)return -1;
 for(unsigned i=0;i<s->n;i++)if(s->tids[i]&&s->waiting[i])return -1;
 /* Original bytes already satisfy rollback even when writes are refused. */
 if(!memio(s->mem,a,verify,n,0)&&!memcmp(bytes,verify,n))return 0;
 if(memio(s->mem,a,(void *)bytes,n,1)){
  /* POKETEXT fallback also supports byte/string patch sites. Preserve the
   * untouched bytes in the native ptrace word on both ARM and host tests. */
  size_t wordsize=sizeof(unsigned long);uint32_t first=a&~(uint32_t)(wordsize-1);
  for(uint32_t at=first;(uint64_t)at<(uint64_t)a+n;at+=(uint32_t)wordsize){
   unsigned long word;unsigned char *b=(unsigned char *)&word;
   if(memio(s->mem,at,&word,wordsize,0))return -1;
   for(unsigned j=0;j<wordsize;j++)if((uint64_t)at+j>=a&&(uint64_t)at+j<(uint64_t)a+n)b[j]=((const unsigned char *)bytes)[at+j-a];
   if(ptrace(PTRACE_POKETEXT,tid,(void *)(uintptr_t)at,(void *)(uintptr_t)word))return -1;
  }
 }
 return memio(s->mem,a,verify,n,0)||memcmp(bytes,verify,n)?-1:0;
}
static int getregs(int tid,struct regs *r){return ptrace(PTRACE_GETREGS,tid,0,r);}
static int setregs(int tid,struct regs *r){return ptrace(PTRACE_SETREGS,tid,0,r);}
/* Audit every shared stop path, including partial/deferred detach. Observation
 * only: no syscall, register or user packet is replayed by this diagnostic. */
static void observe_interrupted_call(int tid){
 int saved_errno=errno;
 do {
#if defined(__arm__)
 struct regs r;unsigned thumb,offset;uint32_t at;long word;unsigned char bytes[sizeof word];
 if(getregs(tid,&r)||(int32_t)r.r[0]!=-EINTR)break;
 thumb=(r.r[16]&0x20u)!=0;at=r.r[15]-(thumb?2u:4u);offset=at&(sizeof word-1);
 errno=0;word=ptrace(PTRACE_PEEKTEXT,tid,(void *)(uintptr_t)(at&~(uint32_t)(sizeof word-1)),0);
 if(word==-1&&errno)break;
 memcpy(bytes,&word,sizeof word);
 if(thumb?memcmp(bytes+offset,"\x00\xdf",2):memcmp(bytes,"\x00\x00\x00\xef",4))break;
 ub_log("injector",UB_ALWAYS,"TID%d at our event-stop: post-svc EINTR, syscall=%u (trigger attribution requires native evidence)\n",tid,r.r[7]);
#elif defined(__x86_64__)
 struct user_regs_struct r;long word;unsigned char bytes[sizeof word];uintptr_t at;
 if(ptrace(PTRACE_GETREGS,tid,0,&r)||(long)r.rax!=-EINTR||(long)r.orig_rax<0)break;
 at=r.rip-2;errno=0;
 word=ptrace(PTRACE_PEEKTEXT,tid,(void *)at,0);
 if(word==-1&&errno)break;
 memcpy(bytes,&word,sizeof word);if(memcmp(bytes,"\x0f\x05",2))break;
 ub_log("injector",UB_ALWAYS,"TID%d at our event-stop: post-syscall EINTR, syscall=%lld (trigger attribution requires native evidence)\n",tid,(long long)r.orig_rax);
#else
 (void)tid;
#endif
 }while(0);
 errno=saved_errno;
}
/* PTRACE_INTERRUPT wakes a thread sleeping in SysV semop/semtimedop, and Linux
 * returns -EINTR from it, never restarted (ipc/sem.c; the -ERESTART* waits,
 * e.g. select and poll, the kernel rewinds itself before our stop). libufs
 * reports that as an I/O error, libavd enters its section without the lock
 * and later raises a system-wide count. A thread stopped by our own INTERRUPT
 * right after such a return is rewound onto its svc with its first argument
 * back, as the kernel does for restartable calls, so it repeats the call on
 * resume. Restrict this historical workaround to indefinite semop (298).
 * semtimedop (312) has a relative timeout that the kernel does not update:
 * repeating it would extend the caller's deadline. Leave its EINTR intact. */
#if defined(__x86_64__)
#define NR_SEMOP SYS_semop
#define NR_SEMTIMEDOP SYS_semtimedop
#else
#define NR_SEMOP 298
#define NR_SEMTIMEDOP 312
#endif
static int repeat_call(long number,long result,int after_svc){return after_svc&&result==-EINTR&&number==NR_SEMOP;}
#ifndef PTRACE_PEEKSIGINFO
#define PTRACE_PEEKSIGINFO 0x4209
#endif
/* A signal queued for the thread or its process may be what interrupted the
 * call; then it keeps its EINTR. Unknown counts as queued. */
static int signal_queued(int tid){
 struct {uint64_t off;uint32_t flags;int32_t nr;} a={0,0,1};unsigned char info[128];
 if(ptrace(PTRACE_PEEKSIGINFO,tid,&a,info))return 1;
 a.flags=1;return ptrace(PTRACE_PEEKSIGINFO,tid,&a,info)!=0; /* PTRACE_PEEKSIGINFO_SHARED */
}
/* 1 when the stopped thread was rewound. Registers change once, before any
 * remote_syscall saves them, so its restore and the detach keep the rewind. */
static int repeat_semop(int tid,int mem){
#if defined(__arm__)
 struct regs r;unsigned char b[4];int thumb;uint32_t at;
 if(getregs(tid,&r))return 0;
 thumb=(r.r[16]&0x20u)!=0;at=r.r[15]-(thumb?2u:4u);
 if(memio(mem,at,b,thumb?2:4,0)||!repeat_call((int32_t)r.r[7],(int32_t)r.r[0],thumb?!memcmp(b,"\x00\xdf",2):!memcmp(b,"\x00\x00\x00\xef",4))||signal_queued(tid))return 0;
 r.r[0]=r.r[17];r.r[15]=at;return !setregs(tid,&r);
#elif defined(__x86_64__)
 /* Host tests: the same decision on x86-64 (syscall 0f 05, orig_rax). */
 struct user_regs_struct r;unsigned char b[2];
 if(ptrace(PTRACE_GETREGS,tid,0,&r)||pread(mem,b,2,(off_t)(r.rip-2))!=2)return 0;
 if(!repeat_call((long)r.orig_rax,(long)r.rax,!memcmp(b,"\x0f\x05",2))||signal_queued(tid))return 0;
 r.rax=r.orig_rax;r.rip-=2;return !ptrace(PTRACE_SETREGS,tid,0,&r);
#else
 (void)tid;(void)mem;return 0;
#endif
}
/* Diagnostics for ub_probe: the injected thread's state and the trap outcome of
 * the most recent remote_syscall. Not used by normal operation. */
static struct { uint32_t pc,trap_pc,old_pc,old_r7,old_orig,old_cpsr; unsigned stops; int lastsig; } rsdbg;
#ifdef UB_HOST_TEST
/* Tests model failing ARM VM syscalls without executing Thumb on an x86 child. */
static int (*remote_syscall_test)(struct stopped *,int,uint32_t,const uint32_t [6],uint32_t *);
#endif
/* The 8-byte stub replaces code at the stopped thread's own PC. It must stay
 * inside that PC's page: the next page can be data (apl_bdv's RX segment ends
 * where RW begins) and the stub's final breakpoint would fault there. */
static uint32_t stub_address(uint32_t pc){pc&=~3u;if((pc&4095u)>4088u)pc-=4u;return pc;}
static int executable_range(int pid,uint32_t a,uint32_t n);
static int remote_syscall(struct stopped *s,int tid,uint32_t number,const uint32_t args[6],uint32_t *result){
#ifdef UB_HOST_TEST
 if(remote_syscall_test)return remote_syscall_test(s,tid,number,args,result);
#endif
 struct regs old,r;int index=trace_index(s,tid),status,changed=0,rc=-1,maskok=0;
 unsigned char saved[8],stub[8]={0xab,0xbe,0x00,0xdf,0xac,0xbe,0x00,0xbf};
 uint32_t pc;uint64_t savemask=0,allmask=~0ull;
 unsigned long long until=ub_now_ms()+UB_PTRACE_PAUSE_MS;
 if(s->until&&until>s->until)until=s->until;
 if(ub_now_ms()>=until)return -1;
 /* A thread holding a dequeued signal is never resumed: an INTERRUPT that
  * arrived during its delivery-stop would turn that stop into an event-stop
  * and the signal could only be re-queued with a forged siginfo. Detach
  * delivers it intact; the caller retries the transaction. */
 if(index<0||s->waiting[index]||s->group_stop[index]||s->signal[index]||s->quarantine)return -1;
 if(getregs(tid,&old))return -1;
 r=old;pc=stub_address(old.r[15]);
 /* Registers are restored after every call, so one maps check per stop. */
 if(s->stub_pc!=(pc|1u)){if(pc>=0xffff0000u||!executable_range(s->pid,pc,8))return -1;s->stub_pc=pc|1u;}
 rsdbg.old_pc=old.r[15];rsdbg.old_r7=old.r[7];rsdbg.old_orig=old.r[17];rsdbg.old_cpsr=old.r[16];rsdbg.pc=pc;rsdbg.trap_pc=0;rsdbg.stops=0;rsdbg.lastsig=0;
 if(memio(s->mem,pc,saved,8,0))return -1;
 /* Mask changes are mandatory: never run a library signal handler while its
  * peers are stopped. A delivery-stop saved by SEIZE is reinjected on detach. */
 if(ptrace(PTRACE_GETSIGMASK,tid,(void *)(uintptr_t)sizeof savemask,&savemask))return -1;
 if(ptrace(PTRACE_SETSIGMASK,tid,(void *)(uintptr_t)sizeof allmask,&allmask))return -1;
 maskok=1;changed=1;
 if(memio(s->mem,pc,stub,8,1))goto finish;
 for(unsigned i=0;i<6;i++)r.r[i]=args[i];
 r.r[7]=number;r.r[15]=pc;r.r[16]=(r.r[16]&~0x0600fc00u)|0x20u;r.r[17]=0xffffffffu;
 if(setregs(tid,&r)||ptrace(PTRACE_CONT,tid,0,0))goto finish;
 s->waiting[index]=1;
 for(;;){
  pid_t waited=waitpid(tid,&status,__WALL|WNOHANG);
  if(waited<0&&errno!=EINTR)goto recover_stop;
  if(waited==0||waited<0){if(ub_now_ms()>=until)goto recover_stop;ub_sleep_ms(1);continue;}
  if(!WIFSTOPPED(status)){s->tids[index]=0;return -1;}
  s->waiting[index]=0;rsdbg.stops++;rsdbg.lastsig=WSTOPSIG(status);
  if(WSTOPSIG(status)==SIGTRAP&&!(status>>16)){
   if(getregs(tid,&r))goto finish;
   rsdbg.trap_pc=r.r[15];
   /* Keep the sync breakpoint for both clean event-stop resumes and the ARM
    * syscall-exit path which can skip the first Thumb instruction. */
   if(r.r[15]==pc){
    r.r[15]=pc+2;for(unsigned i=0;i<6;i++)r.r[i]=args[i];r.r[7]=number;r.r[17]=0xffffffffu;
    if(setregs(tid,&r)||ptrace(PTRACE_CONT,tid,0,0))goto finish;
    s->waiting[index]=1;continue;
   }
   if(r.r[15]==pc+4){*result=r.r[0];rc=0;goto finish;}
  }
  /* Never swallow a foreign stop and run on through it. SIGSTOP cannot be
   * masked; preserve a concurrent group stop. Unexpected synchronous faults
   * during the bootstrap quarantine instead of exposing damaged execution. */
  if(WSTOPSIG(status)==SIGSTOP)s->group_stop[index]=1;
  else if((unsigned)status>>16!=PTRACE_EVENT_STOP)s->quarantine=1;
  goto finish;
 }
 recover_stop:
 /* A deadline does not authorize restoring bytes beneath a running PC. First
  * interrupt and acknowledge the stop. If I/O prevents that, keep the whole
  * process stopped/quarantined, and let poll finish the outstanding detach. */
 ptrace(PTRACE_INTERRUPT,tid,0,0);
 if(wait_stop(s,(unsigned)index,ub_now_ms()+UB_PTRACE_PAUSE_MS,&status)!=0){s->quarantine=1;return -1;}
 /* The interrupt races our own final breakpoint. Never reinject that private
  * SIGTRAP into the restored application as though it were a foreign signal. */
 if(WSTOPSIG(status)==SIGSTOP)s->group_stop[index]=1;
 else if((unsigned)status>>16!=PTRACE_EVENT_STOP){
  if(WSTOPSIG(status)!=SIGTRAP||getregs(tid,&r)||(r.r[15]!=pc&&r.r[15]!=pc+4))s->quarantine=1;
 }
 finish:
 if(s->waiting[index]){s->quarantine=1;return -1;}
 if(changed&&restore_memory(s,tid,pc,saved,8)){s->quarantine=1;rc=-1;}
 if(setregs(tid,&old)){s->quarantine=1;rc=-1;}
 if(maskok&&ptrace(PTRACE_SETSIGMASK,tid,(void *)(uintptr_t)sizeof savemask,&savemask)){s->quarantine=1;rc=-1;}
 return rc;
}
static int cacheflush(struct stopped *s,int tid,uint32_t a,uint32_t n){uint32_t r,args[6]={a,a+n,0,0,0,0};return remote_syscall(s,tid,0x0f0002,args,&r)||((int32_t)r<0)?-1:0;}

/* Relocations never invoke code and reject unsupported kinds/undefined imports. */
static int relocate(struct elf *e,unsigned char *image,uint32_t span,uint32_t base,struct modules *mods){
 unsigned i,j;for(i=0;i<e->eh->e_shnum;i++){Elf32_Shdr *rs=&e->sh[i];if(rs->sh_type==SHT_RELA)return -1;if(rs->sh_type!=SHT_REL||!(rs->sh_flags&SHF_ALLOC))continue;if(rs->sh_entsize!=sizeof(Elf32_Rel)||rs->sh_link>=e->eh->e_shnum)return -1;Elf32_Shdr *sy=&e->sh[rs->sh_link];if(sy->sh_type!=SHT_DYNSYM||sy->sh_entsize!=sizeof(Elf32_Sym))return -1;Elf32_Rel *r=(Elf32_Rel *)(e->raw+rs->sh_offset);Elf32_Sym *symbols=(Elf32_Sym *)(e->raw+sy->sh_offset);for(j=0;j<rs->sh_size/sizeof *r;j++){unsigned type=ELF32_R_TYPE(r[j].r_info),ix=ELF32_R_SYM(r[j].r_info);uint32_t value=0,*where;if(!range(span,r[j].r_offset,4)||r[j].r_offset%4||ix>=sy->sh_size/sizeof *symbols)return -1;where=(uint32_t *)(image+r[j].r_offset);if(type==R_ARM_NONE)continue;if(type==R_ARM_RELATIVE){*where+=base;continue;}if(symbols[ix].st_shndx==SHN_UNDEF){const char *name=symname(e,sy,symbols+ix);if(!name)return -1;value=resolve(mods,name);if(!value&&ELF32_ST_BIND(symbols[ix].st_info)!=STB_WEAK){ub_log("injector",UB_ALWAYS,"Unresolved payload import %s\n",name);return -1;}}else value=base+symbols[ix].st_value;switch(type){case R_ARM_ABS32:*where+=value;break;case R_ARM_GLOB_DAT:case R_ARM_JUMP_SLOT:*where=value;break;default:ub_log("injector",UB_ALWAYS,"Unsupported ARM relocation %u\n",type);return -1;}}}return 0;
}
static uint32_t symbol_address(struct elf *e,uint32_t base,const char *name){Elf32_Sym *s=findsym(e,name);return s?base+s->st_value:0;}
static int branch_thumb(uint32_t from,uint32_t to,unsigned char out[4]){
 int64_t distance=(int64_t)(to&~1u)-((int64_t)from+4);int32_t d;uint32_t s,i1,i2;uint16_t h1,h2;
 if(!(to&1u)||(distance&1)||distance< -16777216||distance>16777214)return -1;
 d=(int32_t)distance;
 s=((uint32_t)d>>24)&1;i1=((uint32_t)d>>23)&1;i2=((uint32_t)d>>22)&1;
 h1=(uint16_t)(0xf000u|(s<<10)|(((uint32_t)d>>12)&1023));
 h2=(uint16_t)(0x9000u|((~(i1^s)&1)<<13)|((~(i2^s)&1)<<11)|(((uint32_t)d>>1)&2047));
 memcpy(out,&h1,2);memcpy(out+2,&h2,2);return 0;
}
/* A distant replacement needs a complete, predecoded eight-byte prefix.
 * Installation still pauses all threads, rejects PCs inside the full prefix,
 * verifies original bytes and uses the same cache flush/rollback transaction. */
static int hook_transfer(uint32_t from,uint32_t gateway,uint32_t replacement,uint32_t length,unsigned char out[32]){
 if((from&3u)||length<4||length>32||(length&1u)||!replacement)return -1;
 for(unsigned i=0;i<length;i+=2){out[i]=0;out[i+1]=0xbf;}
 if(!branch_thumb(from,gateway,out))return 0;
 if(length<8)return -1;
 {static const unsigned char load_pc[4]={0xdf,0xf8,0,0xf0};memcpy(out,load_pc,4);}
 memcpy(out+4,&replacement,4);return 0;
}
/* Mapping metadata must be captured while stopped. A data overlay accepts one
 * readable, non-executable VMA; a prepared anonymous backup restores its bytes
 * and protection if MREMAP_FIXED has already unmapped the destination on error. */
static int mapping_protection(int pid,uint32_t a,uint32_t n){
 static char maps[256*1024];char path[64],*line,*save,perm[8];unsigned long lo,hi;
 snprintf(path,sizeof path,"/proc/%d/maps",pid);if(ub_read_file(path,maps,sizeof maps)<0)return -1;
 for(line=strtok_r(maps,"\n",&save);line;line=strtok_r(0,"\n",&save))
  if(sscanf(line,"%lx-%lx %7s",&lo,&hi,perm)==3&&(uint64_t)a>=lo&&(uint64_t)a+n<=hi)return (perm[0]=='r'?1:0)|(perm[1]=='w'?2:0)|(perm[2]=='x'?4:0);
 return -1;
}
static int remote_map(struct stopped *s,int pid,uint32_t n,const unsigned char *bytes,int prot,uint32_t *out){
 uint32_t args[6]={0,n,3,0x4022,0xffffffffu,0},result=0;*out=0;
 if(remote_syscall(s,pid,192,args,&result)||result>=0xfffff001u||result<65536u)return UB_OVERLAY_MAP;
 *out=result;if(memio(s->mem,result,(void *)bytes,n,1))return UB_OVERLAY_FILL;
 memset(args,0,sizeof args);args[0]=result;args[1]=n;args[2]=(uint32_t)prot;
 if(remote_syscall(s,pid,125,args,&result)||result)return UB_OVERLAY_SEAL;
 return 0;
}
static int remote_move(struct stopped *s,int pid,uint32_t from,uint32_t to,uint32_t n){
 uint32_t args[6]={from,n,n,3,to,0},result=0;
 return remote_syscall(s,pid,163,args,&result)||result!=to?-1:0;
}
static void remote_unmap(struct stopped *s,int pid,uint32_t a,uint32_t n){uint32_t args[6]={a,n,0,0,0,0},result;if(a&&!s->quarantine)remote_syscall(s,pid,91,args,&result);}
int ub_overlay_pages(int pid,uint32_t a,const unsigned char *expected,const unsigned char *replacement,uint32_t n){
 struct stopped stop;unsigned char *now,*check;uint32_t tmp=0,backup=0;int rc=UB_OVERLAY_ARGS,prot;
 if(!expected||!replacement||!n||(a&4095u)||(n&4095u)||n>65536||(uint64_t)a+n>UINT32_MAX)return UB_OVERLAY_ARGS;
 now=malloc(n);check=malloc(n);if(!now||!check){free(now);free(check);return UB_OVERLAY_ARGS;}
 if(stop_threads(pid,&stop)){free(now);free(check);return UB_OVERLAY_STOP;}
 rc=UB_OVERLAY_READ;prot=mapping_protection(pid,a,n);
 if(prot<0||!(prot&1)||(prot&4)||memio(stop.mem,a,now,n,0))goto out;
 if(!memcmp(now,replacement,n)){rc=0;goto out;}if(memcmp(now,expected,n)){rc=1;goto out;}
 /* Prepare both mappings completely before risking the original destination.
  * mmap/mprotect failures here leave the destination mapping untouched. */
 rc=remote_map(&stop,pid,n,now,prot,&backup);if(rc)goto out;
 rc=remote_map(&stop,pid,n,replacement,prot,&tmp);if(rc)goto out;
 rc=UB_OVERLAY_SWAP;
 if(remote_move(&stop,pid,tmp,a,n))goto recover;
 tmp=0;rc=UB_OVERLAY_VERIFY;
 if(memio(stop.mem,a,check,n,0)||memcmp(check,replacement,n))goto recover;
 rc=0;goto out;
 recover:
 stop.until=ub_now_ms()+UB_PTRACE_PAUSE_MS;
 /* A failed MREMAP_FIXED may have destroyed the destination. Even an uncertain
  * remote-syscall outcome must be recovered or quarantined, never resumed. */
 if(stop.quarantine||remote_move(&stop,pid,backup,a,n))goto quarantine;
 backup=0;
 if(memio(stop.mem,a,check,n,0)||memcmp(check,now,n)||mapping_protection(pid,a,n)!=prot)goto quarantine;
 goto out;
 quarantine:
 stop.quarantine=1;rc=UB_OVERLAY_RECOVER;
 ub_log("injector",UB_ALWAYS,"FATAL: language mapping recovery unverified for PID%d; process quarantined. Power cycle required.\n",pid);
 out:
 if(stop.quarantine){rc=UB_OVERLAY_RECOVER;}else{remote_unmap(&stop,pid,tmp,n);remote_unmap(&stop,pid,backup,n);if(stop.quarantine)rc=UB_OVERLAY_RECOVER;}
 if(stop.mem>=0)close(stop.mem);
 detach_all(&stop);free(now);free(check);return rc;
}
/* Executable map membership is checked after every thread has stopped. */
static int executable_range(int pid,uint32_t a,uint32_t n){
 static char maps[256*1024];char path[64],*line,*save,perm[8];unsigned long lo,hi;
 snprintf(path,sizeof path,"/proc/%d/maps",pid);if(ub_read_file(path,maps,sizeof maps)<0)return 0;
 for(line=strtok_r(maps,"\n",&save);line;line=strtok_r(0,"\n",&save))
  if(sscanf(line,"%lx-%lx %7s",&lo,&hi,perm)==3&&strchr(perm,'x')&&(uint64_t)a>=lo&&(uint64_t)a+n<=hi)return 1;
 return 0;
}
static int thread_pc(int tid,uint64_t *pc){
#if defined(__arm__)
 struct regs r;if(getregs(tid,&r))return -1;*pc=r.r[15]&~1u;return 0;
#elif defined(__x86_64__)
 struct user_regs_struct r;if(ptrace(PTRACE_GETREGS,tid,0,&r))return -1;*pc=r.rip;return 0;
#else
 (void)tid;(void)pc;return -1;
#endif
}
/* /proc/pid/mem and POKETEXT both use copy_to_user_page. The vendor ARM64
 * 4.4.8 implementation flushes VM_EXEC instruction caches before returning,
 * as does reference ARM Linux 4.4. No target thread needs to run just to flush
 * these writes; all PCs stay frozen throughout the transaction and rollback. */
static char patch_stage[192]="not attempted";
const char *ub_patch_stage(void){return patch_stage;}
int ub_patch_code(int pid,const struct ub_code_patch *patches,unsigned count){
 struct stopped stop;unsigned char saved[64][32],now[32],different[64];unsigned written=0;int rc=-1;
 snprintf(patch_stage,sizeof patch_stage,"arguments");
 if(!patches||!count||count>64)return -1;
 for(unsigned i=0;i<count;i++){
  const struct ub_code_patch *p=patches+i;
  if(!p->expected||!p->replacement||!p->length||p->length>32||(uint64_t)p->address+p->length>UINT32_MAX){snprintf(patch_stage,sizeof patch_stage,"invalid patch %u at %08x length %u",i,p->address,p->length);return -1;}
  for(unsigned j=0;j<i;j++)if((uint64_t)p->address<(uint64_t)patches[j].address+patches[j].length&&(uint64_t)patches[j].address<(uint64_t)p->address+p->length){snprintf(patch_stage,sizeof patch_stage,"overlapping patches %u/%u",j,i);return -1;}
 }
 if(stop_threads(pid,&stop)){snprintf(patch_stage,sizeof patch_stage,"stop: %s",strerror(errno));return -1;}
 /* Unknown bytes at any site abort before the first write, even when some
  * earlier sites already happen to match their requested replacement. */
 for(unsigned i=0;i<count;i++){
  const struct ub_code_patch *p=patches+i;
  if(!executable_range(pid,p->address,p->length)){snprintf(patch_stage,sizeof patch_stage,"executable mapping missing at %08x length %u",p->address,p->length);goto out;}
  if(memio(stop.mem,p->address,saved[i],p->length,0)){snprintf(patch_stage,sizeof patch_stage,"read %08x: %s",p->address,strerror(errno));goto out;}
  different[i]=memcmp(saved[i],p->replacement,p->length)!=0;
  if(different[i]&&memcmp(saved[i],p->expected,p->length)&&(!p->alternate||memcmp(saved[i],p->alternate,p->length))){snprintf(patch_stage,sizeof patch_stage,"unknown bytes at %08x length %u",p->address,p->length);rc=1;goto out;}
 }
 for(unsigned i=0;i<stop.n;i++){
  uint64_t pc;if(!stop.tids[i])continue;
  if(thread_pc(stop.tids[i],&pc)){snprintf(patch_stage,sizeof patch_stage,"registers TID%d: %s",stop.tids[i],strerror(errno));goto out;}
  for(unsigned j=0;j<count;j++)if(different[j]&&pc>=patches[j].address&&pc<(uint64_t)patches[j].address+patches[j].length){snprintf(patch_stage,sizeof patch_stage,"busy TID%d PC=%08llx in %08x+%u",stop.tids[i],(unsigned long long)pc,patches[j].address,patches[j].length);goto out;}
 }
 for(unsigned i=0;i<count;i++){
  const struct ub_code_patch *p=patches+i;written=i+1;if(!different[i])continue;
  if(memio(stop.mem,p->address,(void *)p->replacement,p->length,1)){snprintf(patch_stage,sizeof patch_stage,"write %08x: %s",p->address,strerror(errno));goto rollback;}
  if(memio(stop.mem,p->address,now,p->length,0)){snprintf(patch_stage,sizeof patch_stage,"verify read %08x: %s",p->address,strerror(errno));goto rollback;}
  if(memcmp(now,p->replacement,p->length)){snprintf(patch_stage,sizeof patch_stage,"verify differs at %08x",p->address);goto rollback;}
 }
 rc=0;snprintf(patch_stage,sizeof patch_stage,"ok");goto out;
 rollback:
 for(unsigned i=written;i>0;i--){const struct ub_code_patch *p=patches+i-1;if(different[i-1]&&restore_memory(&stop,pid,p->address,saved[i-1],p->length))stop.quarantine=1;}
 if(stop.quarantine){
  rc=-2;strncat(patch_stage,"; rollback unverified",sizeof patch_stage-strlen(patch_stage)-1);
  ub_log("injector",UB_ALWAYS,"FATAL: code patch rollback unverified for PID%d; process quarantined. Power cycle required.\n",pid);
 }else strncat(patch_stage,"; rollback verified",sizeof patch_stage-strlen(patch_stage)-1);
 out:if(stop.quarantine)rc=-2;if(stop.mem>=0)close(stop.mem);detach_all(&stop);return rc;
}
int ub_while_stopped(int pid,int (*fn)(int,int,void *),void *arg){
 struct stopped stop;int rc;
 if(stop_threads(pid,&stop))return -1;
 rc=fn(pid,stop.mem,arg);
 if(rc==-2)stop.quarantine=1;
 if(stop.mem>=0)close(stop.mem);
 detach_all(&stop);
 return rc;
}
const char *ub_overlay_error(int rc){
 switch(rc){
 case UB_OVERLAY_ARGS:return "bad arguments";
 case UB_OVERLAY_STOP:return "threads could not be paused";
 case UB_OVERLAY_READ:return "pages could not be read";
 case UB_OVERLAY_MAP:return "copy could not be mapped";
 case UB_OVERLAY_FILL:return "copy could not be filled";
 case UB_OVERLAY_SEAL:return "copy could not be made read-only";
 case UB_OVERLAY_SWAP:return "pages could not be swapped";
 case UB_OVERLAY_VERIFY:return "swapped pages differ";
 case UB_OVERLAY_RECOVER:return "mapping recovery failed; process quarantined";
 default:return "unexpected result";
 }
}
static const char *install_stage="";
static char stage_buf[200];
const char *ub_install_stage(void){return install_stage;}
/* 0 every hook site holds its stock bytes, 1 one holds anything else, -1 one
 * cannot be read (e.g. its library is not mapped yet). */
static int hook_sites(int mem,const struct ub_payload_variant *v){
 for(unsigned i=0;i<v->hook_count;i++){
  const struct ub_hook_spec *h=v->hooks+i;unsigned char b[32];
  if(h->length>sizeof b||memio(mem,h->target,b,h->length,0)){snprintf(stage_buf,sizeof stage_buf,"hook-sites: %08x unreadable",h->target);install_stage=stage_buf;return -1;}
  if(memcmp(b,h->expected,h->length)){snprintf(stage_buf,sizeof stage_buf,"hook-sites: %08x is not stock code",h->target);install_stage=stage_buf;return 1;}
 }
 return 0;
}
int ub_hook_sites(int pid,const struct ub_payload *p,const char *sha){
 const struct ub_payload_variant *v=ub_payload_variant(p,sha);char path[64];int fd,rc;
 install_stage="hook-sites";if(!v)return -1;
 snprintf(path,sizeof path,"/proc/%d/mem",pid);fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
 rc=hook_sites(fd,v);close(fd);return rc;
}
/* 1 if a thread's PC is inside a prefix about to be replaced, -1 if a PC
 * cannot be read. A thread that exited while being stopped has tid 0. */
static int prefix_busy(struct stopped *s,const struct ub_payload_variant *v){
 for(unsigned i=0;i<s->n;i++){
  uint64_t pc;if(!s->tids[i])continue;
  if(thread_pc(s->tids[i],&pc))return -1;
  for(unsigned j=0;j<v->hook_count;j++)if(pc>v->hooks[j].target&&pc<(uint64_t)v->hooks[j].target+v->hooks[j].length)return 1;
 }
 return 0;
}
int ub_install_payload(int pid,const struct ub_payload *p){
 char proc[64],digest[65];struct elf e;struct modules mods;struct stopped stop;unsigned char *image=0;uint32_t span=0,base=0,gateway=0;int rc=-1,attached=0,changed=0;unsigned i;uint32_t args[6],result;
 install_stage="hash";
 const struct ub_payload_variant *v=0;
 snprintf(proc,sizeof proc,"/proc/%d/exe",pid);if(ub_hash_file(proc,digest)||!(v=ub_payload_variant(p,digest))){ub_log("injector",UB_ALWAYS,"%s PID%d firmware hash mismatch\n",p->process,pid);return -1;}
 /* Hook sites are read before anything else and without stopping: code that
  * is not stock (an earlier helper run's hooks) is refused at once, and
  * unmapped sites cost no stop and no library scan. */
 if((rc=ub_hook_sites(pid,p,digest)))return rc;
 rc=-1;
 if(view(&e,p->elf,p->elf_size)||e.eh->e_type!=ET_DYN||e.eh->e_phentsize!=sizeof(Elf32_Phdr)||!range(e.n,e.eh->e_phoff,(size_t)e.eh->e_phnum*sizeof(Elf32_Phdr)))return -1;
 Elf32_Phdr *ph=(Elf32_Phdr *)(e.raw+e.eh->e_phoff);
 for(i=0;i<e.eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD){if(ph[i].p_vaddr>16*1024*1024||ph[i].p_memsz>16*1024*1024-ph[i].p_vaddr||ph[i].p_filesz>ph[i].p_memsz||!range(e.n,ph[i].p_offset,ph[i].p_filesz))return -1;if(ph[i].p_vaddr+ph[i].p_memsz>span)span=ph[i].p_vaddr+ph[i].p_memsz;}
 span=(span+4095u)&~4095u;if(!span||span>16*1024*1024)return -1;
 image=calloc(1,span);if(!image)return -1;
 for(i=0;i<e.eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD)memcpy(image+ph[i].p_vaddr,e.raw+ph[i].p_offset,ph[i].p_filesz);
 install_stage="maps";if(module_list(pid,&mods)){free(image);return -1;}
 /* Resolve before stopping any threads; base-zero local relocations get fixed
  * below. No filesystem scans or hashing while the UI is stopped. */
 install_stage="relocate";if(relocate(&e,image,span,0,&mods))goto out;
 install_stage="stop";if(stop_threads(pid,&stop)){ub_log("injector",UB_ALWAYS,"%s ptrace attach denied: %s\n",p->process,strerror(errno));goto out;}attached=1;
 install_stage="thread-busy";if(prefix_busy(&stop,v))goto out;
 install_stage="hook-bytes";{int sites=hook_sites(stop.mem,v);if(sites){rc=sites;goto out;}}
 install_stage="base-mmap";memset(args,0,sizeof args);args[1]=span+4096;args[2]=7;args[3]=0x4022;args[4]=0xffffffffu;
 {int src=remote_syscall(&stop,pid,192,args,&base);
  if(src||base>=0xfffff001u||base<65536u){snprintf(stage_buf,sizeof stage_buf,"base-mmap rc=%d ret=%08x len=%x | thread r7=%u orig=%08x oldpc=%08x stub=%08x trap=%08x stops=%u sig=%d",src,(unsigned)base,(unsigned)(span+4096),(unsigned)rsdbg.old_r7,(unsigned)rsdbg.old_orig,(unsigned)rsdbg.old_pc,(unsigned)rsdbg.pc,(unsigned)rsdbg.trap_pc,rsdbg.stops,rsdbg.lastsig);install_stage=stage_buf;goto out;}}
 /* Short prefixes need B.W reach to a gateway; a complete >=8-byte prefix
  * can use an absolute Thumb PC load when the same payload spans libraries.
  * A single nearby gateway page, not the entire DSO, needs B.W reach. Hints
  * begin in a gap between pinned ELF load segments. MAP_FIXED is never used;
  * an occupied hint is harmless and a distant kernel result is unmapped. */
 install_stage="gateway";for(unsigned trial=0;trial<19&&!gateway;trial++){
  static const uint32_t extra[3]={0x00c00000,0x00600000,0x00e00000};
  memset(args,0,sizeof args);args[0]=trial<16?v->gateway_hint+4096*trial:extra[trial-16];args[1]=4096;args[2]=7;args[3]=0x4022;args[4]=0xffffffffu;
  uint32_t allocated=0;if(remote_syscall(&stop,pid,192,args,&allocated)||allocated>=0xfffff001u||allocated<65536u)continue;
  int fits=1;for(i=0;i<v->hook_count;i++){unsigned char test[4];if(branch_thumb(v->hooks[i].target,(allocated+16*i)|1u,test)&&v->hooks[i].length<8)fits=0;}
  if(fits)gateway=allocated;else{memset(args,0,sizeof args);args[0]=allocated;args[1]=4096;remote_syscall(&stop,pid,91,args,&result);}
 }
 if(!gateway)goto rollback;
 /* Apply only base contributions to the already resolved local relocation image. */
 for(i=0;i<e.eh->e_shnum;i++){Elf32_Shdr *s=&e.sh[i];if(s->sh_type!=SHT_REL||!(s->sh_flags&SHF_ALLOC))continue;Elf32_Rel *r=(Elf32_Rel *)(e.raw+s->sh_offset);Elf32_Shdr *sy=&e.sh[s->sh_link];Elf32_Sym *a=(Elf32_Sym *)(e.raw+sy->sh_offset);for(unsigned j=0;j<s->sh_size/sizeof *r;j++){unsigned type=ELF32_R_TYPE(r[j].r_info),ix=ELF32_R_SYM(r[j].r_info);if(type==R_ARM_RELATIVE||((type==R_ARM_ABS32||type==R_ARM_GLOB_DAT||type==R_ARM_JUMP_SLOT)&&a[ix].st_shndx!=SHN_UNDEF))*(uint32_t *)(image+r[j].r_offset)+=base;}}
 for(i=0;i<v->binding_count;i++){uint32_t a=symbol_address(&e,0,v->bindings[i].symbol);if(!a||!range(span,a,4))goto rollback;memcpy(image+a,&v->bindings[i].value,4);}
 for(i=0;i<v->hook_count;i++){uint32_t a=symbol_address(&e,0,v->hooks[i].original_pointer),tramp=(base+span+256*i)|1u;if(!a||!range(span,a,4)||v->hooks[i].trampoline_length>256||i>=16)goto rollback;memcpy(image+a,&tramp,4);if(memio(stop.mem,tramp&~1u,(void *)v->hooks[i].trampoline,v->hooks[i].trampoline_length,1))goto rollback;}
 install_stage="map-image";if(memio(stop.mem,base,image,span,1)||cacheflush(&stop,pid,base,span+4096))goto rollback;
 for(i=0;i<v->hook_count;i++){unsigned char stub[8]={0xdf,0xf8,0x00,0xf0};uint32_t to=symbol_address(&e,base,v->hooks[i].replacement);if(!to)goto rollback;memcpy(stub+4,&to,4);if(memio(stop.mem,gateway+16*i,stub,8,1))goto rollback;}
 if(cacheflush(&stop,pid,gateway,4096))goto rollback;
 install_stage="patch";for(i=0;i<v->hook_count;i++){unsigned char b[32];uint32_t replacement=symbol_address(&e,base,v->hooks[i].replacement);if(hook_transfer(v->hooks[i].target,(gateway+16*i)|1u,replacement,v->hooks[i].length,b))goto rollback;changed=(int)i+1;if(memio(stop.mem,v->hooks[i].target,b,v->hooks[i].length,1))goto rollback;if(cacheflush(&stop,pid,v->hooks[i].target,v->hooks[i].length))goto rollback;}
 rc=0;install_stage="ok";ub_log("injector",UB_ALWAYS,"Installed %s PID%d payload at %08x (%u hooks, %s addresses)\n",p->process,pid,base,v->hook_count,v->firmware);goto out;
 rollback:
 {int restored=1;for(int j=changed-1;j>=0;j--){if(restore_memory(&stop,pid,v->hooks[j].target,v->hooks[j].expected,v->hooks[j].length))restored=0;}
 if(!restored||stop.quarantine){stop.quarantine=1;rc=-2;ub_log("injector",UB_ALWAYS,"FATAL: rollback not verified for %s PID%d; keep payload mapped and stop process. Power cycle required.\n",p->process,pid);goto out;}}
 /* Never unmap a successfully hooked callback; failure rollback runs while all
  * threads remain stopped and no payload instruction has executed. A separate
  * bounded cleanup budget still permits unmapping after the install deadline. */
 stop.until=ub_now_ms()+UB_PTRACE_PAUSE_MS;
 if(base){memset(args,0,sizeof args);args[0]=base;args[1]=span+4096;remote_syscall(&stop,pid,91,args,&result);}
 if(gateway){memset(args,0,sizeof args);args[0]=gateway;args[1]=4096;remote_syscall(&stop,pid,91,args,&result);}
 out:if(attached){if(stop.quarantine)rc=-2;if(stop.mem>=0)close(stop.mem);detach_all(&stop);}module_free(&mods);free(image);return rc;
}

/* Diagnostic: inject a throwaway 4 KiB mmap into pid and report the injected
 * thread's syscall state and the result, then unmap and detach. No code is
 * changed. Used by `engage --probe <pid> [count]` over SSH to see live why a
 * remote syscall returns a wrong value while the target is busy. */
int ub_probe(int pid){
 struct stopped stop;uint32_t args[6],res=0,r2=0;int rc;
 if(stop_threads(pid,&stop)){printf("probe PID%d stop_threads failed: %s\n",pid,strerror(errno));fflush(stdout);return -1;}
 memset(args,0,sizeof args);args[1]=4096;args[2]=3;args[3]=0x4022;args[4]=0xffffffffu;
 rc=remote_syscall(&stop,pid,192,args,&res);
 printf("probe PID%d threads=%u | stopstate pc=%08x r7=%u orig=%08x cpsr=%08x | mmap rc=%d res=%08x | stops=%u lastsig=%d stubpc=%08x trap=%08x\n",
  pid,stop.n,rsdbg.old_pc,rsdbg.old_r7,rsdbg.old_orig,rsdbg.old_cpsr,rc,res,rsdbg.stops,rsdbg.lastsig,rsdbg.pc,rsdbg.trap_pc);
 fflush(stdout);
 if(!rc&&res<0xfffff001u&&res>=65536u){memset(args,0,sizeof args);args[0]=res;args[1]=4096;remote_syscall(&stop,pid,91,args,&r2);}
 if(stop.mem>=0)close(stop.mem);
 detach_all(&stop);
 return rc;
}
