/* Detailed diagnostics (diagnostics=1): what the player does, not only what
 * the modules do. Included by module.c after crash.h. Everything here only
 * reads /proc and the kernel log, except two RAM-only kernel settings made
 * for crash reports: core_pattern (crash.h) and each process's
 * coredump_filter. Once per second:
 *   - new kernel messages (driver errors, OOM killer, oopses);
 *   - player processes starting and ending, with how they ended (exit code or
 *     signal, from the kernel's process events where available);
 *   - a thread using a whole CPU for 10 s (a busy loop: where it runs), or
 *     stuck 5 s in an uninterruptible kernel wait (a hung driver call: where
 *     in the kernel);
 *   - low memory.
 * Every 60 s one line with load, memory and the busiest processes, so a
 * continuously saved log shows the state just before a freeze. */
#ifndef UB_LOG_DIAG_H
#define UB_LOG_DIAG_H
#include <sys/klog.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>

#define DIAG_ID "diag"
#define KERNEL_ID "kernel"
#ifndef DIAG_BUSY_SECONDS
#define DIAG_BUSY_SECONDS 10
#endif
#define DIAG_BLOCKED_SECONDS 5
#define DIAG_SNAPSHOT_SECONDS 60
#define DIAG_LOW_MEMORY_KB (8*1024)
#define DIAG_KERNEL_LINES 30

/* ---- exit statuses ---------------------------------------------------- */
static void exit_text(int status,char *out,size_t size){
 if(status&0x7f)snprintf(out,size,"killed by %s%s",signal_name(status&0x7f),status&0x80?" (core dumped)":"");
 else snprintf(out,size,"exit code %d",(status>>8)&0xff);
}
/* Process exit events (proc connector), kept until the scan notices the
 * process is gone. */
#define DIAG_EXITS 64
static struct { int pid,status;unsigned age; } exits[DIAG_EXITS];
static unsigned exit_next;
static void exit_seen(int pid,int status){exits[exit_next%DIAG_EXITS].pid=pid;exits[exit_next%DIAG_EXITS].status=status;exits[exit_next%DIAG_EXITS].age=0;exit_next++;}
static int exit_take(int pid,int *status){
 for(unsigned i=0;i<DIAG_EXITS;i++)if(exits[i].pid==pid){*status=exits[i].status;exits[i].pid=0;return 1;}
 return 0;
}
/* One netlink datagram; returns how many exits it recorded. */
static int netlink_parse(const unsigned char *buf,size_t n){
 int found=0;size_t off=0;
 while(off+sizeof(struct nlmsghdr)<=n){
  const struct nlmsghdr *h=(const void *)(buf+off);const struct cn_msg *cn=NLMSG_DATA(h);struct proc_event ev;size_t data;
  if(h->nlmsg_len<sizeof *h||h->nlmsg_len>n-off)break;
  off+=NLMSG_ALIGN(h->nlmsg_len);
  if(h->nlmsg_len<NLMSG_LENGTH(sizeof *cn+offsetof(struct proc_event,event_data)+16))continue;
  if(cn->id.idx!=CN_IDX_PROC||cn->id.val!=CN_VAL_PROC)continue;
  data=h->nlmsg_len-NLMSG_LENGTH(sizeof *cn);
  memset(&ev,0,sizeof ev);memcpy(&ev,cn->data,data<sizeof ev?data:sizeof ev);
  if(ev.what==PROC_EVENT_EXIT&&ev.event_data.exit.process_pid==ev.event_data.exit.process_tgid){exit_seen(ev.event_data.exit.process_pid,(int)ev.event_data.exit.exit_code);found++;}
 }
 return found;
}
static int events=-1;
static void events_open(void){
 struct sockaddr_nl addr;
 struct { struct nlmsghdr h;struct cn_msg cn;uint32_t op; } __attribute__((packed)) msg;
 events=socket(PF_NETLINK,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,NETLINK_CONNECTOR);
 if(events<0)return;
 memset(&addr,0,sizeof addr);addr.nl_family=AF_NETLINK;addr.nl_groups=CN_IDX_PROC;
 memset(&msg,0,sizeof msg);
 msg.h.nlmsg_len=sizeof msg;msg.h.nlmsg_type=NLMSG_DONE;msg.h.nlmsg_pid=(uint32_t)getpid();
 msg.cn.id.idx=CN_IDX_PROC;msg.cn.id.val=CN_VAL_PROC;msg.cn.len=sizeof msg.op;msg.op=PROC_CN_MCAST_LISTEN;
 if(bind(events,(struct sockaddr *)&addr,sizeof addr)||send(events,&msg,sizeof msg,0)!=(ssize_t)sizeof msg){close(events);events=-1;}
}
static void events_read(void){
 unsigned char buf[4096] __attribute__((aligned(8)));struct sockaddr_nl from;socklen_t len;ssize_t n;
 if(events<0)return;
 for(unsigned i=0;i<512;i++){
  len=sizeof from;n=recvfrom(events,buf,sizeof buf,0,(struct sockaddr *)&from,&len);
  if(n<0){if(errno==ENOBUFS)ub_log(DIAG_ID,UB_DEBUG,"some process events were lost");if(errno!=EINTR&&errno!=ENOBUFS)break;continue;}
  if(from.nl_pid)continue; /* only the kernel */
  netlink_parse(buf,(size_t)n);
 }
}

