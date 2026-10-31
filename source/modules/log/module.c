/* Logging: how much goes into the RAM log, saving it to the stick, detailed
 * diagnostics of the player itself, and a record of mounts.
 *
 * Errors and essential events are always kept in the RAM log, even with this
 * module off. With it on, informational lines (level=info) or everything
 * (level=debug) are added. The stick is not mounted in Linux, so the log is
 * saved through the player's own file layer (core/storage.c):
 *   save=menu    when the menu asks (OPTIONS > External Subtitles > Save log
 *                to USB);
 *   save=always  continuously: a few seconds after it changes, at once after
 *                a crash or hang report, and the previous run's log is kept
 *                as sub/ub820_submenu.prev.log when the helper starts, so the
 *                log of a run that ended in a freeze or reboot survives.
 * diagnostics=1 adds crash reports, hangs, process exits and kernel messages
 * (diag.h, crash.h). */
#define _GNU_SOURCE
#include "ub.h"
#include "storage.h"
#include "helper_menu.h"
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
static void keep_previous(void){
 int rc=ub_stick_read(UB_STICK_LOG,UB_PREV_TMP);
 if(rc==4)return; /* no earlier log */
 if(!rc)rc=ub_stick_write(UB_PREV_TMP,UB_STICK_PREV);
 unlink(UB_PREV_TMP);
 if(rc)ub_log(ID,UB_ALWAYS,"cannot keep the previous log as %s: %s (code %d)",UB_STICK_PREV,ub_stick_error(rc),rc);
 else ub_log(ID,UB_INFO,"previous run's log kept as %s",UB_STICK_PREV);
}
static void start(struct ub_module *m){
 (void)m;ub_log_level=level;
 {int fd=open(UB_LOG_MODE,O_WRONLY|O_CREAT|O_TRUNC,0600); /* for the menu */
  if(fd>=0){if(write(fd,save_names[save],strlen(save_names[save]))<0){}close(fd);}}
 if(save==SAVE_ALWAYS){keep_previous();ub_log(ID,UB_INFO,"the log is saved to %s continuously",UB_STICK_LOG);}
 if(diagnostics)diag_start();
}

/* Status for the menu: "<seq> <code> <text>", replaced atomically. */
static void answer(unsigned seq,int code,const char *text){
 char line[160];int n=snprintf(line,sizeof line,"%u %d %s\n",seq,code,text),fd;
 if(n<0||n>=(int)sizeof line)return;
 fd=open(UB_STORAGE_STATUS ".tmp",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return;
 if(write(fd,line,(size_t)n)!=n){close(fd);unlink(UB_STORAGE_STATUS ".tmp");return;}
 if(close(fd)||rename(UB_STORAGE_STATUS ".tmp",UB_STORAGE_STATUS))unlink(UB_STORAGE_STATUS ".tmp");
}
/* save=always: write the RAM log to the stick when it changed, at most every
 * SAVE_PAUSE seconds unless something urgent was logged. Each write replaces
 * the whole file through the player's file layer. */
static struct { long long size;unsigned pause;int failures; } kept={-1,0,0};
static void keep_saved(void){
 struct ub_stat st;int rc,now_urgent;
 if(kept.pause)kept.pause--;
 if(ub_stat(UB_LOG_FILE,&st))return;
 now_urgent=urgent||!access(UB_LOG_URGENT,F_OK);
 if(st.size==kept.size)return; /* the log only grows, or is trimmed */
 if(kept.pause&&!(now_urgent&&!kept.failures))return;
 urgent=0;unlink(UB_LOG_URGENT);
 rc=ub_stick_write(UB_LOG_FILE,UB_STICK_LOG);
 if(rc){
  if(!kept.failures++)ub_log(ID,UB_ALWAYS,"cannot save the log to the stick: %s (code %d); trying again every minute",ub_stick_error(rc),rc);
  kept.pause=SAVE_RETRY;return;
 }
 if(kept.failures){ub_log(ID,UB_ALWAYS,"the log is saved to the stick again (after %d failed attempts)",kept.failures);kept.failures=0;}
 kept.size=st.size;kept.pause=SAVE_PAUSE;
}
static void step(struct ub_part *p,struct ub_target *t){
 char buf[64];unsigned seq;int fd,rc;ssize_t n;
 (void)p;(void)t;
 if(save==SAVE_ALWAYS)keep_saved();
 fd=open(UB_STORAGE_REQUEST,O_RDONLY);if(fd<0)return;
 n=read(fd,buf,sizeof buf-1);close(fd);unlink(UB_STORAGE_REQUEST);
 if(n<=0)return;
 buf[n]=0;
 if(sscanf(buf,"save-log %u",&seq)!=1){ub_log(ID,UB_INFO,"ignored storage request");return;}
 if(save==SAVE_OFF){answer(seq,10,"saving is off ([log] save=off)");return;}
 ub_log(ID,UB_INFO,"saving the log to %s",UB_STICK_LOG);
 rc=ub_stick_write(UB_LOG_FILE,UB_STICK_LOG);
 if(rc)ub_log(ID,UB_ALWAYS,"cannot save the log to the stick: %s (code %d)",ub_stick_error(rc),rc);
 else ub_log(ID,UB_INFO,"log saved to %s",UB_STICK_LOG);
 answer(seq,rc,rc?ub_stick_error(rc):"saved as sub/ub820_submenu.log");
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
