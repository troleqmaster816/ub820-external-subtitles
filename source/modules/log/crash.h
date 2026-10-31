/* Crash reports (diagnostics=1). Included by module.c after ub.h.
 *
 * The kernel's core_pattern is pointed at this helper ("|engage --core %p %s
 * %e"), so when any program crashes, the kernel starts a new engage process
 * and pipes the core to its stdin. The crashed process is still present while
 * that happens, so its memory map and stack can be read. Only the ELF header
 * and notes are parsed (signal, fault address, registers of the crashing
 * thread, which the kernel writes first); the rest is read and discarded.
 * Diagnostics also set each process's coredump_filter to 0, so the core holds
 * little beyond those notes. Nothing is written anywhere but the RAM log. */
#ifndef UB_LOG_CRASH_H
#define UB_LOG_CRASH_H
#include <signal.h>

#define CRASH_ID "crash"
#define CRASH_HEAD (256*1024)
#define CRASH_SECONDS 30

static const char *signal_name(int sig){
 static const char *const names[]={0,"SIGHUP","SIGINT","SIGQUIT","SIGILL","SIGTRAP","SIGABRT","SIGBUS","SIGFPE","SIGKILL","SIGUSR1",
  "SIGSEGV","SIGUSR2","SIGPIPE","SIGALRM","SIGTERM","SIGSTKFLT","SIGCHLD","SIGCONT","SIGSTOP","SIGTSTP","SIGTTIN","SIGTTOU",
  "SIGURG","SIGXCPU","SIGXFSZ","SIGVTALRM","SIGPROF","SIGWINCH","SIGIO","SIGPWR","SIGSYS"};
 static char other[16];
 if(sig>0&&sig<(int)(sizeof names/sizeof *names))return names[sig];
 snprintf(other,sizeof other,"signal %d",sig);return other;
}

/* Memory map of a process, for "library+offset" addresses. */
struct crash_map { uint32_t start,end,offset; char exec; char name[40]; };
#define CRASH_MAPS 512
static unsigned maps_parse(const char *text,struct crash_map *out,unsigned cap){
 unsigned n=0;
 while(*text&&n<cap){
  unsigned long s,e,o;char perm[8],path[256];int used=0;const char *nl=strchr(text,'\n');
  path[0]=0;
  if(sscanf(text,"%lx-%lx %7s %lx %*s %*s%n",&s,&e,perm,&o,&used)==4){
   const char *p=text+used,*base;size_t len;
   while(*p==' ')p++;
   len=nl?(size_t)(nl-p):strlen(p);if(len>=sizeof path)len=sizeof path-1;
   memcpy(path,p,len);path[len]=0;
   base=strrchr(path,'/');base=base?base+1:path;
   out[n].start=(uint32_t)s;out[n].end=(uint32_t)e;out[n].offset=(uint32_t)o;out[n].exec=perm[2]=='x';
   snprintf(out[n].name,sizeof out[n].name,"%.39s",*base?base:"anon");n++;
  }
  if(!nl)break;
  text=nl+1;
 }
 return n;
}
static const struct crash_map *map_of(const struct crash_map *m,unsigned n,uint32_t a){
 for(unsigned i=0;i<n;i++)if(a>=m[i].start&&a<m[i].end)return &m[i];
 return 0;
}
/* "0x2a38a6d4 libnavi_api.so+0x4a6d4": the file offset, as in a disassembler
 * opened on that file. */
static void where(const struct crash_map *m,unsigned n,uint32_t a,char *out,size_t size){
 const struct crash_map *x=map_of(m,n,a);
 if(x)snprintf(out,size,"0x%08x %s+0x%x",a,x->name,a-x->start+x->offset);
 else snprintf(out,size,"0x%08x",a);
}
static unsigned maps_read(int pid,struct crash_map *out,unsigned cap){
 static char text[64*1024];char path[64];int fd;ssize_t n,got=0;
 snprintf(path,sizeof path,"/proc/%d/maps",pid);
 if((fd=open(path,O_RDONLY))<0)return 0;
 while(got<(ssize_t)sizeof text-1&&(n=read(fd,text+got,sizeof text-1-(size_t)got))>0)got+=n;
 close(fd);text[got]=0;
 return maps_parse(text,out,cap);
}

