/* Version-pinned ARM32 native payload loader. No flash/filesystem firmware writes.
 * Payloads have no constructors. All UI/native calls occur on subsequent natural
 * hooked calls, never inside the ptrace bootstrap. Only mmap/cacheflush syscalls
 * execute while all enumerated process threads are stopped.
 */
#define _GNU_SOURCE
#include "injector.h"
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
static int load_file(const char *p,struct elf *e){int fd=open(p,O_RDONLY);struct stat st;unsigned char *b;size_t got=0;ssize_t n;if(fd<0)return -1;if(fstat(fd,&st)||st.st_size<52||st.st_size>64*1024*1024){close(fd);return -1;}b=malloc((size_t)st.st_size);if(!b){close(fd);return -1;}while(got<(size_t)st.st_size&&(n=read(fd,b+got,(size_t)st.st_size-got))>0)got+=(size_t)n;close(fd);if(got!=(size_t)st.st_size||view(e,b,got)){free(b);return -1;}return 0;}
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
static int module_list(int pid,struct modules *ms){char p[64],line[1200],name[600],perm[8];unsigned long start,end,off;FILE *f;memset(ms,0,sizeof *ms);ms->pid=pid;snprintf(p,sizeof p,"/proc/%d/mem",pid);ms->mem=open(p,O_RDWR);if(ms->mem<0)return -1;snprintf(p,sizeof p,"/proc/%d/maps",pid);f=fopen(p,"r");if(!f){close(ms->mem);return -1;}while(fgets(line,sizeof line,f)){name[0]=0;if(sscanf(line,"%lx-%lx %7s %lx %*s %*s %599[^\n]",&start,&end,perm,&off,name)!=5||name[0]!='/'||strstr(name,"(deleted)"))continue;unsigned i;for(i=0;i<ms->n;i++)if(!strcmp(ms->m[i].path,name))break;if(i<ms->n||ms->n==160)continue;struct elf e;if(load_file(name,&e))continue;uint32_t bias=0;unsigned j;int matched=0;if(e.eh->e_phentsize==sizeof(Elf32_Phdr)&&range(e.n,e.eh->e_phoff,(size_t)e.eh->e_phnum*sizeof(Elf32_Phdr))){Elf32_Phdr *ph=(Elf32_Phdr *)(e.raw+e.eh->e_phoff);for(j=0;j<e.eh->e_phnum;j++)if(ph[j].p_type==PT_LOAD&&(ph[j].p_offset&~4095u)==off){bias=(uint32_t)start-(ph[j].p_vaddr&~4095u);matched=1;break;}}free(e.raw);if(!matched)continue;struct module *m=&ms->m[ms->n++];if(strlen(name)>=sizeof m->path){ms->n--;continue;}memcpy(m->path,name,strlen(name)+1);m->start=(uint32_t)start;m->offset=(uint32_t)off;m->bias=bias;}fclose(f);return ms->n?0:-1;}
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
static void detach_all(struct stopped *s){while(s->n){int tid=s->tids[--s->n];if(ptrace(PTRACE_DETACH,tid,0,(void *)(uintptr_t)(s->quarantine?SIGSTOP:0))<0&&errno!=ESRCH)fprintf(stderr,"detach %d: %s\n",tid,strerror(errno));}}
static int stop_threads(int pid,struct stopped *s){unsigned pass;memset(s,0,sizeof *s);s->mem=-1;for(pass=0;pass<5;pass++){char path[64];snprintf(path,sizeof path,"/proc/%d/task",pid);DIR *d=opendir(path);struct dirent *de;unsigned added=0;if(!d)goto fail;while((de=readdir(d))){int tid=atoi(de->d_name);unsigned j;int st;if(tid<=0)continue;for(j=0;j<s->n;j++)if(s->tids[j]==tid)break;if(j<s->n)continue;if(s->n==256){closedir(d);goto fail;}if(ptrace(PTRACE_ATTACH,tid,0,0)){if(errno==ESRCH)continue;closedir(d);goto fail;}while(waitpid(tid,&st,__WALL)<0){if(errno!=EINTR){ptrace(PTRACE_DETACH,tid,0,0);closedir(d);goto fail;}}if(!WIFSTOPPED(st)){closedir(d);goto fail;}s->tids[s->n++]=tid;added++;}closedir(d);if(!added&&s->n){snprintf(path,sizeof path,"/proc/%d/mem",pid);s->mem=open(path,O_RDWR);if(s->mem<0)goto fail;return 0;}}fail:detach_all(s);return -1;}
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
static int remote_syscall(struct stopped *s,int tid,uint32_t number,const uint32_t args[6],uint32_t *result){
 struct regs old,r;unsigned char saved[8],stub[8]={0x00,0xdf,0xab,0xbe,0x00,0xbf,0x00,0xbf};uint32_t pc;int status,changed=0,rc=-1;
 if(getregs(tid,&old))return -1;
 r=old;pc=old.r[15]&~3u;
 if(memio(s->mem,pc,saved,8,0))return -1;
 changed=1;
 if(memio(s->mem,pc,stub,8,1))goto finish;
 for(unsigned i=0;i<6;i++)r.r[i]=args[i];
 r.r[7]=number;r.r[15]=pc;r.r[16]=(r.r[16]&~0x0600fc00u)|0x20u;r.r[17]=0xffffffffu;
 if(setregs(tid,&r)||ptrace(PTRACE_CONT,tid,0,0))goto finish;
 do {status=0;}while(waitpid(tid,&status,__WALL)<0&&errno==EINTR);
 if(!WIFSTOPPED(status)||WSTOPSIG(status)!=SIGTRAP||getregs(tid,&r))goto finish;
 if(r.r[15]!=pc+2&&r.r[15]!=pc+4)goto finish;
 *result=r.r[0];rc=0;
 finish:if(changed&&restore_memory(s,tid,pc,saved,8)){s->quarantine=1;rc=-1;}if(setregs(tid,&old)){s->quarantine=1;rc=-1;}return rc;
}
static int cacheflush(struct stopped *s,int tid,uint32_t a,uint32_t n){uint32_t r,args[6]={a,a+n,0,0,0,0};return remote_syscall(s,tid,0x0f0002,args,&r)||((int32_t)r<0)?-1:0;}

