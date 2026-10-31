/* Version-pinned ARM32 native payload loader. No flash/filesystem firmware writes.
 * Payloads have no constructors. All UI/native calls occur on subsequent natural
 * hooked calls, never inside the ptrace bootstrap. Only mmap/cacheflush syscalls
 * execute while all enumerated process threads are stopped.
 */
#define _GNU_SOURCE
#include "injector.h"
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
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef PTRACE_GETSIGMASK
#define PTRACE_GETSIGMASK 0x420a
#endif
#ifndef PTRACE_SETSIGMASK
#define PTRACE_SETSIGMASK 0x420b
#endif

/* Small self-contained SHA256; hashes the immutable executable file, not RAM. */
struct hash {uint32_t h[8];uint64_t n;unsigned char b[64];unsigned used;};
static uint32_t rr(uint32_t x,unsigned n){return x>>n|x<<(32-n);}
static void block(struct hash *s,const unsigned char *b){
 static const uint32_t k[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
 uint32_t w[64],a,c,d,e,f,g,h,t1,t2,bv;unsigned i;
 for(i=0;i<16;i++)w[i]=(uint32_t)b[i*4]<<24|(uint32_t)b[i*4+1]<<16|(uint32_t)b[i*4+2]<<8|b[i*4+3];
 for(i=16;i<64;i++)w[i]=w[i-16]+(rr(w[i-15],7)^rr(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(rr(w[i-2],17)^rr(w[i-2],19)^(w[i-2]>>10));
 a=s->h[0];bv=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
 for(i=0;i<64;i++){t1=h+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^((~e)&g))+k[i]+w[i];t2=(rr(a,2)^rr(a,13)^rr(a,22))+((a&bv)^(a&c)^(bv&c));h=g;g=f;f=e;e=d+t1;d=c;c=bv;bv=a;a=t1+t2;}
 s->h[0]+=a;s->h[1]+=bv;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void hash_init(struct hash *s){static const uint32_t v[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};memset(s,0,sizeof *s);memcpy(s->h,v,sizeof v);}
static void hash_feed(struct hash *s,const void *v,size_t n){const unsigned char *p=v;s->n+=n;while(n--){s->b[s->used++]=*p++;if(s->used==64){block(s,s->b);s->used=0;}}}
static void hash_end(struct hash *s,char out[65]){uint64_t bits=s->n*8;unsigned i;unsigned char pad=0x80;hash_feed(s,&pad,1);pad=0;while(s->used!=56)hash_feed(s,&pad,1);for(i=0;i<8;i++)s->b[56+i]=(unsigned char)(bits>>(56-8*i));block(s,s->b);for(i=0;i<8;i++)sprintf(out+i*8,"%08x",s->h[i]);out[64]=0;}
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
int ub_hash_file(const char *p,char out[65]){int fd=open(p,O_RDONLY);ssize_t n;unsigned char buf[8192];struct hash h;if(fd<0)return -1;hash_init(&h);while((n=read(fd,buf,sizeof buf))>0)hash_feed(&h,buf,(size_t)n);close(fd);if(n<0)return -1;hash_end(&h,out);return 0;}

struct elf {unsigned char *raw;size_t n;Elf32_Ehdr *eh;Elf32_Shdr *sh;};
static int range(size_t n,size_t a,size_t z){return a<=n&&z<=n-a;}
static int view(struct elf *e,const void *raw,size_t n){
 unsigned i;e->raw=(unsigned char *)raw;e->n=n;e->eh=(Elf32_Ehdr *)raw;
 if(n<sizeof(Elf32_Ehdr)||memcmp(raw,ELFMAG,4)||e->eh->e_ident[EI_CLASS]!=ELFCLASS32||e->eh->e_ident[EI_DATA]!=ELFDATA2LSB||e->eh->e_machine!=EM_ARM||e->eh->e_shentsize!=sizeof(Elf32_Shdr)||!range(n,e->eh->e_shoff,(size_t)e->eh->e_shnum*sizeof(Elf32_Shdr)))return -1;
 e->sh=(Elf32_Shdr *)(e->raw+e->eh->e_shoff);
 for(i=0;i<e->eh->e_shnum;i++)if(e->sh[i].sh_type!=SHT_NOBITS&&!range(n,e->sh[i].sh_offset,e->sh[i].sh_size))return -1;
 return 0;
}
static int load_file(const char *p,struct elf *e){int fd=open(p,O_RDONLY);struct ub_stat st;unsigned char *b;size_t got=0;ssize_t n;if(fd<0)return -1;if(ub_fstat(fd,&st)||st.size<52||st.size>64*1024*1024){close(fd);return -1;}b=malloc((size_t)st.size);if(!b){close(fd);return -1;}while(got<(size_t)st.size&&(n=read(fd,b+got,(size_t)st.size-got))>0)got+=(size_t)n;close(fd);if(got!=(size_t)st.size||view(e,b,got)){free(b);return -1;}return 0;}
static const char *symname(struct elf *e,Elf32_Shdr *syms,Elf32_Sym *s){Elf32_Shdr *str;if(syms->sh_link>=e->eh->e_shnum)return 0;str=&e->sh[syms->sh_link];if(s->st_name>=str->sh_size)return 0;const char *p=(char *)e->raw+str->sh_offset+s->st_name;return memchr(p,0,str->sh_size-s->st_name)?p:0;}
static Elf32_Sym *findsym(struct elf *e,const char *name){unsigned i,j;for(i=0;i<e->eh->e_shnum;i++){Elf32_Shdr *s=&e->sh[i];if(s->sh_type!=SHT_DYNSYM||s->sh_entsize!=sizeof(Elf32_Sym))continue;Elf32_Sym *a=(Elf32_Sym *)(e->raw+s->sh_offset);for(j=0;j<s->sh_size/sizeof *a;j++){const char *n=symname(e,s,a+j);if(n&&!strcmp(n,name)&&a[j].st_shndx!=SHN_UNDEF)return a+j;}}return 0;}
struct module {char path[512];uint32_t start,offset,bias;};
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
static int module_list(int pid,struct modules *ms){static char text[256*1024];char p[64],*line,*save,name[600],perm[8];unsigned long start,end,off;memset(ms,0,sizeof *ms);ms->pid=pid;snprintf(p,sizeof p,"/proc/%d/mem",pid);ms->mem=open(p,O_RDWR);if(ms->mem<0)return -1;snprintf(p,sizeof p,"/proc/%d/maps",pid);if(ub_read_file(p,text,sizeof text)<0){close(ms->mem);return -1;}for(line=strtok_r(text,"\n",&save);line;line=strtok_r(0,"\n",&save)){name[0]=0;if(sscanf(line,"%lx-%lx %7s %lx %*s %*s %599[^\n]",&start,&end,perm,&off,name)!=5||name[0]!='/'||strstr(name,"(deleted)"))continue;unsigned i;for(i=0;i<ms->n;i++)if(!strcmp(ms->m[i].path,name))break;if(i<ms->n||ms->n==160)continue;struct elf e;if(load_file(name,&e))continue;uint32_t bias=0;unsigned j;int matched=0;if(e.eh->e_phentsize==sizeof(Elf32_Phdr)&&range(e.n,e.eh->e_phoff,(size_t)e.eh->e_phnum*sizeof(Elf32_Phdr))){Elf32_Phdr *ph=(Elf32_Phdr *)(e.raw+e.eh->e_phoff);for(j=0;j<e.eh->e_phnum;j++)if(ph[j].p_type==PT_LOAD&&(ph[j].p_offset&~4095u)==off){bias=(uint32_t)start-(ph[j].p_vaddr&~4095u);matched=1;break;}}free(e.raw);if(!matched)continue;struct module *m=&ms->m[ms->n++];if(strlen(name)>=sizeof m->path){ms->n--;continue;}memcpy(m->path,name,strlen(name)+1);m->start=(uint32_t)start;m->offset=(uint32_t)off;m->bias=bias;}return ms->n?0:-1;}
/* IFUNCs are resolved through an existing target PLT entry/GOT, not by calling
 * an IFUNC resolver as a normal function. Plain exported functions retain their
 * ARM/Thumb symbol bit. */
static uint32_t plt_symbol(struct modules *ms,const char *name){unsigned i,j,k;for(i=0;i<ms->n;i++){struct elf e;if(load_file(ms->m[i].path,&e))continue;Elf32_Shdr *plt=0;if(e.eh->e_shstrndx<e.eh->e_shnum){Elf32_Shdr *s=&e.sh[e.eh->e_shstrndx];for(j=0;j<e.eh->e_shnum;j++)if(e.sh[j].sh_name<s->sh_size&&!strcmp((char *)e.raw+s->sh_offset+e.sh[j].sh_name,".plt")){plt=&e.sh[j];break;}}for(j=0;j<e.eh->e_shnum&&plt;j++){Elf32_Shdr *r=&e.sh[j];if(r->sh_type!=SHT_REL||r->sh_link>=e.eh->e_shnum||r->sh_entsize!=sizeof(Elf32_Rel))continue;Elf32_Shdr *s=&e.sh[r->sh_link];if(s->sh_type!=SHT_DYNSYM)continue;Elf32_Rel *a=(Elf32_Rel *)(e.raw+r->sh_offset);Elf32_Sym *sy=(Elf32_Sym *)(e.raw+s->sh_offset);for(k=0;k<r->sh_size/sizeof *a;k++){if(ELF32_R_TYPE(a[k].r_info)!=R_ARM_JUMP_SLOT)continue;unsigned ix=ELF32_R_SYM(a[k].r_info);if(ix>=s->sh_size/sizeof *sy)continue;const char *n=symname(&e,s,sy+ix);if(n&&!strcmp(n,name)){uint32_t result=0;if(pread(ms->mem,&result,4,(off_t)(ms->m[i].bias+a[k].r_offset))!=4)result=0;uint32_t first=ms->m[i].bias+plt->sh_addr,last=first+plt->sh_size;if(result&&!(result>=first&&result<last)){free(e.raw);return result;}result=plt_for_got(&e,plt,ms->m[i].bias,ms->m[i].bias+a[k].r_offset);if(result){free(e.raw);return result;}}}}free(e.raw);}return 0;}
static uint32_t resolve(struct modules *ms,const char *name){
 /* /proc/maps is address order, not the dynamic linker's global scope order.
  * glibc2.20 also exports pthread mutex stubs. Bind pthread APIs to the real
  * loaded libpthread before considering libc's compatibility definitions. */
 unsigned i,pass,passes=!strncmp(name,"pthread_",8)?2:1;
 for(pass=0;pass<passes;pass++)for(i=0;i<ms->n;i++){
  if(passes==2&&pass==0&&!strstr(ms->m[i].path,"libpthread"))continue;
  struct elf e;if(load_file(ms->m[i].path,&e))continue;
  Elf32_Sym *s=findsym(&e,name);uint32_t v=s?ms->m[i].bias+s->st_value:0;
  unsigned type=s?ELF32_ST_TYPE(s->st_info):0;free(e.raw);
  if(v){if(type==STT_GNU_IFUNC)return plt_symbol(ms,name);return v;}
 }
 return 0;
}

struct regs {uint32_t r[18];};
struct stopped {int tids[256];unsigned n;int mem,quarantine;};
static void detach_all(struct stopped *s){while(s->n){int tid=s->tids[--s->n];if(ptrace(PTRACE_DETACH,tid,0,(void *)(uintptr_t)(s->quarantine?SIGSTOP:0))<0&&errno!=ESRCH)ub_log("injector",UB_ALWAYS,"detach %d: %s\n",tid,strerror(errno));}}
/* Attach every thread, re-scanning /proc/<pid>/task until a full pass adds none
 * (attached threads cannot spawn more, so the running set only shrinks). A busy
 * apl_dispsrv churns short-lived threads at boot, so allow many passes before
 * giving up; each pass is cheap and the caller still retries on a real miss. */
static int stop_threads(int pid,struct stopped *s){unsigned pass;memset(s,0,sizeof *s);s->mem=-1;for(pass=0;pass<64;pass++){char path[64];snprintf(path,sizeof path,"/proc/%d/task",pid);static struct ub_dir dir;struct ub_dir *d=&dir;const char *de;unsigned added=0;if(ub_dir_open(d,path))goto fail;while((de=ub_dir_next(d))){int tid=atoi(de);unsigned j;int st;if(tid<=0)continue;for(j=0;j<s->n;j++)if(s->tids[j]==tid)break;if(j<s->n)continue;if(s->n==256){ub_dir_close(d);goto fail;}if(ptrace(PTRACE_ATTACH,tid,0,0)){if(errno==ESRCH)continue;ub_dir_close(d);goto fail;}while(waitpid(tid,&st,__WALL)<0){if(errno!=EINTR){ptrace(PTRACE_DETACH,tid,0,0);ub_dir_close(d);goto fail;}}if(!WIFSTOPPED(st)){ub_dir_close(d);goto fail;}s->tids[s->n++]=tid;added++;}ub_dir_close(d);if(!added&&s->n){snprintf(path,sizeof path,"/proc/%d/mem",pid);s->mem=open(path,O_RDWR);if(s->mem<0)goto fail;return 0;}}fail:detach_all(s);return -1;}
static int memio(int fd,uint32_t a,void *buf,size_t n,int writing){size_t done=0;while(done<n){ssize_t k=writing?pwrite(fd,(char *)buf+done,n-done,(off_t)(a+done)):pread(fd,(char *)buf+done,n-done,(off_t)(a+done));if(k<=0)return -1;done+=(size_t)k;}return 0;}
static int restore_memory(struct stopped *s,int tid,uint32_t a,const void *bytes,size_t n){
 unsigned char verify[32];if(n>sizeof verify||n%4||a%4)return -1;
 if(memio(s->mem,a,(void *)bytes,n,1)){
  for(size_t i=0;i<n;i+=4){uint32_t word;memcpy(&word,(const char *)bytes+i,4);if(ptrace(PTRACE_POKETEXT,tid,(void *)(uintptr_t)(a+i),(void *)(uintptr_t)word))return -1;}
 }
 return memio(s->mem,a,verify,n,0)||memcmp(bytes,verify,n)?-1:0;
}
static int getregs(int tid,struct regs *r){return ptrace(PTRACE_GETREGS,tid,0,r);}
static int setregs(int tid,struct regs *r){return ptrace(PTRACE_SETREGS,tid,0,r);}
/* Diagnostics for ub_probe: the injected thread's state and the trap outcome of
 * the most recent remote_syscall. Not used by normal operation. */
static struct { uint32_t pc,trap_pc,old_pc,old_r7,old_orig,old_cpsr; unsigned stops; int lastsig; } rsdbg;
static int remote_syscall(struct stopped *s,int tid,uint32_t number,const uint32_t args[6],uint32_t *result){
 struct regs old,r;
 /* Thumb stub: bkpt, svc, bkpt, nop. On ARM Linux, when the target thread is
  * resumed from a signal-delivery-stop (which is what PTRACE_ATTACH on a
  * thread blocked in a syscall produces), the kernel's syscall-exit short path
  * can skip the first user instruction, landing straight on whatever is at
  * pc+2. Putting a sync bkpt at pc+0 catches BOTH cases: a clean resume at pc
  * fires the sync bkpt (we then step to pc+2 to run the svc), while a skipped
  * resume at pc+2 runs the svc directly and traps at the second bkpt (pc+4).
  * Either way, pc+4 is the final trap with mmap's return in r0. */
 unsigned char saved[8],stub[8]={0xab,0xbe,0x00,0xdf,0xac,0xbe,0x00,0xbf};
 uint32_t pc;int status,changed=0,rc=-1,maskok=0;
 uint64_t savemask=0,allmask=~0ull;
 if(getregs(tid,&old))return -1;
 r=old;pc=old.r[15]&~3u;
 rsdbg.old_pc=old.r[15];rsdbg.old_r7=old.r[7];rsdbg.old_orig=old.r[17];rsdbg.old_cpsr=old.r[16];rsdbg.pc=pc;rsdbg.trap_pc=0;rsdbg.stops=0;rsdbg.lastsig=0;
 if(memio(s->mem,pc,saved,8,0))return -1;
 changed=1;
 if(memio(s->mem,pc,stub,8,1))goto finish;
 for(unsigned i=0;i<6;i++)r.r[i]=args[i];
 r.r[7]=number;r.r[15]=pc;r.r[16]=(r.r[16]&~0x0600fc00u)|0x20u;r.r[17]=0xffffffffu;
 /* Block every signal in this thread for the length of the injected call. All
  * threads are frozen, so letting the C library's own thread-management signals
  * (SIGCANCEL/SIGSETXID) run a handler here would reach for a stopped peer and
  * corrupt the call. Blocked signals stay pending and are delivered once the
  * mask and registers below are restored and the process resumes. */
 maskok=!ptrace(PTRACE_GETSIGMASK,tid,(void *)(uintptr_t)sizeof savemask,&savemask);
 if(maskok)ptrace(PTRACE_SETSIGMASK,tid,(void *)(uintptr_t)sizeof allmask,&allmask);
 if(setregs(tid,&r)||ptrace(PTRACE_CONT,tid,0,0))goto finish;
 for(unsigned pass=0;;pass++){
  do {status=0;}while(waitpid(tid,&status,__WALL)<0&&errno==EINTR);
  if(!WIFSTOPPED(status))goto finish;
  rsdbg.stops++;rsdbg.lastsig=WSTOPSIG(status);
  if(WSTOPSIG(status)==SIGTRAP){
   if(getregs(tid,&r))goto finish;
   rsdbg.trap_pc=r.r[15];
   if(r.r[15]==pc){
    /* Sync bkpt (pc) fired. Advance to the svc at pc+2 and resume; next stop
     * is the post-svc bkpt at pc+4 with mmap's result in r0. */
    r.r[15]=pc+2;
    for(unsigned i=0;i<6;i++)r.r[i]=args[i];
    r.r[7]=number;r.r[17]=0xffffffffu;
    if(setregs(tid,&r)||ptrace(PTRACE_CONT,tid,0,0))goto finish;
    continue;
   }
   if(r.r[15]==pc+4)break; /* post-svc bkpt */
   goto finish; /* a trap somewhere unexpected */
  }
  /* With signals blocked only the unblockable ones (SIGKILL/SIGSTOP) can land;
   * never run a handler mid-injection, so swallow and continue to our trap. */
  if(pass>=8)goto finish;
  if(ptrace(PTRACE_CONT,tid,0,0))goto finish;
 }
 *result=r.r[0];rc=0;
 finish:
 if(maskok)ptrace(PTRACE_SETSIGMASK,tid,(void *)(uintptr_t)sizeof savemask,&savemask);
 if(changed&&restore_memory(s,tid,pc,saved,8)){s->quarantine=1;rc=-1;}if(setregs(tid,&old)){s->quarantine=1;rc=-1;}return rc;
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
/* Data-only page swap: a private anonymous copy is filled and sealed read-only,
 * then one mremap(MREMAP_MAYMOVE|MREMAP_FIXED) atomically takes the place of
 * the old pages. Until that call succeeds the target mapping is untouched.
 * Negative results name the step that failed (UB_OVERLAY_* in injector.h). */
int ub_overlay_pages(int pid,uint32_t a,const unsigned char *expected,const unsigned char *replacement,uint32_t n){
 struct stopped stop;unsigned char *now;uint32_t args[6],tmp=0,result=0;int rc=UB_OVERLAY_ARGS;
 if(!n||(a&4095u)||(n&4095u)||n>65536)return UB_OVERLAY_ARGS;
 now=malloc(n);if(!now)return UB_OVERLAY_ARGS;
 if(stop_threads(pid,&stop)){free(now);return UB_OVERLAY_STOP;}
 rc=UB_OVERLAY_READ;if(memio(stop.mem,a,now,n,0))goto out;
 if(!memcmp(now,replacement,n)){rc=0;goto out;}
 if(memcmp(now,expected,n)){rc=1;goto out;}
 rc=UB_OVERLAY_MAP;
 /* MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE. NORESERVE skips the overcommit
  * heuristic, which refuses a fresh mapping while the player is loading the
  * disc and RAM is tight; the pages are tiny and filled immediately below. */
 memset(args,0,sizeof args);args[1]=n;args[2]=3;args[3]=0x4022;args[4]=0xffffffffu;
 if(remote_syscall(&stop,pid,192,args,&tmp)||tmp>=0xfffff001u||tmp<65536u){tmp=0;goto out;}
 rc=UB_OVERLAY_FILL;if(memio(stop.mem,tmp,(void *)replacement,n,1))goto out;
 rc=UB_OVERLAY_SEAL;
 memset(args,0,sizeof args);args[0]=tmp;args[1]=n;args[2]=1;
 if(remote_syscall(&stop,pid,125,args,&result)||result)goto out;
 rc=UB_OVERLAY_SWAP;
 memset(args,0,sizeof args);args[0]=tmp;args[1]=n;args[2]=n;args[3]=3;args[4]=a;
 if(remote_syscall(&stop,pid,163,args,&result))goto out;
 if(result>=0xfffff001u)goto out;
 if(result!=a){tmp=result;goto out;}
 tmp=0;rc=memio(stop.mem,a,now,n,0)||memcmp(now,replacement,n)?UB_OVERLAY_VERIFY:0;
 out:if(tmp){memset(args,0,sizeof args);args[0]=tmp;args[1]=n;remote_syscall(&stop,pid,91,args,&result);}
 if(stop.mem>=0)close(stop.mem);
 detach_all(&stop);free(now);return rc;
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
 default:return "unexpected result";
 }
}
static const char *install_stage="";
static char stage_buf[200];
const char *ub_install_stage(void){return install_stage;}
int ub_install_payload(int pid,const struct ub_payload *p){
 char proc[64],digest[65];struct elf e;struct modules mods;struct stopped stop;unsigned char *image=0;uint32_t span=0,base=0,gateway=0;int rc=-1,attached=0,changed=0;unsigned i;uint32_t args[6],result;
 install_stage="hash";
 const struct ub_payload_variant *v=0;
 snprintf(proc,sizeof proc,"/proc/%d/exe",pid);if(ub_hash_file(proc,digest)||!(v=ub_payload_variant(p,digest))){ub_log("injector",UB_ALWAYS,"%s PID%d firmware hash mismatch\n",p->process,pid);return -1;}
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
 install_stage="thread-busy";for(i=0;i<stop.n;i++){struct regs r;if(getregs(stop.tids[i],&r))goto out;for(unsigned j=0;j<v->hook_count;j++)if((r.r[15]&~1u)>v->hooks[j].target&&(r.r[15]&~1u)<v->hooks[j].target+v->hooks[j].length)goto out;}
 install_stage="hook-bytes";for(i=0;i<v->hook_count;i++){unsigned char b[32];if(v->hooks[i].length>32||memio(stop.mem,v->hooks[i].target,b,v->hooks[i].length,0)||memcmp(b,v->hooks[i].expected,v->hooks[i].length)){ub_log("injector",UB_ALWAYS,"Hook bytes mismatch %s:%x\n",p->process,v->hooks[i].target);goto out;}}
 install_stage="base-mmap";memset(args,0,sizeof args);args[1]=span+4096;args[2]=7;args[3]=0x4022;args[4]=0xffffffffu;
 {int src=remote_syscall(&stop,pid,192,args,&base);
  if(src||base>=0xfffff001u||base<65536u){snprintf(stage_buf,sizeof stage_buf,"base-mmap rc=%d ret=%08x len=%x | thread r7=%u orig=%08x oldpc=%08x stub=%08x trap=%08x stops=%u sig=%d",src,(unsigned)base,(unsigned)(span+4096),(unsigned)rsdbg.old_r7,(unsigned)rsdbg.old_orig,(unsigned)rsdbg.old_pc,(unsigned)rsdbg.pc,(unsigned)rsdbg.trap_pc,rsdbg.stops,rsdbg.lastsig);install_stage=stage_buf;goto out;}}
 /* A single nearby gateway page, not the entire DSO, needs B.W reach. Hints
  * begin in a gap between pinned ELF load segments. MAP_FIXED is never used;
  * an occupied hint is harmless and a distant kernel result is unmapped. */
 install_stage="gateway";for(unsigned trial=0;trial<19&&!gateway;trial++){
  static const uint32_t extra[3]={0x00c00000,0x00600000,0x00e00000};
  memset(args,0,sizeof args);args[0]=trial<16?v->gateway_hint+4096*trial:extra[trial-16];args[1]=4096;args[2]=7;args[3]=0x4022;args[4]=0xffffffffu;
  uint32_t allocated=0;if(remote_syscall(&stop,pid,192,args,&allocated)||allocated>=0xfffff001u||allocated<65536u)continue;
  int fits=1;for(i=0;i<v->hook_count;i++){unsigned char test[4];if(branch_thumb(v->hooks[i].target,(allocated+16*i)|1u,test))fits=0;}
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
 install_stage="patch";for(i=0;i<v->hook_count;i++){unsigned char b[32];uint32_t to=(gateway+16*i)|1u;if(v->hooks[i].length<4||(v->hooks[i].target&3u))goto rollback;for(unsigned j=0;j<v->hooks[i].length;j+=2){b[j]=0x00;b[j+1]=0xbf;}if(branch_thumb(v->hooks[i].target,to,b))goto rollback;changed=(int)i+1;if(memio(stop.mem,v->hooks[i].target,b,v->hooks[i].length,1))goto rollback;if(cacheflush(&stop,pid,v->hooks[i].target,v->hooks[i].length))goto rollback;}
 rc=0;install_stage="ok";ub_log("injector",UB_ALWAYS,"Installed %s PID%d payload at %08x (%u hooks, %s addresses)\n",p->process,pid,base,v->hook_count,v->firmware);goto out;
 rollback:
 {int restored=1;for(int j=changed-1;j>=0;j--){if(restore_memory(&stop,pid,v->hooks[j].target,v->hooks[j].expected,v->hooks[j].length)||cacheflush(&stop,pid,v->hooks[j].target,v->hooks[j].length))restored=0;}
 if(!restored||stop.quarantine){stop.quarantine=1;rc=-2;ub_log("injector",UB_ALWAYS,"FATAL: rollback not verified for %s PID%d; keep payload mapped and stop process. Power cycle required.\n",p->process,pid);goto out;}}
 /* Never unmap a successfully hooked callback; failure rollback runs while all
  * threads remain stopped and no payload instruction has executed. */
 if(base){memset(args,0,sizeof args);args[0]=base;args[1]=span+4096;remote_syscall(&stop,pid,91,args,&result);}
 if(gateway){memset(args,0,sizeof args);args[0]=gateway;args[1]=4096;remote_syscall(&stop,pid,91,args,&result);}
 out:if(attached){if(stop.mem>=0)close(stop.mem);detach_all(&stop);}close(mods.mem);free(image);return rc;
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