/* ---- /proc scan ------------------------------------------------------- */
struct diag_stat { char comm[17],state;int ppid;unsigned long ticks,vsize;unsigned long long born;uint32_t pc; };
/* Fields of /proc/<pid>/stat after the "(comm)", which may hold anything. */
static int stat_parse(const char *buf,struct diag_stat *s){
 const char *open=strchr(buf,'('),*close=strrchr(buf,')'),*p;unsigned field=3;size_t len;
 memset(s,0,sizeof *s);
 if(!open||!close||close<open)return -1;
 len=(size_t)(close-open-1);if(len>16)len=16;memcpy(s->comm,open+1,len);
 for(p=close+2;*p;field++){
  char *end;unsigned long long v=strtoull(p,&end,10);
  if(field==3)s->state=*p;
  else if(field==4)s->ppid=(int)v;
  else if(field==14||field==15)s->ticks+=(unsigned long)v;
  else if(field==22)s->born=v;
  else if(field==23)s->vsize=(unsigned long)v;
  else if(field==30){s->pc=(uint32_t)v;break;}
  p=strchr(p,' ');if(!p)break;
  p++;
 }
 return field>=23?0:-1;
}
static int stat_read(const char *path,struct diag_stat *s){
 char buf[1024];int fd=open(path,O_RDONLY);ssize_t n;
 if(fd<0)return -1;
 n=read(fd,buf,sizeof buf-1);close(fd);
 if(n<=0)return -1;
 buf[n]=0;return stat_parse(buf,s);
}
static void small_file(const char *path,char *out,size_t size){
 int fd=open(path,O_RDONLY);ssize_t n=fd>=0?read(fd,out,size-1):-1;
 if(fd>=0)close(fd);
 out[n>0?n:0]=0;
 for(char *p=out;*p;p++)if(*p=='\n')*p=*(p+1)?' ':0;
}

/* Two generations of hash tables: what the last scan saw, and this one. */
#define DIAG_PROCS 512
#define DIAG_THREADS 2048
struct diag_proc { int pid,ppid;unsigned long long born;unsigned long ticks,snap;unsigned age;int announced,ours,filtered;char comm[17]; };
struct diag_thread { int tid,pid;unsigned long long born;unsigned long ticks;unsigned busy,blocked;int busy_said,blocked_said;uint32_t pcs[3];char comm[17]; };
static struct diag_proc procs_a[DIAG_PROCS],procs_b[DIAG_PROCS],*procs=procs_a,*procs_new=procs_b;
static struct diag_thread threads_a[DIAG_THREADS],threads_b[DIAG_THREADS],*threads=threads_a,*threads_new=threads_b;
static unsigned thread_count;
#define SLOT(table,size,key,field) for(i=(unsigned)(key)%(size);table[i].field&&table[i].field!=(key);i=(i+1)%(size))
static struct diag_proc *proc_find(struct diag_proc *t,int pid){unsigned i;SLOT(t,DIAG_PROCS,pid,pid);return t[i].pid?&t[i]:0;}
static struct diag_proc *proc_add(struct diag_proc *t,int pid){unsigned i;SLOT(t,DIAG_PROCS,pid,pid);return &t[i];}
static struct diag_thread *thread_find(struct diag_thread *t,int tid){unsigned i;SLOT(t,DIAG_THREADS,tid,tid);return t[i].tid?&t[i]:0;}
static struct diag_thread *thread_add(struct diag_thread *t,int tid){unsigned i;SLOT(t,DIAG_THREADS,tid,tid);return &t[i];}

