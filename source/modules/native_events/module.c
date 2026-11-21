/* Shared native-loader support. Region policy is a client of this service;
 * stock region and a disabled region module still get correct EV waits. */
#define _GNU_SOURCE
#include "ub.h"
#include "assets.h"
#include "payloads.h"
#include "native_wait.h"
#include "../region/bd_launch.h"
#include <signal.h>
#include <sys/stat.h>

extern struct ub_module ub_module_native_events;
static int native_ready,native_policy,native_failed;
/* Every protected child leaves <pid>.ready in RAM (tmpfs, one page each).
 * Nothing else removes them, so a long uptime would keep growing the set.
 * A stale receipt never authorizes a stop (PID, birth and the live
 * descriptor are checked), so this is housekeeping only. A receipt is
 * removed when its process is gone or its PID belongs to a newer process;
 * the inode is compared again right before unlink, so a receipt that a
 * newer process just renamed into place is kept. */
static int native_stale(const char *path,unsigned long long pid,int temporary){
 char text[128],stat[4096],proc[48];unsigned long long live=0,owner,born;const char *c;
 snprintf(proc,sizeof proc,"/proc/%llu/stat",pid);
 if(ub_native_wait_read(proc,stat,sizeof stat)>=0)live=ub_native_wait_birth(stat);
 if(!live)return kill((pid_t)pid,0)&&errno==ESRCH; /* unreadable but alive: keep */
 if(temporary)return 0;                             /* still being published */
 if(ub_native_wait_read(path,text,sizeof text)<0||strncmp(text,"EVW2 ",5))return 0;
 c=text+5;
 if(ub_native_wait_number(&c,10,&owner)||*c++!=' '||ub_native_wait_number(&c,10,&born))return 0;
 return owner==pid&&born!=live;
}
unsigned ub_native_wait_sweep(void){
 struct ub_dir d;const char *name;unsigned removed=0;
 if(ub_dir_open(&d,UB_NATIVE_WAIT_DIR))return 0;
 while((name=ub_dir_next(&d))){
  char path[256];const char *c=name;unsigned long long pid;int temporary;struct ub_stat before,now;int fd;
  if(ub_native_wait_number(&c,10,&pid)||!pid||pid>INT_MAX)continue;
  if(!strcmp(c,".ready"))temporary=0;else if(!strcmp(c,".ready.tmp"))temporary=1;else continue;
  if(snprintf(path,sizeof path,UB_NATIVE_WAIT_DIR "/%s",name)>=(int)sizeof path)continue;
  fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)continue;
  if(ub_fstat(fd,&before)){close(fd);continue;}
  close(fd);
  if(!S_ISREG(before.mode)||!native_stale(path,pid,temporary))continue;
  if(ub_stat(path,&now)||now.ino!=before.ino||now.dev!=before.dev)continue;
  if(!unlink(path))removed++;
 }
 ub_dir_close(&d);
 return removed;
}
static void native_sweep(void){
 static unsigned wait;unsigned removed;
 if(wait){wait--;return;}
 wait=59; /* about once a minute */
 if((removed=ub_native_wait_sweep()))ub_log("native_events",UB_INFO,"removed %u stale EV wait receipt(s)",removed);
}
static void native_disabled(struct ub_module *m){
 static int reported;(void)m;native_ready=0;
 native_sweep(); /* receipts of a previous enabled run are still cleaned up */
 /* An already resident launcher must not keep using a previous helper's
  * policy when this service is disabled on a same-boot helper restart. */
 if(unlink(UB_BD_LAUNCH_CONFIG)&&errno!=ENOENT){
  if(!reported++)ub_log("native_events",UB_ALWAYS,"cannot remove disabled launch configuration: %s",strerror(errno));
 }else reported=0;
}
static int native_blob(const char *path,const void *data,size_t size){
 char temp[256];size_t used=0;ssize_t n;int fd,rc=-1;
 if(snprintf(temp,sizeof temp,"%s.%d.tmp",path,(int)getpid())>=(int)sizeof temp)return -1;
 fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);if(fd<0)return -1;
 while(used<size){n=write(fd,(const char *)data+used,size-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
 if(used==size&&!fchmod(fd,0600))rc=0;
 if(close(fd))rc=-1;
 if(!rc&&rename(temp,path))rc=-1;
 if(rc)unlink(temp);
 return rc;
}
int ub_native_bd_publish(int policy){
 struct ub_bd_launch_config c={UB_BD_LAUNCH_MAGIC,1,0,0};struct ub_stat st;char hash[65];
 native_ready=0;
 if(!ub_module_native_events.enabled){native_disabled(&ub_module_native_events);return -1;}
 if(!ub_bd_valid_policy(policy))goto fail;
 native_policy=policy;
 if((mkdir(UB_BD_RAM_DIR,0700)&&errno!=EEXIST)||ub_stat(UB_BD_RAM_DIR,&st)||!S_ISDIR(st.mode))goto fail;
 if((mkdir(UB_NATIVE_WAIT_DIR,0700)&&errno!=EEXIST)||ub_stat(UB_NATIVE_WAIT_DIR,&st)||!S_ISDIR(st.mode))goto fail;
 if(ub_hash_file(UB_BD_PRELOAD,hash)||strcmp(hash,ub_asset_native_events_bd_preload_sha256)){
  if(native_blob(UB_BD_PRELOAD,ub_asset_native_events_bd_preload,ub_asset_native_events_bd_preload_size)||
     ub_hash_file(UB_BD_PRELOAD,hash)||strcmp(hash,ub_asset_native_events_bd_preload_sha256))goto fail;
 }
 c.policy=policy;
 if(native_blob(UB_BD_LAUNCH_CONFIG,&c,sizeof c))goto fail;
 native_ready=1;
 if(native_failed){native_failed=0;ub_log("native_events",UB_ALWAYS,"native EV wait support and BD policy published again");}
 return 0;
 fail:
 unlink(UB_BD_LAUNCH_CONFIG);
 if(!native_failed++)ub_log("native_events",UB_ALWAYS,"native EV wait support unavailable; awaiting successful publication before future BD clock installs");
 return -1;
}
static void native_launch_step(struct ub_part *p,struct ub_target *t){
 if(t->state[0]||!native_ready)return;
 t->state[0]=1;
 ub_log("native_events",UB_ALWAYS,"native EV wait launcher armed in apl_medm PID%d; future BD/DVD children get pre-main protection; an already-in-flight boot-daemon wait is not covered",t->pid);
 (void)p;
}
static void native_retry(struct ub_part *p,struct ub_target *t){
 static unsigned retry;(void)p;(void)t;
 native_sweep();
 if(native_ready)return;
 if(!retry){ub_native_bd_publish(native_policy);retry=9;}else retry--;
}
static void native_medm_prepare(struct ub_part *p,struct ub_target *t){
 char path[600],hash[65];(void)p;
 /* Pin the entire API provider before installing an entry hook in its text.
  * Never run a hash/filesystem scan while the native threads are stopped. */
 if(!ub_map_base(t->pid,"/libev.so",path,sizeof path)){t->retry=1;return;}
 if(ub_hash_file(path,hash)||
    (strcmp(hash,"e5391f9728f1b6e3e30f61e6b1672eef9f43ee692694b8b6b513feaf23969c8d")&&
     strcmp(hash,"d48123aecb85995a14cb762b7e1ba6e43a6ae49f973ebd5b6307648f3584844d"))){
  t->failed=1;ub_log("native_events",UB_ALWAYS,"apl_medm PID%d has an unverified libev provider; launcher/wait hooks refused",t->pid);
 }
}
static struct ub_part native_parts[]={
 {"native BD launcher","apl_medm","c092d29f3a16df117ae36d9ac915dd2dad87d1ea1fb9ca29bfcc7bb48d9971a4",&ub_payload_native_events_launch,1,native_medm_prepare,native_launch_step,{0},0,0},
 {"native Settings launcher","apl_mgr","5ba307fa43e18ee0aa72576ff4e5a49b316f7f239794fb84579f638ec00ba6a9",&ub_payload_native_events_mgr_launch,1,0,0,{0},0,0},
 {"native assets",0,0,0,0,0,native_retry,{0},0,0}
};
struct ub_part *ub_native_bd_part(void){return ub_module_native_events.enabled?&native_parts[0]:0;}
static void native_start(struct ub_module *m){(void)m;ub_native_bd_publish(0);}
struct ub_module ub_module_native_events={
 "native_events","Scoped native waits and pre-main BD/DVD/Settings launch support",1,0,native_start,
 native_parts,sizeof native_parts/sizeof *native_parts,0,native_disabled
};