/* Relocations never invoke code and reject unsupported kinds/undefined imports. */
static int relocate(struct elf *e,unsigned char *image,uint32_t span,uint32_t base,struct modules *mods){
 unsigned i,j;for(i=0;i<e->eh->e_shnum;i++){Elf32_Shdr *rs=&e->sh[i];if(rs->sh_type==SHT_RELA)return -1;if(rs->sh_type!=SHT_REL||!(rs->sh_flags&SHF_ALLOC))continue;if(rs->sh_entsize!=sizeof(Elf32_Rel)||rs->sh_link>=e->eh->e_shnum)return -1;Elf32_Shdr *sy=&e->sh[rs->sh_link];if(sy->sh_type!=SHT_DYNSYM||sy->sh_entsize!=sizeof(Elf32_Sym))return -1;Elf32_Rel *r=(Elf32_Rel *)(e->raw+rs->sh_offset);Elf32_Sym *symbols=(Elf32_Sym *)(e->raw+sy->sh_offset);for(j=0;j<rs->sh_size/sizeof *r;j++){unsigned type=ELF32_R_TYPE(r[j].r_info),ix=ELF32_R_SYM(r[j].r_info);uint32_t value=0,*where;if(!range(span,r[j].r_offset,4)||r[j].r_offset%4||ix>=sy->sh_size/sizeof *symbols)return -1;where=(uint32_t *)(image+r[j].r_offset);if(type==R_ARM_NONE)continue;if(type==R_ARM_RELATIVE){*where+=base;continue;}if(symbols[ix].st_shndx==SHN_UNDEF){const char *name=symname(e,sy,symbols+ix);if(!name)return -1;value=resolve(mods,name);if(!value&&ELF32_ST_BIND(symbols[ix].st_info)!=STB_WEAK){fprintf(stderr,"Unresolved payload import %s\n",name);return -1;}}else value=base+symbols[ix].st_value;switch(type){case R_ARM_ABS32:*where+=value;break;case R_ARM_GLOB_DAT:case R_ARM_JUMP_SLOT:*where=value;break;default:fprintf(stderr,"Unsupported ARM relocation %u\n",type);return -1;}}}return 0;
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
int ub_install_payload(int pid,const struct ub_payload *p){
 char proc[64],digest[65];struct elf e;struct modules mods;struct stopped stop;unsigned char *image=0;uint32_t span=0,base=0,gateway=0;int rc=-1,attached=0,changed=0;unsigned i;uint32_t args[6],result;
 snprintf(proc,sizeof proc,"/proc/%d/exe",pid);if(ub_hash_file(proc,digest)||strcmp(digest,p->sha256)){fprintf(stderr,"%s PID%d firmware hash mismatch\n",p->process,pid);return -1;}
 if(view(&e,p->elf,p->elf_size)||e.eh->e_type!=ET_DYN||e.eh->e_phentsize!=sizeof(Elf32_Phdr)||!range(e.n,e.eh->e_phoff,(size_t)e.eh->e_phnum*sizeof(Elf32_Phdr)))return -1;
 Elf32_Phdr *ph=(Elf32_Phdr *)(e.raw+e.eh->e_phoff);
 for(i=0;i<e.eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD){if(ph[i].p_vaddr>16*1024*1024||ph[i].p_memsz>16*1024*1024-ph[i].p_vaddr||ph[i].p_filesz>ph[i].p_memsz||!range(e.n,ph[i].p_offset,ph[i].p_filesz))return -1;if(ph[i].p_vaddr+ph[i].p_memsz>span)span=ph[i].p_vaddr+ph[i].p_memsz;}
 span=(span+4095u)&~4095u;if(!span||span>16*1024*1024)return -1;
 image=calloc(1,span);if(!image)return -1;
 for(i=0;i<e.eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD)memcpy(image+ph[i].p_vaddr,e.raw+ph[i].p_offset,ph[i].p_filesz);
 if(module_list(pid,&mods)){free(image);return -1;}
 /* Resolve before stopping any threads; base-zero local relocations get fixed
  * below. No filesystem scans or hashing while the UI is stopped. */
 if(relocate(&e,image,span,0,&mods))goto out;
 if(stop_threads(pid,&stop)){fprintf(stderr,"%s ptrace attach denied: %s\n",p->process,strerror(errno));goto out;}attached=1;
 for(i=0;i<stop.n;i++){struct regs r;if(getregs(stop.tids[i],&r))goto out;for(unsigned j=0;j<p->hook_count;j++)if((r.r[15]&~1u)>p->hooks[j].target&&(r.r[15]&~1u)<p->hooks[j].target+p->hooks[j].length)goto out;}
 for(i=0;i<p->hook_count;i++){unsigned char b[32];if(p->hooks[i].length>32||memio(stop.mem,p->hooks[i].target,b,p->hooks[i].length,0)||memcmp(b,p->hooks[i].expected,p->hooks[i].length)){fprintf(stderr,"Hook bytes mismatch %s:%x\n",p->process,p->hooks[i].target);goto out;}}
 memset(args,0,sizeof args);args[1]=span+4096;args[2]=7;args[3]=0x22;args[4]=0xffffffffu;
 if(remote_syscall(&stop,pid,192,args,&base)||base>=0xfffff001u||base<65536u)goto out;
 /* A single nearby gateway page, not the entire DSO, needs B.W reach. Hints
  * begin in a gap between pinned ELF load segments. MAP_FIXED is never used;
  * an occupied hint is harmless and a distant kernel result is unmapped. */
 for(unsigned trial=0;trial<19&&!gateway;trial++){
  static const uint32_t extra[3]={0x00c00000,0x00600000,0x00e00000};
  memset(args,0,sizeof args);args[0]=trial<16?p->gateway_hint+4096*trial:extra[trial-16];args[1]=4096;args[2]=7;args[3]=0x22;args[4]=0xffffffffu;
  uint32_t allocated=0;if(remote_syscall(&stop,pid,192,args,&allocated)||allocated>=0xfffff001u||allocated<65536u)continue;
  int fits=1;for(i=0;i<p->hook_count;i++){unsigned char test[4];if(branch_thumb(p->hooks[i].target,(allocated+16*i)|1u,test))fits=0;}
  if(fits)gateway=allocated;else{memset(args,0,sizeof args);args[0]=allocated;args[1]=4096;remote_syscall(&stop,pid,91,args,&result);}
 }
 if(!gateway)goto rollback;
 /* Apply only base contributions to the already resolved local relocation image. */
 for(i=0;i<e.eh->e_shnum;i++){Elf32_Shdr *s=&e.sh[i];if(s->sh_type!=SHT_REL||!(s->sh_flags&SHF_ALLOC))continue;Elf32_Rel *r=(Elf32_Rel *)(e.raw+s->sh_offset);Elf32_Shdr *sy=&e.sh[s->sh_link];Elf32_Sym *a=(Elf32_Sym *)(e.raw+sy->sh_offset);for(unsigned j=0;j<s->sh_size/sizeof *r;j++){unsigned type=ELF32_R_TYPE(r[j].r_info),ix=ELF32_R_SYM(r[j].r_info);if(type==R_ARM_RELATIVE||((type==R_ARM_ABS32||type==R_ARM_GLOB_DAT||type==R_ARM_JUMP_SLOT)&&a[ix].st_shndx!=SHN_UNDEF))*(uint32_t *)(image+r[j].r_offset)+=base;}}
 for(i=0;i<p->binding_count;i++){uint32_t a=symbol_address(&e,0,p->bindings[i].symbol);if(!a||!range(span,a,4))goto rollback;memcpy(image+a,&p->bindings[i].value,4);}
 for(i=0;i<p->hook_count;i++){uint32_t a=symbol_address(&e,0,p->hooks[i].original_pointer),tramp=(base+span+256*i)|1u;if(!a||!range(span,a,4)||p->hooks[i].trampoline_length>256||i>=16)goto rollback;memcpy(image+a,&tramp,4);if(memio(stop.mem,tramp&~1u,(void *)p->hooks[i].trampoline,p->hooks[i].trampoline_length,1))goto rollback;}
 if(memio(stop.mem,base,image,span,1)||cacheflush(&stop,pid,base,span+4096))goto rollback;
 for(i=0;i<p->hook_count;i++){unsigned char stub[8]={0xdf,0xf8,0x00,0xf0};uint32_t to=symbol_address(&e,base,p->hooks[i].replacement);if(!to)goto rollback;memcpy(stub+4,&to,4);if(memio(stop.mem,gateway+16*i,stub,8,1))goto rollback;}
 if(cacheflush(&stop,pid,gateway,4096))goto rollback;
 for(i=0;i<p->hook_count;i++){unsigned char b[32];uint32_t to=(gateway+16*i)|1u;if(p->hooks[i].length<4||(p->hooks[i].target&3u))goto rollback;for(unsigned j=0;j<p->hooks[i].length;j+=2){b[j]=0x00;b[j+1]=0xbf;}if(branch_thumb(p->hooks[i].target,to,b))goto rollback;changed=(int)i+1;if(memio(stop.mem,p->hooks[i].target,b,p->hooks[i].length,1))goto rollback;if(cacheflush(&stop,pid,p->hooks[i].target,p->hooks[i].length))goto rollback;}
 rc=0;fprintf(stderr,"Installed %s PID%d payload at %08x (%u hooks)\n",p->process,pid,base,p->hook_count);goto out;
 rollback:
 {int restored=1;for(int j=changed-1;j>=0;j--){if(restore_memory(&stop,pid,p->hooks[j].target,p->hooks[j].expected,p->hooks[j].length)||cacheflush(&stop,pid,p->hooks[j].target,p->hooks[j].length))restored=0;}
 if(!restored||stop.quarantine){stop.quarantine=1;rc=-2;fprintf(stderr,"FATAL: rollback not verified for %s PID%d; keep payload mapped and stop process. Power cycle required.\n",p->process,pid);goto out;}}
 /* Never unmap a successfully hooked callback; failure rollback runs while all
  * threads remain stopped and no payload instruction has executed. */
 if(base){memset(args,0,sizeof args);args[0]=base;args[1]=span+4096;remote_syscall(&stop,pid,91,args,&result);}
 if(gateway){memset(args,0,sizeof args);args[0]=gateway;args[1]=4096;remote_syscall(&stop,pid,91,args,&result);}
 out:if(attached){if(stop.mem>=0)close(stop.mem);detach_all(&stop);}close(mods.mem);free(image);return rc;
}
