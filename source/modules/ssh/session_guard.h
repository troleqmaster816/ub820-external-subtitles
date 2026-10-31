/* Runaway guard for ssh sessions ([ssh] guard=1, off by default). Included
 * by module.c after ub.h. It treats a symptom, not a cause, and is kept as a
 * diagnostic option (its session trace shows what a session ran).
 *
 * A session whose shell keeps starting the next one (/bin/sh is sash; a
 * chain of a few hundred can form in about two seconds) can take the whole
 * player down. While dropbear runs, a small process
 * watches everything below the dropbear master every 100 ms (20 ms for a
 * few seconds after a session starts a process). If a chain gets
 * GUARD_DEPTH deep or the sessions hold more than GUARD_COUNT processes, it
 * first stops (SIGSTOP) every one of them, so none is orphaned and escapes,
 * then kills them all. dropbear itself keeps listening. */
#ifndef UB_SSH_SESSION_GUARD_H
#define UB_SSH_SESSION_GUARD_H
#include <signal.h>

#define GUARD_POLL_MS 100   /* someone is connected */
#define GUARD_IDLE_MS 500   /* nobody is connected */
/* After a new process appears in a session, poll fast for a few seconds so
 * short commands are seen too; an idle open session costs only the slow poll
 * (polling every 20 ms all the time cost the helper about 9% CPU). */
#define GUARD_BUSY_MS 20
#define GUARD_BUSY_SECONDS 3
static unsigned long long guard_fast_until;
static int guard_sessions;
#define GUARD_DEPTH 16
#define GUARD_COUNT 64
#define GUARD_PROCS 2048

struct tree_proc { int pid,ppid; char comm[17]; };
static unsigned tree_read(struct tree_proc *t,unsigned cap){
 static struct ub_dir d;const char *e;unsigned n=0;
 if(ub_dir_open(&d,"/proc"))return 0;
 while(n<cap&&(e=ub_dir_next(&d))){
  char path[48],buf[512],*lp,*rp;int pid=atoi(e),fd;ssize_t k;size_t len;
  if(pid<=0)continue;
  snprintf(path,sizeof path,"/proc/%d/stat",pid);
  if((fd=open(path,O_RDONLY))<0)continue;
  k=read(fd,buf,sizeof buf-1);close(fd);
  if(k<=0)continue;
  buf[k]=0;lp=strchr(buf,'(');rp=strrchr(buf,')');
  if(!lp||!rp||rp<lp||!rp[1]||!rp[2])continue;
  len=(size_t)(rp-lp-1);if(len>16)len=16;
  memset(t[n].comm,0,sizeof t[n].comm);memcpy(t[n].comm,lp+1,len);
  t[n].pid=pid;t[n].ppid=atoi(rp+4); /* ") S ppid" */
  n++;
 }
 ub_dir_close(&d);return n;
}
/* Depth of pid below root (1 = child), or 0 if root is not an ancestor. */
static int tree_depth(const struct tree_proc *t,unsigned n,int pid,int root){
 for(int depth=1;depth<256;depth++){
  unsigned i;
  for(i=0;i<n&&t[i].pid!=pid;i++){}
  if(i==n||t[i].ppid<=1)return 0;
  if(t[i].ppid==root)return depth;
  pid=t[i].ppid;
 }
 return 0;
}
/* Processes below master: how many, and the deepest chain. */
static unsigned tree_below(const struct tree_proc *t,unsigned n,int master,int *deepest,const char **deep_comm){
 unsigned count=0;
 *deepest=0;*deep_comm="";
 for(unsigned i=0;i<n;i++){
  int d=tree_depth(t,n,t[i].pid,master);
  if(!d)continue;
  count++;
  if(d>*deepest){*deepest=d;*deep_comm=t[i].comm;}
 }
 return count;
}
/* What a stopped process was doing, for finding the cause of a runaway:
 * program, command line, state, kernel wait, current syscall and signal
 * masks (signal state is inherited through fork and exec). */
static void describe(int pid,char *exe,size_t exe_size,char *cmd,size_t cmd_size){
 char path[48];ssize_t n;
 snprintf(path,sizeof path,"/proc/%d/exe",pid);n=readlink(path,exe,exe_size-1);exe[n>0?n:0]=0;
 snprintf(path,sizeof path,"/proc/%d/cmdline",pid);n=ub_read_file(path,cmd,cmd_size);
 for(ssize_t i=0;i<n-1;i++)if(!cmd[i])cmd[i]=' ';
 if(n<0)cmd[0]=0;
}
static void evidence(int pid){
 char path[48],exe[96],cmd[160],stat[512],wchan[48],sys[96],status[2048],*p,*save;char masks[160]="";size_t used=0;
 describe(pid,exe,sizeof exe,cmd,sizeof cmd);
 snprintf(path,sizeof path,"/proc/%d/stat",pid);if(ub_read_file(path,stat,sizeof stat)<0)stat[0]=0;
 snprintf(path,sizeof path,"/proc/%d/wchan",pid);if(ub_read_file(path,wchan,sizeof wchan)<0)wchan[0]=0;
 snprintf(path,sizeof path,"/proc/%d/syscall",pid);if(ub_read_file(path,sys,sizeof sys)<0)sys[0]=0;
 sys[strcspn(sys,"\n")]=0;
 snprintf(path,sizeof path,"/proc/%d/status",pid);
 if(ub_read_file(path,status,sizeof status)>=0)
  for(p=strtok_r(status,"\n",&save);p;p=strtok_r(0,"\n",&save))
   if(!strncmp(p,"SigBlk:",7)||!strncmp(p,"SigIgn:",7)||!strncmp(p,"SigCgt:",7)||!strncmp(p,"PPid:",5))
    used+=(size_t)snprintf(masks+used,used<sizeof masks?sizeof masks-used:0,"%s%s",used?" ":"",p);
 for(p=masks;*p;p++)if(*p=='\t')*p=' ';
 p=strrchr(stat,')');
 ub_log(ID,UB_ALWAYS,"  PID%d exe '%s' cmdline '%s' state %c wchan '%s' syscall '%s' %s",
  pid,exe,cmd,p&&p[1]&&p[2]?p[2]:'?',wchan,sys,masks);
}
/* Stop everything below master until nothing new appears, then kill it all;
 * record: log what they were doing first. Returns how many were ended. */
