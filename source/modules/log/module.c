/* Logging: how much goes into the RAM log, saving it to the stick, detailed
 * diagnostics of the player itself, and a record of mounts.
 *
 * Errors and essential events are always kept in the RAM log, even with this
 * module off. With it on, informational lines (level=info) or everything
 * (level=debug) are added. The stick is not mounted in Linux, so the log is
 * saved through the player's own file layer (core/storage.c):
 *   save=menu    when the menu asks (OPTIONS > External Subtitles > Save log
 *                to USB);
 *   save=always  during startup, then periodically and after crash reports.
 *                The previous run is archived before replacing the canonical
 *                file. If preservation cannot be confirmed, a separate file
 *                identifies this run; failures never prove the old log absent.
 * diagnostics=1 adds crash reports, hangs, process exits and kernel messages
 * (diag.h, crash.h). */
#define _GNU_SOURCE
#include "ub.h"
#include "storage.h"
#include "helper_menu.h"
#include "action_ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ID "log"
#define UB_PREV_TMP "/tmp/ub820_prev.log"
#define UB_SNAPSHOT_TMP "/tmp/ub820_log_snapshot"
#ifndef UB_LOG_BOOT_ID
#define UB_LOG_BOOT_ID "/proc/sys/kernel/random/boot_id"
#endif
#include "crash.h"
#include "diag.h"

enum { SAVE_OFF, SAVE_MENU, SAVE_ALWAYS };
#define SAVE_PAUSE 5      /* seconds between continuous saves */
#define SAVE_RETRY 60     /* after a failed save */

static int level=UB_INFO,save=SAVE_MENU,diagnostics=0;
static const char *const level_names[]={"errors","info","debug",0};
static const char *const save_names[]={"off","menu","always",0};
static const struct ub_option options[]={
 {"level",0,2,&level,level_names},
 {"save",0,2,&save,save_names},
 {"diagnostics",0,1,&diagnostics,0},
 {0,0,0,0,0}
};

/* The stick's log from the previous run is copied aside before this run's
 * log replaces it. */
static int rotation_pending,rotation_loaded,rotation_error;
static unsigned rotation_pause;
static int startup_checkpoint;
static char fallback_path[256];
static struct { long long size;unsigned pause;int failures,fallback; } kept={-1,0,0,0};
static void keep_saved(void);
/* Never overwrite an unpreserved earlier run merely because opening its log
 * failed. Boot UUID plus PID/start time identifies this run without listing USB. */
static int fallback_name(void){
 char boot[40];ssize_t n;int i,w;
 if(fallback_path[0])return 0;
 n=ub_read_file(UB_LOG_BOOT_ID,boot,sizeof boot);
 if(n==37&&boot[36]=='\n')boot[--n]=0;
 if(n!=36)return -1;
 for(i=0;i<36;i++){
  if(i==8||i==13||i==18||i==23){if(boot[i]!='-')return -1;}
  else if(!((boot[i]>='0'&&boot[i]<='9')||(boot[i]>='a'&&boot[i]<='f')||(boot[i]>='A'&&boot[i]<='F')))return -1;
 }
 w=snprintf(fallback_path,sizeof fallback_path,UB_STICK_DIR "/ub820_submenu.%s.%ld.%llu.log",boot,(long)getpid(),ub_now_ms());
 if(w<0||w>=(int)sizeof fallback_path){fallback_path[0]=0;return -1;}
 return 0;
}
static const char *save_destination(void){
 if(!rotation_pending)return UB_STICK_LOG;
 return fallback_name()?0:fallback_path;
}
static const char *save_error(int rc){return rc==11?"safe per-run USB log name unavailable; log retained in RAM":ub_stick_error(rc);}
static int keep_previous(void){
 int rc;
 if(!rotation_pending)return 0;
 if(!rotation_loaded){
  rc=ub_stick_read(UB_STICK_LOG,UB_PREV_TMP);
  /* Code 4 includes every source-open failure, not confirmed absence. */
  if(rc)goto failed;
  rotation_loaded=1;
 }
 rc=ub_stick_write(UB_PREV_TMP,UB_STICK_PREV);
 if(rc)goto failed; /* Keep the complete RAM copy for the next attempt. */
 rotation_pending=rotation_loaded=rotation_error=0;unlink(UB_PREV_TMP);
 ub_log(ID,UB_INFO,"previous run's log kept as %s",UB_STICK_PREV);return 0;
 failed:
 if(rc!=rotation_error)ub_log(ID,UB_ALWAYS,"cannot keep the previous log as %s: %s (code %d); canonical USB log left untouched",UB_STICK_PREV,ub_stick_error(rc),rc);
 rotation_error=rc;return rc;
}
/* Freeze a bounded copy before the vendor tool starts reading. Appends or
 * trimming during USB I/O affect the live log, never the transfer's source. */