/* What the notes of an ARM ELF core say about the crash. */
struct crash_info {
 int tid,signo,code,have_regs,have_addr;
 uint32_t addr,reg[18]; /* r0..r15, cpsr, orig_r0 */
 char fname[17],args[81];
};
static uint32_t rd32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint16_t rd16(const unsigned char *p){return (uint16_t)(p[0]|p[1]<<8);}
/* Bytes of the core needed to reach the end of its notes, 0 if this is not
 * an ARM core; parse once that many are in. */
static size_t crash_need(const unsigned char *core,size_t n){
 uint32_t phoff,i;uint16_t phnum,phent;
 if(n<52||memcmp(core,"\177ELF\1\1",6)||rd16(core+16)!=4||rd16(core+18)!=40)return 0;
 phoff=rd32(core+28);phent=rd16(core+42);phnum=rd16(core+44);
 if(phent<32||phoff>CRASH_HEAD||(size_t)phoff+(size_t)phent*phnum>CRASH_HEAD)return 0;
 if(n<phoff+(size_t)phent*phnum)return phoff+(size_t)phent*phnum;
 for(i=0;i<phnum;i++){
  const unsigned char *ph=core+phoff+i*phent;
  if(rd32(ph)==4){size_t end=(size_t)rd32(ph+4)+rd32(ph+16);return end>CRASH_HEAD?CRASH_HEAD:end;}
 }
 return 0;
}
static int crash_parse(const unsigned char *core,size_t n,struct crash_info *ci){
 uint32_t phoff,i,off=0,end=0;uint16_t phnum,phent;
 memset(ci,0,sizeof *ci);
 if(!crash_need(core,n))return -1;
 phoff=rd32(core+28);phent=rd16(core+42);phnum=rd16(core+44);
 for(i=0;i<phnum;i++){const unsigned char *ph=core+phoff+i*phent;if(rd32(ph)==4){off=rd32(ph+4);end=off+rd32(ph+16);break;}}
 if(!end||off>n)return -1;
 if(end>n)end=(uint32_t)n;
 while(off+12<=end){
  uint32_t namesz=rd32(core+off),descsz=rd32(core+off+4),type=rd32(core+off+8);
  uint32_t desc=off+12+((namesz+3)&~3u);const unsigned char *d=core+desc;
  if(namesz>64||descsz>end||desc>end||descsz>end-desc)break;
  if(type==1&&!ci->have_regs&&descsz>=148){ /* NT_PRSTATUS: the crashing thread comes first */
   ci->signo=rd16(d+12);ci->tid=(int)rd32(d+24);
   for(unsigned r=0;r<18;r++)ci->reg[r]=rd32(d+72+4*r);
   ci->have_regs=1;
  }else if(type==3&&descsz>=124){ /* NT_PRPSINFO */
   memcpy(ci->fname,d+28,16);ci->fname[16]=0;memcpy(ci->args,d+44,80);ci->args[80]=0;
  }else if(type==0x53494749&&descsz>=16){ /* NT_SIGINFO */
   ci->signo=(int)rd32(d);ci->code=(int)rd32(d+8);ci->addr=rd32(d+12);
   ci->have_addr=ci->signo==SIGSEGV||ci->signo==SIGBUS||ci->signo==SIGILL||ci->signo==SIGFPE;
  }
  off=desc+((descsz+3)&~3u);
 }
 return ci->have_regs?0:-1;
}

/* Words on the stack that point into code: likely return addresses. */
static void stack_scan(int pid,uint32_t sp,const struct crash_map *m,unsigned n,char *out,size_t size){
 uint32_t words[256];char path[64],one[80];int fd;ssize_t got;unsigned found=0;size_t used=0;
 out[0]=0;
 snprintf(path,sizeof path,"/proc/%d/mem",pid);
 if((fd=open(path,O_RDONLY))<0)return;
 got=pread(fd,words,sizeof words,(off_t)sp);close(fd);
 for(ssize_t i=0;i<got/4&&found<8;i++){
  const struct crash_map *x=map_of(m,n,words[i]);
  if(!x||!x->exec)continue;
  where(m,n,words[i],one,sizeof one);
  used+=(size_t)snprintf(out+used,used<size?size-used:0,"%s%s",found?", ":"",strchr(one,' ')+1);
  if(used>=size)break;
  found++;
 }
}

