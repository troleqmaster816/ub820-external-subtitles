#define _GNU_SOURCE
#include "ub.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>

#define UB_LOCK "/tmp/ub820_submenu.supervisor.lock"
#define UB_NAMES 16
#define UB_HASHES 8

struct linux_dirent64 {unsigned long long d_ino;long long d_off;unsigned short d_reclen;unsigned char d_type;char d_name[];};

/* One /proc scan per second for every process name a part asks for. */
static struct { const char *name; int pid; } names[UB_NAMES];
static unsigned name_count;
static void want(const char *name){
 for(unsigned i=0;i<name_count;i++)if(!strcmp(names[i].name,name))return;
 if(name_count<UB_NAMES)names[name_count++].name=name;
}
static void scan(void){
 char buf[4096] __attribute__((aligned(8))),p[64],comm[64];int n,fd;unsigned i;
 for(i=0;i<name_count;i++)names[i].pid=0;
 fd=open("/proc",O_RDONLY|O_DIRECTORY);if(fd<0)return;
 while((n=(int)syscall(SYS_getdents64,fd,buf,sizeof buf))>0){
  int off=0;
  while(off<n){
   struct linux_dirent64 *e=(void *)(buf+off);int pid,cfd;ssize_t k;
   if(e->d_reclen<20||off+e->d_reclen>n)break;
   off+=e->d_reclen;pid=atoi(e->d_name);if(pid<=0)continue;
   snprintf(p,sizeof p,"/proc/%d/comm",pid);cfd=open(p,O_RDONLY);if(cfd<0)continue;
   k=read(cfd,comm,sizeof comm-1);close(cfd);if(k<=0)continue;
   comm[k]=0;comm[strcspn(comm,"\r\n")]=0;
   for(i=0;i<name_count;i++)if(!names[i].pid&&!strcmp(comm,names[i].name))names[i].pid=pid;
  }
 }
 close(fd);
}
static int pid_of(const char *name){for(unsigned i=0;i<name_count;i++)if(!strcmp(names[i].name,name))return names[i].pid;return 0;}
static unsigned long long birth(int pid){
 char p[64],buf[2048],*q,*save;int fd;ssize_t n;unsigned field=3;
 snprintf(p,sizeof p,"/proc/%d/stat",pid);fd=open(p,O_RDONLY);if(fd<0)return 0;
 n=read(fd,buf,sizeof buf-1);close(fd);if(n<=0)return 0;buf[n]=0;
 q=strrchr(buf,')');if(!q)return 0;
 for(q=strtok_r(q+1," ",&save);q;q=strtok_r(0," ",&save),field++)if(field==22)return strtoull(q,0,10);
 return 0;
}
/* Executable hash per process instance, shared by all parts. */
static struct { int pid; unsigned long long born; char sha[65]; } hashes[UB_HASHES];
static const char *exe_hash(int pid,unsigned long long born){
 static unsigned next;char path[64];unsigned i;
 for(i=0;i<UB_HASHES;i++)if(hashes[i].pid==pid&&hashes[i].born==born)return hashes[i].sha;
 i=next++%UB_HASHES;snprintf(path,sizeof path,"/proc/%d/exe",pid);
 if(ub_hash_file(path,hashes[i].sha))return 0;
 hashes[i].pid=pid;hashes[i].born=born;return hashes[i].sha;
}

static unsigned change_order;
void ub_changed(struct ub_part *part){if(!part->target.changed)part->target.changed=++change_order;}

#define FOR_PARTS(m,p) for(struct ub_module *const *mm=ub_modules;(m=*mm);mm++) for(unsigned pi=0;pi<m->part_count&&(p=&m->parts[pi]);pi++)

int ub_settled(const char *process){
 struct ub_module *m;struct ub_part *p;int pid=pid_of(process);
 FOR_PARTS(m,p){
  if(!m->enabled||p->tripped||!p->payload||!p->process||strcmp(p->process,process))continue;
  if(p->target.pid!=pid||!p->target.verified)return 0;
  if(p->target.verified==1&&!p->target.installed&&!p->target.failed)return 0;
 }
 return 1;
}

/* A process instance ended: switch off the guarded part that changed it most
 * recently, then forget the instance for every part that tracked it. */