static int diag_scans,crash_active;
static long hz;
static unsigned long long diag_ms;

static const char *proc_name(int pid){const struct diag_proc *p=proc_find(procs_new,pid);if(!p)p=proc_find(procs,pid);return p?p->comm:"?";}

static void thread_report(const struct diag_thread *t,const char *what){
 static struct crash_map maps[CRASH_MAPS];
 char path[80],wchan[64],stack[200],at[3][80],one[80];unsigned n,k=0;
 if(!strcmp(what,"busy")){
  n=maps_read(t->pid,maps,CRASH_MAPS);
  for(unsigned i=0;i<3;i++){
   if(!t->pcs[i])continue;
   where(maps,n,t->pcs[i],one,sizeof one);snprintf(at[k++],sizeof at[0],"%s",one);
  }
  ub_log(DIAG_ID,UB_ALWAYS,"%s PID%d thread '%s' TID%d has used a whole CPU for %u s; running at %s%s%s%s%s",proc_name(t->pid),t->pid,t->comm,t->tid,t->busy,
   k?at[0]:"(position not readable)",k>1?", ":"",k>1?at[1]:"",k>2?", ":"",k>2?at[2]:"");
  return;
 }
 snprintf(path,sizeof path,"/proc/%d/task/%d/wchan",t->pid,t->tid);small_file(path,wchan,sizeof wchan);
 snprintf(path,sizeof path,"/proc/%d/task/%d/stack",t->pid,t->tid);small_file(path,stack,sizeof stack);
 ub_log(DIAG_ID,UB_ALWAYS,"%s PID%d thread '%s' TID%d %s %u s in an uninterruptible kernel wait (%s)%s%s",proc_name(t->pid),t->pid,t->comm,t->tid,what,t->blocked,
  wchan[0]&&strcmp(wchan,"0")?wchan:"wait point unknown",stack[0]?"; kernel stack: ":"",stack);
}

static int urgent;
static void diag_threads(int pid,unsigned long long elapsed_ms){
 static struct ub_dir d;char path[64];const char *e;
 snprintf(path,sizeof path,"/proc/%d/task",pid);
 if(ub_dir_open(&d,path))return;
 while((e=ub_dir_next(&d))){
  struct diag_stat s;struct diag_thread *old,*t;int tid=atoi(e);char p[80];
  if(tid<=0||thread_count>=DIAG_THREADS*3/4)continue;
  snprintf(p,sizeof p,"/proc/%d/task/%d/stat",pid,tid);
  if(stat_read(p,&s))continue;
  old=thread_find(threads,tid);t=thread_add(threads_new,tid);thread_count++;
  if(old&&old->born==s.born)*t=*old;
  else{memset(t,0,sizeof *t);t->tid=tid;t->pid=pid;t->born=s.born;t->ticks=s.ticks;}
  memcpy(t->comm,s.comm,sizeof t->comm);
  /* busy: at least 90% of one CPU since the last scan */
  if(elapsed_ms&&(s.ticks-t->ticks)*100000ULL>=90ULL*(unsigned long long)hz*elapsed_ms){
   t->pcs[t->busy%3]=s.pc;t->busy++;
   if(t->busy==DIAG_BUSY_SECONDS){thread_report(t,"busy");t->busy_said=1;urgent=1;}
  }else{
   if(t->busy_said)ub_log(DIAG_ID,UB_ALWAYS,"%s PID%d thread '%s' TID%d no longer busy after %u s",proc_name(pid),pid,t->comm,tid,t->busy);
   t->busy=0;t->busy_said=0;
  }
  t->ticks=s.ticks;
  if(s.state=='D'){
   t->blocked++;
   if(t->blocked==DIAG_BLOCKED_SECONDS){thread_report(t,"has been");t->blocked_said=1;urgent=1;}
   else if(t->blocked==60)thread_report(t,"still after");
  }else{
   if(t->blocked_said)ub_log(DIAG_ID,UB_ALWAYS,"%s PID%d thread '%s' TID%d continued after %u s in an uninterruptible wait",proc_name(pid),pid,t->comm,tid,t->blocked);
   t->blocked=0;t->blocked_said=0;
  }
 }
 ub_dir_close(&d);
}