static void crash_report(int pid,int sig,const char *comm,const struct crash_info *ci){
 static struct crash_map maps[CRASH_MAPS];
 char path[64],thread[24]="",pc[80],lr[80],stack[400];unsigned n=maps_read(pid,maps,CRASH_MAPS);int fd;ssize_t k;
 if(ci->tid){
  snprintf(path,sizeof path,"/proc/%d/task/%d/comm",pid,ci->tid);
  if((fd=open(path,O_RDONLY))>=0){k=read(fd,thread,sizeof thread-1);close(fd);thread[k>0?k:0]=0;thread[strcspn(thread,"\n")]=0;}
 }
 if(ci->have_addr)ub_log(CRASH_ID,UB_ALWAYS,"%s PID%d crashed: %s at address 0x%08x (code %d), thread '%s' TID%d",comm,pid,signal_name(ci->signo?ci->signo:sig),ci->addr,ci->code,thread,ci->tid);
 else ub_log(CRASH_ID,UB_ALWAYS,"%s PID%d crashed: %s, thread '%s' TID%d",comm,pid,signal_name(ci->signo?ci->signo:sig),thread,ci->tid);
 if(!ci->have_regs){ub_log(CRASH_ID,UB_ALWAYS,"  no register notes in the core");return;}
 where(maps,n,ci->reg[15],pc,sizeof pc);where(maps,n,ci->reg[14],lr,sizeof lr);
 ub_log(CRASH_ID,UB_ALWAYS,"  pc %s",pc);
 ub_log(CRASH_ID,UB_ALWAYS,"  lr %s",lr);
 ub_log(CRASH_ID,UB_ALWAYS,"  r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x",ci->reg[0],ci->reg[1],ci->reg[2],ci->reg[3],ci->reg[4],ci->reg[5],ci->reg[6],ci->reg[7]);
 ub_log(CRASH_ID,UB_ALWAYS,"  r8 %08x r9 %08x r10 %08x fp %08x ip %08x sp %08x cpsr %08x",ci->reg[8],ci->reg[9],ci->reg[10],ci->reg[11],ci->reg[12],ci->reg[13],ci->reg[16]);
 stack_scan(pid,ci->reg[13],maps,n,stack,sizeof stack);
 if(stack[0])ub_log(CRASH_ID,UB_ALWAYS,"  code addresses on the stack: %s",stack);
 if(ci->args[0])ub_log(CRASH_ID,UB_ALWAYS,"  command: %s",ci->args);
}

/* engage --core <pid> <signal> <comm...>, started by the kernel. */
static int crash_command(int argc,char **argv){
 static unsigned char head[CRASH_HEAD];
 char comm[64]="";size_t got=0,need=52,used=0;ssize_t n;struct crash_info ci;int pid,sig,fd;
 if(argc<4||strcmp(argv[1],"--core"))return -1;
 pid=atoi(argv[2]);sig=atoi(argv[3]);
 for(int i=4;i<argc&&used<sizeof comm;i++)used+=(size_t)snprintf(comm+used,sizeof comm-used,"%s%s",i>4?" ":"",argv[i]);
 alarm(CRASH_SECONDS); /* never hold the crashed process for long */
 ub_log_open();
 while(got<need&&(n=read(0,head+got,need-got))>0){
  size_t want;got+=(size_t)n;
  if(got==need&&(want=crash_need(head,got))>need)need=want;
 }
 if(crash_parse(head,got,&ci))ub_log(CRASH_ID,UB_ALWAYS,"%s PID%d crashed: %s (core not readable)",comm,pid,signal_name(sig));
 else crash_report(pid,sig,comm,&ci);
 while(read(0,head,sizeof head)>0){}
 if((fd=open(UB_LOG_URGENT,O_WRONLY|O_CREAT,0600))>=0)close(fd);
 return 0;
}
#endif