static void ended(const char *process,int pid,unsigned long long born){
 struct ub_module *m;struct ub_part *p,*newest=0;
 FOR_PARTS(m,p){
  if(!p->process||strcmp(p->process,process)||p->target.pid!=pid||p->target.born!=born)continue;
  if(p->guarded&&!p->tripped&&p->target.changed&&(!newest||p->target.changed>newest->target.changed))newest=p;
 }
 if(newest){newest->tripped=1;ub_log(newest->module,UB_ALWAYS,"%s PID%d ended after part '%s' changed it; that part is off until power cycle",process,pid,newest->name);}
 else ub_log("core",UB_INFO,"%s PID%d ended",process,pid);
 FOR_PARTS(m,p)if(p->process&&!strcmp(p->process,process)&&p->target.pid==pid&&p->target.born==born){memset(&p->target,0,sizeof p->target);}
}

static void install(struct ub_part *p,struct ub_target *t){
 int rc;
 if(t->retry){t->retry--;return;}
 if(p->prepare)p->prepare(p,t);
 rc=ub_install_payload(t->pid,p->payload);
 if(rc==0){t->installed=1;ub_changed(p);return;}
 if(rc==-2){t->failed=1;return;}
 /* Bounded retries; a persistent incompatibility fails closed. */
 t->attempts++;t->retry=1;
 if(t->attempts>=20){t->failed=1;ub_log(p->module,UB_ALWAYS,"giving up installing %s in PID%d (stalled at %s)",p->process,t->pid,ub_install_stage());}
 else ub_log(p->module,UB_INFO,"install attempt %d for '%s' in %s PID%d did not complete (%s); retrying",t->attempts,p->name,p->process,t->pid,ub_install_stage());
}

static void tick(struct ub_part *p){
 struct ub_target *t=&p->target;int pid;unsigned long long born;
 if(!p->process){if(p->step)p->step(p,t);return;}
 pid=pid_of(p->process);born=pid?birth(pid):0;
 if(pid&&pid==t->pid&&!born)return; /* /proc/<pid>/stat briefly unreadable: keep state */
 if(pid!=t->pid||born!=t->born){
  if(t->pid&&t->born){
   unsigned long long now=birth(t->pid);
   int gone=(kill(t->pid,0)&&errno==ESRCH)||(now&&now!=t->born);
   if(!gone)return; /* the tracked instance still runs; a missed scan is not an end */
   ended(p->process,t->pid,t->born);
  }
  if(t->pid!=pid||t->born!=born){memset(t,0,sizeof *t);t->pid=pid;t->born=born;}
 }
 if(!t->pid||!t->born||p->tripped)return;
 if(!t->verified){
  const char *h=exe_hash(t->pid,t->born);if(!h)return;
  /* A part with a payload runs where the payload has a variant (its own
   * addresses, or the 1.82 ones for a code-identical 1.85 build); a part
   * without one where ub_firmware_match accepts the file. */
  const struct ub_payload_variant *v=p->payload?ub_payload_variant(p->payload,h):0;
  int match=p->payload?(v?(strcmp(h,p->sha256)?185:182):0):ub_firmware_match(h,p->sha256);
  t->verified=match?1:-1;
  if(t->verified<0)ub_log(p->module,UB_ALWAYS,"%s PID%d hash is not a supported 1.82 or 1.85 build; part '%s' makes no changes",p->process,t->pid,p->name);
  else if(match==185)ub_log(p->module,UB_ALWAYS,"%s PID%d is a supported 1.85 build%s; part '%s' runs (not confirmed on a player)",p->process,t->pid,v&&v!=p->payload->variants?" (its own 1.85 addresses)":"",p->name);
 }
 if(t->verified!=1)return;
 if(p->payload&&!t->installed){if(!t->failed)install(p,t);if(!t->installed)return;}
 if(p->step)p->step(p,t);
}

/* The helper is started from inside a player process; what it inherited
 * there (blocked and ignored signals) is logged once, since every program it
 * starts would get it too unless reset (the ssh module resets it). */