/* Starts and ends are listed one by one up to DIAG_LIST_LINES per second;
 * beyond that one summary line says how many and of what, so a burst (a
 * runaway chain of processes, say) stays readable and does not fill the log. */
#define DIAG_LIST_LINES 8
#define DIAG_CHAIN_DEPTH 12
struct burst { unsigned shown,more,kinds;char comm[6][17];unsigned count[6]; };
static int burst_line(struct burst *b,const char *comm){
 unsigned i;
 if(b->shown<DIAG_LIST_LINES){b->shown++;return 1;}
 b->more++;
 for(i=0;i<b->kinds&&strcmp(b->comm[i],comm);i++){}
 if(i==b->kinds&&b->kinds<6){memcpy(b->comm[i],comm,17);b->count[i]=0;b->kinds++;}
 if(i<b->kinds)b->count[i]++;
 return 0;
}
static void burst_summary(const struct burst *b,const char *what){
 char text[200];size_t used=0;
 if(!b->more)return;
 for(unsigned i=0;i<b->kinds&&used<sizeof text;i++)used+=(size_t)snprintf(text+used,sizeof text-used,"%s%s x%u",i?", ":"",b->comm[i],b->count[i]);
 ub_log(DIAG_ID,UB_ALWAYS,"... and %u more processes %s in the last second (%s)",b->more,what,text);
 urgent=1;
}
static void process_ended(const struct diag_proc *p,struct burst *b){
 char how[64];int status=0,known=exit_take(p->pid,&status),signal=known&&(status&0x7f);
 if(known)exit_text(status,how,sizeof how);
 if(p->ours)return;
 if((p->announced>0||signal)&&burst_line(b,p->comm))ub_log(DIAG_ID,signal?UB_ALWAYS:UB_INFO,"%s PID%d ended after %u s%s%s",p->comm,p->pid,p->age,known?": ":"",known?how:"");
 if(signal)urgent=1;
}
/* A process whose ancestors are DIAG_CHAIN_DEPTH or more processes of the
 * same kind is reported (a program starting itself over and over). */
static void chain_check(void){
 static unsigned quiet;const struct diag_proc *deep=0,*top=0;int deepest=0;
 if(quiet){quiet--;return;}
 for(unsigned i=0;i<DIAG_PROCS;i++){
  const struct diag_proc *p=&procs_new[i],*q=p,*up;int depth=0;
  if(!p->pid||p->ours)continue;
  while(depth<512&&(up=proc_find(procs_new,q->ppid))&&!strcmp(up->comm,p->comm)){q=up;depth++;}
  if(depth>deepest){deepest=depth;deep=p;top=q;}
 }
 if(deepest<DIAG_CHAIN_DEPTH)return;
 ub_log(DIAG_ID,UB_ALWAYS,"runaway: a chain of %d '%s' processes, each started by the one before (newest PID%d, first PID%d started by %s PID%d)",
  deepest+1,deep->comm,deep->pid,top->pid,proc_name(top->ppid),top->ppid);
 urgent=1;quiet=10;
}