static int save_snapshot(const char *destination,long long *saved_size){
 char buf[4096];struct ub_stat st;long long at=0;int in=-1,out=-1,rc=6;ssize_t n;
 in=open(UB_LOG_FILE,O_RDONLY|O_CLOEXEC);if(in<0)return 4;
 if(ub_fstat(in,&st)||!S_ISREG(st.mode)||st.size<0)goto done;
 if(st.size>1024*1024){rc=9;goto done;} /* Vendor copy limit. */
 out=open(UB_SNAPSHOT_TMP,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
 if(out<0){rc=5;goto done;}
 {struct ub_stat dst;if(ub_fstat(out,&dst)||!S_ISREG(dst.mode)){rc=5;goto done;}}
 while(at<st.size){
  size_t want=(size_t)(st.size-at),used=0;if(want>sizeof buf)want=sizeof buf;
  do{n=pread(in,buf,want,(off_t)at);}while(n<0&&errno==EINTR);
  if(n<=0)goto done;
  while(used<(size_t)n){ssize_t w=write(out,buf+used,(size_t)n-used);if(w<0&&errno==EINTR)continue;if(w<=0){rc=7;goto done;}used+=(size_t)w;}
  at+=n;
 }
 {struct ub_stat after;if(ub_fstat(in,&after)||after.size<st.size)goto done;}
 if(close(out)){out=-1;rc=8;goto done;}out=-1;
 close(in);in=-1;
 rc=ub_stick_write(UB_SNAPSHOT_TMP,destination);
 if(!rc&&saved_size)*saved_size=at;
 done:
 if(in>=0)close(in);
 if(out>=0)close(out);
 unlink(UB_SNAPSHOT_TMP);return rc;
}
static void start(struct ub_module *m){
 (void)m;ub_log_level=level;
 {int fd=open(UB_LOG_MODE,O_WRONLY|O_CREAT|O_TRUNC,0600); /* for the menu */
  if(fd>=0){if(write(fd,save_names[save],strlen(save_names[save]))<0){}close(fd);}}
 if(save==SAVE_ALWAYS){
  kept.size=-1;kept.pause=0;kept.failures=kept.fallback=0;
  rotation_pending=1;rotation_loaded=rotation_error=0;rotation_pause=0;unlink(UB_PREV_TMP);
  startup_checkpoint=1;
  ub_log(ID,UB_INFO,"the log is saved to %s continuously",UB_STICK_LOG);
  /* Preserve startup/config evidence before diagnostics or later module
   * initializers can stall. SSH's earlier module order remains unchanged. */
  keep_saved();
 }
 if(diagnostics)diag_start();
}

/* Status for the menu: "<seq> <code> <text>", replaced atomically. */
static char action_reply[UB_ACTION_PATH];
static void answer(unsigned seq,int code,const char *text){
 char line[160];int n=snprintf(line,sizeof line,"%u %d %s\n",seq,code,text),fd;
 if(n<0||n>=(int)sizeof line)return;
 if(ub_action_reply(action_reply,line,(size_t)n))ub_log(ID,UB_ALWAYS,"cannot acknowledge log-save request");
 fd=open(UB_STORAGE_STATUS ".tmp",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return;
 if(write(fd,line,(size_t)n)!=n){close(fd);unlink(UB_STORAGE_STATUS ".tmp");return;}
 if(close(fd)||rename(UB_STORAGE_STATUS ".tmp",UB_STORAGE_STATUS))unlink(UB_STORAGE_STATUS ".tmp");
}
/* save=always: write the RAM log to the stick when it changed, at most every
 * SAVE_PAUSE seconds unless something urgent was logged. Each write replaces
 * the whole file through the player's file layer. */
static void keep_saved(void){
 struct ub_stat st;int rc,now_urgent,target_changed;long long saved_size;const char *destination;
 if(kept.pause)kept.pause--;
 if(rotation_pause)rotation_pause--;
 if(rotation_pending&&!rotation_pause){if(keep_previous())rotation_pause=SAVE_RETRY;}
 if(ub_stat(UB_LOG_FILE,&st))return;
 now_urgent=urgent||!access(UB_LOG_URGENT,F_OK);
 target_changed=kept.fallback!=!!rotation_pending;
 if(!target_changed&&st.size==kept.size)return;
 if(kept.pause&&!(target_changed&&!rotation_pending)&&!(now_urgent&&!kept.failures))return;
 destination=save_destination();if(!destination){rc=11;goto failed;}
 if(target_changed&&rotation_pending)ub_log(ID,UB_INFO,"current run's log uses %s while the previous USB log is retained",destination);
 urgent=0;unlink(UB_LOG_URGENT);
 rc=save_snapshot(destination,&saved_size);
 if(rc){
  /* Consume only the old urgency; concurrent crash reporters may have
   * published another marker during the copy. A failure retains this one. */
  if(now_urgent){int fd;urgent=1;fd=open(UB_LOG_URGENT,O_WRONLY|O_CREAT|O_CLOEXEC|O_NONBLOCK,0600);if(fd>=0)close(fd);}
 failed:
  if(!kept.failures++)ub_log(ID,UB_ALWAYS,"cannot save the log to the stick: %s (code %d); trying again every minute",save_error(rc),rc);
  kept.pause=SAVE_RETRY;return;
 }
 if(kept.failures){ub_log(ID,UB_ALWAYS,"the log is saved to the stick again (after %d failed attempts)",kept.failures);kept.failures=0;}
 kept.size=saved_size;kept.pause=SAVE_PAUSE;kept.fallback=!!rotation_pending;
}
static void step(struct ub_part *p,struct ub_target *t){
 char buf[64],message[256];unsigned seq;int rc;const char *destination;
 (void)p;(void)t;
 if(save==SAVE_ALWAYS){
  /* Capture the later module initializers before the first payload install.
   * A failed early write keeps its normal retry deadline. */
  if(startup_checkpoint){startup_checkpoint=0;if(!kept.failures)kept.pause=0;}
  keep_saved();
 }
 if(!ub_action_receive(UB_STORAGE_REQUEST,buf,sizeof buf,action_reply,sizeof action_reply))return;
 if(sscanf(buf,"save-log %u",&seq)!=1){ub_log(ID,UB_INFO,"ignored storage request");return;}
 if(save==SAVE_OFF){answer(seq,10,"saving is off ([log] save=off)");return;}
 if(rotation_pending){if(keep_previous())rotation_pause=SAVE_RETRY;}
 destination=save_destination();
 if(!destination)rc=11;
 else{ub_log(ID,UB_INFO,"saving the log to %s",destination);rc=save_snapshot(destination,0);}
 if(rc)ub_log(ID,UB_ALWAYS,"cannot save the log to the stick: %s (code %d)",save_error(rc),rc);
 else ub_log(ID,UB_INFO,"log saved to %s",destination);
 if(rc)snprintf(message,sizeof message,"%s",save_error(rc));
 else if(!rotation_pending)snprintf(message,sizeof message,"saved as sub/ub820_submenu.log");
 else snprintf(message,sizeof message,"saved to %s",destination);
 answer(seq,rc,message);
}

/* Mount table: listed once, then every change, so it is visible when and
 * where the stick and the disc are mounted (the player mounts on demand). */
#define MOUNT_LINES 48
#define MOUNT_TEXT 160
static char mounts[MOUNT_LINES][MOUNT_TEXT];
static unsigned mount_count;
static int mounts_listed;
static int mount_known(char (*list)[MOUNT_TEXT],unsigned n,const char *line){
 for(unsigned i=0;i<n;i++)if(!strcmp(list[i],line))return 1;
 return 0;
}
static void mount_step(struct ub_part *p,struct ub_target *t){
 static char now[MOUNT_LINES][MOUNT_TEXT];
 static char text[16384];
 char *buf,*save,dev[200],dir[200],type[40],opts[200];unsigned n=0,i;
 (void)p;(void)t;
 if(ub_read_file("/proc/mounts",text,sizeof text)<0)return;
 for(buf=strtok_r(text,"\n",&save);n<MOUNT_LINES&&buf;buf=strtok_r(0,"\n",&save)){
  if(sscanf(buf,"%199s %199s %39s %199s",dev,dir,type,opts)!=4)continue;
  snprintf(now[n++],MOUNT_TEXT,"%.60s %.60s %.12s %.20s",dev,dir,type,opts);
 }
 if(!mounts_listed){mounts_listed=1;for(i=0;i<n;i++)ub_log(ID,UB_INFO,"mount: %s",now[i]);}
 else{
  for(i=0;i<n;i++)if(!mount_known(mounts,mount_count,now[i]))ub_log(ID,UB_INFO,"mounted: %s",now[i]);
  for(i=0;i<mount_count;i++)if(!mount_known(now,n,mounts[i]))ub_log(ID,UB_INFO,"unmounted: %s",mounts[i]);
 }
 memcpy(mounts,now,sizeof mounts);mount_count=n;
}

static void diag_step(struct ub_part *p,struct ub_target *t){(void)p;(void)t;if(diagnostics)diag_tick();}

static struct ub_part parts[]={
 {"diagnostics",0,0,0,0,0,diag_step,{0},0,0},
 {"mounts",0,0,0,0,0,mount_step,{0},0,0},
 {"save",0,0,0,0,0,step,{0},0,0}, /* last: saves what the others logged */
};
struct ub_module ub_module_log={
 ID,"Log detail, saving the log to the USB stick (on request or always), detailed diagnostics, mount record",
 1,options,start,parts,sizeof parts/sizeof *parts,crash_command
};