static void inherited(void){
 char text[4096],out[160],*line,*save;size_t used=0;
 if(ub_read_file("/proc/self/status",text,sizeof text)<0)return;
 for(line=strtok_r(text,"\n",&save);line;line=strtok_r(0,"\n",&save)){
  if(strncmp(line,"SigBlk:",7)&&strncmp(line,"SigIgn:",7))continue;
  for(char *c=line;*c;c++)if(*c=='\t')*c=' ';
  used+=(size_t)snprintf(out+used,used<sizeof out?sizeof out-used:0,"%s%s",used?", ":"",line);
 }
 if(used)ub_log("core",UB_ALWAYS,"inherited signal state: %s",out);
}
void ub_core_start(void){
 struct ub_module *m;struct ub_part *p;
 ub_log("core",UB_ALWAYS,"UB820 QoL helper starting (RAM only)");
 inherited();
 ub_config_load();
 ub_log_level=UB_ALWAYS; /* raised by the log module when it is enabled */
 FOR_PARTS(m,p){p->module=m->id;if(m->enabled&&p->process)want(p->process);}
 for(struct ub_module *const *mm=ub_modules;(m=*mm);mm++){
  char opts[256];size_t n=0;opts[0]=0;
  for(const struct ub_option *o=m->options;o&&o->key&&n<sizeof opts;o++)n+=(size_t)snprintf(opts+n,sizeof opts-n," %s=%d",o->key,*o->value);
  if(m->enabled&&m->start)m->start(m);
  ub_log("core",UB_ALWAYS,"module %s %s%s",m->id,m->enabled?"on":"off",opts);
 }
}
void ub_core_tick(void){
 struct ub_module *m;struct ub_part *p;
 scan();
 FOR_PARTS(m,p)if(m->enabled)tick(p);
}

#ifndef UB_HOST_TEST
int main(int argc,char **argv){
 struct ub_module *m;int lock;
 if(argc==3&&!strcmp(argv[1],"--hash")){char h[65];if(ub_hash_file(argv[2],h))return 2;puts(h);return 0;}
 if(argc==2&&!strcmp(argv[1],"--selftest")){char h[65];if(ub_hash_file("/proc/self/exe",h))return 2;printf("ARM native supervisor self-test: SHA256=%s\n",h);return 0;}
 if(argc==2&&!strcmp(argv[1],"--modules")){
  for(struct ub_module *const *mm=ub_modules;(m=*mm);mm++){
   printf("[%s] %s (default %s)\n",m->id,m->summary,m->enabled?"on":"off");
   for(const struct ub_option *o=m->options;o&&o->key;o++)printf("  %s=%d (%d..%d)\n",o->key,*o->value,o->min,o->max);
  }
  return 0;
 }
 if(argc>=3&&!strcmp(argv[1],"--probe")){
  int pid=atoi(argv[2]),count=argc>=4?atoi(argv[3]):1,i;
  for(i=0;i<count;i++){ub_probe(pid);ub_sleep_ms(30);}
  return 0;
 }
 if(argc>=4&&!strcmp(argv[1],"--peek")){
  int pid=atoi(argv[2]);uint32_t a=(uint32_t)strtoul(argv[3],0,0),n=argc>=5?(uint32_t)strtoul(argv[4],0,0):4,i;
  unsigned char b[256];if(n>sizeof b)n=sizeof b;
  if(ub_mem_read(pid,a,b,n)){fprintf(stderr,"peek PID%d @%08x failed\n",pid,a);return 1;}
  printf("%08x:",a);for(i=0;i<n;i++)printf(" %02x",b[i]);printf("\n");return 0;
 }
 if(argc>=4&&!strcmp(argv[1],"--poke")){
  int pid=atoi(argv[2]);uint32_t a=(uint32_t)strtoul(argv[3],0,0),v=(uint32_t)strtoul(argv[4],0,0),n=argc>=6?(uint32_t)strtoul(argv[5],0,0):4;
  unsigned char b[4];uint32_t rb=0;
  if(n>4)n=4;
  memcpy(b,&v,4);
  if(ub_mem_write(pid,a,b,n)){fprintf(stderr,"poke PID%d @%08x <- %08x failed\n",pid,a,v);return 1;}
  if(ub_mem_read(pid,a,&rb,4)){fprintf(stderr,"readback failed\n");return 1;}
  printf("PID%d @%08x: wrote %08x, readback %08x %s\n",pid,a,v,rb,(rb&((1u<<(n*8))-1))==(v&((1u<<(n*8))-1))?"OK":"MISMATCH");
  return 0;
 }
 for(struct ub_module *const *mm=ub_modules;(m=*mm);mm++){int rc;if(m->command&&(rc=m->command(argc,argv))>=0)return rc;}
 lock=open(UB_LOCK,O_CREAT|O_RDWR,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 0;
 ub_log_open();
 ub_core_start();
 for(;;){ub_core_tick();ub_sleep_ms(1000);}
}
#endif