static void diag_scan(unsigned long long elapsed_ms){
 static struct ub_dir d;const char *e;int self=getpid();struct burst started,ended;
 if(ub_dir_open(&d,"/proc"))return;
 memset(&started,0,sizeof started);memset(&ended,0,sizeof ended);
 memset(procs_new,0,sizeof procs_a);memset(threads_new,0,sizeof threads_a);thread_count=0;
 while((e=ub_dir_next(&d))){
  struct diag_stat s;struct diag_proc *old,*p;int pid=atoi(e);char path[64];
  if(pid<=0)continue;
  snprintf(path,sizeof path,"/proc/%d/stat",pid);
  if(stat_read(path,&s)||!s.vsize)continue; /* gone, or a kernel thread */
  old=proc_find(procs,pid);p=proc_add(procs_new,pid);
  if(old&&old->born==s.born)*p=*old;
  else{
   memset(p,0,sizeof *p);p->pid=pid;p->born=s.born;p->snap=s.ticks;
   p->announced=!diag_scans; /* already running when diagnostics started */
   p->ours=pid==self||s.ppid==self;
  }
  p->ppid=s.ppid;p->ticks=s.ticks;p->age++;memcpy(p->comm,s.comm,sizeof p->comm);
  if(!p->announced&&p->age>=2&&!p->ours){
   p->announced=1;
   if(burst_line(&started,p->comm))ub_log(DIAG_ID,UB_INFO,"%s PID%d started (parent %s PID%d)",p->comm,pid,proc_name(p->ppid),p->ppid);
  }
  if(crash_active&&!p->filtered){
   int fd;p->filtered=1;
   snprintf(path,sizeof path,"/proc/%d/coredump_filter",pid);
   if((fd=open(path,O_WRONLY))>=0){if(write(fd,"0",1)<0){}close(fd);}
  }
  if(!p->ours)diag_threads(pid,elapsed_ms);
 }
 ub_dir_close(&d);
 burst_summary(&started,"started");
 chain_check();
 for(unsigned i=0;i<DIAG_PROCS;i++){
  struct diag_proc *p=&procs[i],*now;
  if(!p->pid)continue;
  now=proc_find(procs_new,p->pid);
  if(!now||now->born!=p->born)process_ended(p,&ended);
 }
 /* Exits of processes too short-lived for a scan: only deaths by signal. */
 for(unsigned i=0;i<DIAG_EXITS;i++){
  char how[64];
  if(!exits[i].pid||++exits[i].age<3)continue;
  if(exits[i].status&0x7f){exit_text(exits[i].status,how,sizeof how);if(burst_line(&ended,"?"))ub_log(DIAG_ID,UB_ALWAYS,"short-lived PID%d ended: %s",exits[i].pid,how);urgent=1;}
  exits[i].pid=0;
 }
 burst_summary(&ended,"ended");
 {struct diag_proc *t=procs;procs=procs_new;procs_new=t;}
 {struct diag_thread *t=threads;threads=threads_new;threads_new=t;}
 diag_scans++;
}

/* ---- kernel log ------------------------------------------------------- */
static char *klog;
static int klog_size;
static char klog_tail[256];
static size_t klog_tail_len;
static int kernel_level(const char **line){
 const char *p=*line;int level=-1;
 if(p[0]=='<'){char *end;long v=strtol(p+1,&end,10);if(*end=='>'){level=(int)(v&7);*line=end+1;}}
 return level;
}
static int kernel_urgent(const char *line,int level){
 static const char *const words[]={"Oops","BUG","Out of memory","Killed process","oom","segfault","Unable to handle","panic","hung_task","blocked for more than",0};
 if(level>=0&&level<=3)return 1;
 for(unsigned i=0;words[i];i++)if(strstr(line,words[i]))return 1;
 return 0;
}
/* Logs the lines after the previous read's tail; returns how many. */
static unsigned klog_lines(const char *buf,size_t n,int first){
 const char *start=buf,*end=buf+n,*hit=0;unsigned shown=0,skipped=0;
 if(klog_tail_len){
  for(const char *p=buf;(p=memmem(p,(size_t)(end-p),klog_tail,klog_tail_len));p++)hit=p;
  if(hit)start=hit+klog_tail_len;
  else ub_log(KERNEL_ID,UB_INFO,"kernel log moved on faster than it was read; newest lines follow");
 }
 while(start<end){
  const char *nl=memchr(start,'\n',(size_t)(end-start)),*text=start;char line[300];size_t len;int level;
  if(!nl)break; /* an unfinished line: next time */
  len=(size_t)(nl-start);if(len>=sizeof line)len=sizeof line-1;
  memcpy(line,start,len);line[len]=0;text=line;start=nl+1;
  level=kernel_level(&text);
  if(!*text||(first&&level>4)||level==7)continue;
  if(kernel_urgent(text,level))urgent=1;
  if(shown<DIAG_KERNEL_LINES){ub_log(KERNEL_ID,UB_ALWAYS,"%s",text);shown++;}else skipped++;
 }
 if(skipped)ub_log(KERNEL_ID,UB_ALWAYS,"%u more kernel lines not shown",skipped);
 /* remember the end of the last complete line, so an unfinished one is read
  * whole next time */
 while(n&&buf[n-1]!='\n')n--;
 if(n){klog_tail_len=n<sizeof klog_tail?n:sizeof klog_tail;memcpy(klog_tail,buf+n-klog_tail_len,klog_tail_len);}
 return shown;
}
static void kernel_read(int first){
 int n;
 if(!klog){
  klog_size=klogctl(10,0,0);
  if(klog_size<=0||klog_size>4*1024*1024||!(klog=malloc((size_t)klog_size))){klog_size=0;return;}
 }
 n=klogctl(3,klog,klog_size);
 if(n>0)klog_lines(klog,(size_t)n,first);
}