static unsigned tree_end(int master,int record){
 static struct tree_proc t[GUARD_PROCS];static int victims[GUARD_PROCS];
 unsigned n,count=0,round;
 for(round=0;round<50;round++){
  unsigned added=0;
  n=tree_read(t,GUARD_PROCS);
  for(unsigned i=0;i<n;i++){
   unsigned j;
   if(!tree_depth(t,n,t[i].pid,master))continue;
   for(j=0;j<count&&victims[j]!=t[i].pid;j++){}
   if(j<count||count==GUARD_PROCS)continue;
   victims[count++]=t[i].pid;kill(t[i].pid,SIGSTOP);added++;
  }
  if(!added)break;
 }
 /* while they are stopped: the top of the chain and its newest end */
 if(record)for(unsigned i=0;i<count;i++)if(i<4||i+2>=count)evidence(victims[i]);
 for(unsigned i=0;i<count;i++)kill(victims[i],SIGKILL);
 return count;
}
/* Session trace: every process that appears below the dropbear master is
 * logged once, at once (program, command line, parent), and the log is
 * saved to the stick straight away, so the steps before a freeze survive
 * it. At most GUARD_TRACE_LINES per GUARD_TRACE_WINDOW polls. */
#define GUARD_TRACE_LINES 40
#define GUARD_TRACE_WINDOW 100
/* Returns how many processes are new. */
static unsigned guard_trace(const struct tree_proc *t,unsigned n,int master){
 static int seen[512];static unsigned next,lines,window,muted;
 int logged=0;unsigned fresh=0;
 if(++window>=GUARD_TRACE_WINDOW){
  if(muted)ub_log(ID,UB_ALWAYS,"session trace: %u more new processes not listed",muted);
  window=0;lines=0;muted=0;
 }
 for(unsigned i=0;i<n;i++){
  char exe[96],cmd[160];unsigned j;
  if(!tree_depth(t,n,t[i].pid,master))continue;
  for(j=0;j<512&&seen[j]!=t[i].pid;j++){}
  if(j<512)continue;
  seen[next++%512]=t[i].pid;fresh++;
  if(lines>=GUARD_TRACE_LINES){muted++;continue;}
  lines++;logged=1;
  describe(t[i].pid,exe,sizeof exe,cmd,sizeof cmd);
  ub_log(ID,UB_INFO,"session trace: PID%d '%s' parent PID%d, depth %d, exe '%s' cmdline '%s'",t[i].pid,t[i].comm,t[i].ppid,tree_depth(t,n,t[i].pid,master),exe,cmd);
 }
 if(logged){int fd=open(UB_LOG_URGENT,O_WRONLY|O_CREAT,0600);if(fd>=0)close(fd);}
 return fresh;
}
/* 1 when the sessions below master ran away (and were ended). */
static int guard_check(int master){
 static struct tree_proc t[GUARD_PROCS];
 unsigned n=tree_read(t,GUARD_PROCS),count;int deepest;const char *comm;char what[40];
 if(guard_trace(t,n,master))guard_fast_until=ub_now_ms()+GUARD_BUSY_SECONDS*1000ULL;
 count=tree_below(t,n,master,&deepest,&comm);
 guard_sessions=count>0;
 if(deepest<GUARD_DEPTH&&count<=GUARD_COUNT)return 0;
 snprintf(what,sizeof what,"%.16s",comm);
 ub_log(ID,UB_ALWAYS,"an ssh session ran away (a chain %d deep of '%s', %u processes); stopping all ssh sessions to protect the player",deepest,what,count);
 count=tree_end(master,1);
 ub_log(ID,UB_ALWAYS,"ended %u processes of ssh sessions; the ssh server keeps running",count);
 {int fd=open(UB_LOG_URGENT,O_WRONLY|O_CREAT,0600);if(fd>=0)close(fd);}
 return 1;
}
static void guard_loop(int master){
 while(!kill(master,0)){
  guard_check(master);
  ub_sleep_ms(ub_now_ms()<guard_fast_until?GUARD_BUSY_MS:guard_sessions?GUARD_POLL_MS:GUARD_IDLE_MS);
 }
 _exit(0);
}
#endif