/* ---- memory and load -------------------------------------------------- */
static long meminfo(const char *text,const char *key){
 const char *p=strstr(text,key);return p?strtol(p+strlen(key),0,10):-1;
}
static int memory_read(long *total,long *free_kb,long *cache){
 char text[2048];small_file("/proc/meminfo",text,sizeof text);
 *total=meminfo(text,"MemTotal:");*free_kb=meminfo(text,"MemFree:");
 *cache=meminfo(text,"Buffers:")+meminfo(text,"Cached:");
 return *total>0&&*free_kb>=0?0:-1;
}
static void memory_check(void){
 static int low;long total,free_kb,cache;
 if(memory_read(&total,&free_kb,&cache))return;
 if(!low&&free_kb+cache<DIAG_LOW_MEMORY_KB){low=1;urgent=1;ub_log(DIAG_ID,UB_ALWAYS,"low memory: %ld KB free, %ld KB cache, of %ld KB",free_kb,cache,total);}
 else if(low&&free_kb+cache>2*DIAG_LOW_MEMORY_KB){low=0;ub_log(DIAG_ID,UB_ALWAYS,"memory recovered: %ld KB free, %ld KB cache",free_kb,cache);}
}
static void snapshot(unsigned long long elapsed_ms){
 struct diag_proc *top[3]={0,0,0};char load[64],text[200];long total,free_kb,cache;size_t used;unsigned processes=0;
 small_file("/proc/loadavg",load,sizeof load);
 for(unsigned i=0;i<DIAG_PROCS;i++){
  struct diag_proc *p=&procs[i];
  if(!p->pid)continue;
  processes++;
  for(unsigned k=0;k<3;k++)if(!top[k]||p->ticks-p->snap>top[k]->ticks-top[k]->snap){
   for(unsigned j=2;j>k;j--)top[j]=top[j-1];
   top[k]=p;break;
  }
 }
 if(memory_read(&total,&free_kb,&cache))total=free_kb=cache=0;
 used=(size_t)snprintf(text,sizeof text,"load %.14s, %ld KB free + %ld KB cache, %u processes, %u threads; CPU in the last %llu s:",
  load,free_kb,cache,processes,thread_count,elapsed_ms/1000);
 for(unsigned k=0;k<3&&top[k]&&used<sizeof text;k++)
  used+=(size_t)snprintf(text+used,sizeof text-used," %s %llu%%",top[k]->comm,elapsed_ms?(top[k]->ticks-top[k]->snap)*100000ULL/((unsigned long long)hz*elapsed_ms):0);
 ub_log(DIAG_ID,UB_INFO,"%s",text);
 for(unsigned i=0;i<DIAG_PROCS;i++)procs[i].snap=procs[i].ticks;
}

/* ---- start and step --------------------------------------------------- */
/* The kernel starts the crash reporter by path; when the helper's own file is
 * gone (a loader may delete it after starting it), a copy in /tmp is used. */
#define CRASH_COPY "/tmp/.ub820_crash"
static int self_copy(void){
 char buf[16384];int in=open("/proc/self/exe",O_RDONLY),out;ssize_t n=0;
 if(in<0)return -1;
 unlink(CRASH_COPY);out=open(CRASH_COPY,O_WRONLY|O_CREAT|O_TRUNC,0700);
 if(out<0){close(in);return -1;}
 while((n=read(in,buf,sizeof buf))>0)if(write(out,buf,(size_t)n)!=n){n=-1;break;}
 close(in);
 if(close(out)||n<0){unlink(CRASH_COPY);return -1;}
 return 0;
}
static void crash_setup(void){
 char exe[160],old[160],pattern[160];int fd;ssize_t n;
#ifdef UB_HOST_TEST
 if(1){ub_log(DIAG_ID,UB_ALWAYS,"crash reports off: host test");return;} /* never change the host's core_pattern */
#endif
 n=readlink("/proc/self/exe",exe,sizeof exe-1);
 if(n<=0||n>=(ssize_t)sizeof exe-1)exe[0]=0;
 else exe[n]=0;
 if(!exe[0]||strstr(exe," (deleted)")||access(exe,X_OK)){
  if(self_copy()){ub_log(DIAG_ID,UB_ALWAYS,"crash reports off: the helper cannot be copied to " CRASH_COPY);return;}
  snprintf(exe,sizeof exe,"%s",CRASH_COPY);
 }
 small_file("/proc/sys/kernel/core_pattern",old,sizeof old);
 if(snprintf(pattern,sizeof pattern,"|%s --core %%p %%s %%e",exe)>=128){ub_log(DIAG_ID,UB_ALWAYS,"crash reports off: path too long");return;}
 if(strcmp(old,"core")&&strcmp(old,pattern)&&old[0]){ub_log(DIAG_ID,UB_ALWAYS,"crash reports off: the kernel's core_pattern is already '%s'",old);return;}
 if((fd=open("/proc/sys/kernel/core_pattern",O_WRONLY))<0){ub_log(DIAG_ID,UB_ALWAYS,"crash reports off: core_pattern not writable (errno %d)",errno);return;}
 n=write(fd,pattern,strlen(pattern));close(fd);
 small_file("/proc/sys/kernel/core_pattern",old,sizeof old);
 if(n<0||strcmp(old,pattern)){ub_log(DIAG_ID,UB_ALWAYS,"crash reports off: core_pattern was not accepted");return;}
 crash_active=1;
}
static unsigned long long now_ms(void){return ub_now_ms();}
static void diag_start(void){
 struct utsname u;long total,free_kb,cache;
 hz=sysconf(_SC_CLK_TCK);if(hz<=0)hz=100;
 if(!uname(&u))ub_log(DIAG_ID,UB_ALWAYS,"detailed diagnostics on: kernel %s %s, %ld CPUs",u.release,u.version,sysconf(_SC_NPROCESSORS_ONLN));
 if(!memory_read(&total,&free_kb,&cache))ub_log(DIAG_ID,UB_ALWAYS,"memory: %ld KB, %ld KB free, %ld KB cache",total,free_kb,cache);
 crash_setup();
 events_open();
 ub_log(DIAG_ID,UB_ALWAYS,"crash reports %s; process exit codes %s",crash_active?"on":"off",events>=0?"on":"not available (process events off in this kernel)");
 kernel_read(1);
 diag_ms=now_ms();diag_scan(0);
}
static void diag_tick(void){
 static unsigned long long snap_ms;unsigned long long now=now_ms(),elapsed=now-diag_ms;
 if(!snap_ms)snap_ms=now;
 diag_ms=now;
 events_read();
 kernel_read(0);
 diag_scan(elapsed);
 memory_check();
 if(now-snap_ms>=DIAG_SNAPSHOT_SECONDS*1000ULL){snapshot(now-snap_ms);snap_ms=now;}
}
#endif
