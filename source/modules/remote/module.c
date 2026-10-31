/* Remote: what the remote's Netflix button does.
 *
 * apl_mgr hands every remote key to the running application after asking its
 * key filter (FUN_0002e4f0) whether to drop it. keys.c, a payload in
 * apl_mgr, drops the Netflix key (0x168) while UB_NETFLIX_OFF exists, and
 * sends it to the UB820 Helper overlay (ui module, apl_dispsrv) through
 * UB_KEYS_FIFO while UB_NETFLIX_HELPER exists; this module makes the FIFO
 * and keeps those files in step with netflix_key:
 *   netflix  the button opens Netflix, as without the helper
 *   nothing  the button does nothing
 *   helper   the button opens and closes the UB820 Helper overlay, on the
 *            home screen, in menus and during playback
 * Menu: the UB820 Helper menu (ui module) shows and changes netflix_key
 * through helper_menu.h; the choice is saved to the stick and applies at
 * once. */
#include "ub.h"
#include "payloads.h"
#include "helper_menu.h"
#include "storage.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#define ID "remote"
#define APL_MGR_PATH "/usr/dtvrec/bin/apl_mgr"
/* 1 when keys.c has a variant for this apl_mgr (1.82, or 1.85 with its own
 * addresses); otherwise the menu does not offer the Netflix button (status
 * file absent), since nothing would act on it. */
static int keys_ok;

enum { NETFLIX_STOCK, NETFLIX_NOTHING, NETFLIX_HELPER };
static const char *const netflix_names[]={"netflix","nothing","helper",0};
static int netflix_key=NETFLIX_STOCK;

/* One marker file present or absent; 0 ok. */
static int marker(const char *path,int present){
 int fd;
 if(present){
  fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);
  if(fd<0){ub_log(ID,UB_ALWAYS,"cannot create %s",path);return -1;}
  close(fd);return 0;
 }
 if(unlink(path)&&access(path,F_OK)==0){ub_log(ID,UB_ALWAYS,"cannot remove %s",path);return -1;}
 return 0;
}
/* The files the payload checks on each Netflix press; marker_state is what
 * they were last set to (-1 unknown, so start() always sets them). */
static int marker_state=-1;
static void sync_marker(void){
 static const char *const said[]={"opens Netflix","does nothing","opens the UB820 Helper overlay"};
 if(netflix_key==marker_state)return;
 if(marker(UB_NETFLIX_OFF,netflix_key==NETFLIX_NOTHING)||marker(UB_NETFLIX_HELPER,netflix_key==NETFLIX_HELPER))return;
 marker_state=netflix_key;
 ub_log(ID,UB_INFO,"Netflix button: %s",said[netflix_key]);
}

/* Status for the menu, rewritten only when it changes. */
static unsigned menu_seq;
static char menu_message[96]="";
static void menu_status(void){
 static char last[160];char line[160];int n,fd;
 n=snprintf(line,sizeof line,"%u %s %s\n",menu_seq,netflix_names[netflix_key],menu_message);
 if(n<0||n>=(int)sizeof line||!strcmp(line,last))return;
 fd=open(UB_REMOTE_STATUS ".tmp",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return;
 if(write(fd,line,(size_t)n)!=n){close(fd);unlink(UB_REMOTE_STATUS ".tmp");return;}
 if(close(fd)||rename(UB_REMOTE_STATUS ".tmp",UB_REMOTE_STATUS)){unlink(UB_REMOTE_STATUS ".tmp");return;}
 memcpy(last,line,sizeof last);
}
/* "set <choice> <seq>" from the menu: apply, save to the stick, answer. */
static void menu_request(void){
 char buf[64],choice[16];unsigned seq;int fd,rc;ssize_t n;
 fd=open(UB_REMOTE_REQUEST,O_RDONLY);if(fd<0)return;
 n=read(fd,buf,sizeof buf-1);close(fd);unlink(UB_REMOTE_REQUEST);
 if(n<=0)return;
 buf[n]=0;
 if(sscanf(buf,"set %15s %u",choice,&seq)!=2){ub_log(ID,UB_INFO,"ignored menu request");return;}
 rc=ub_setting_set(ID,"netflix_key",choice);
 menu_seq=seq;
 if(rc<0)snprintf(menu_message,sizeof menu_message,"unknown choice %s",choice);
 else if(rc>0)snprintf(menu_message,sizeof menu_message,"not saved to USB (%s)",ub_stick_error(rc));
 else snprintf(menu_message,sizeof menu_message,"saved");
 ub_log(ID,UB_ALWAYS,"Netflix button chosen in the menu: %s (%s)",netflix_names[netflix_key],menu_message);
}
static void menu_step(struct ub_part *p,struct ub_target *t){
 (void)p;(void)t;
 if(!keys_ok)return;
 menu_request();
 sync_marker();
 menu_status();
}

static const struct ub_option options[]={
 {"netflix_key",NETFLIX_STOCK,NETFLIX_HELPER,&netflix_key,netflix_names},
 {0,0,0,0,0}
};
static struct ub_part parts[]={
 {"keys","apl_mgr",UB_SHA256_APL_MGR,&ub_payload_remote_keys,1,0,0,{0},0,0},
 {"menu",0,0,0,0,0,menu_step,{0},0,0},
};
/* The key FIFO for the overlay. One left by an earlier run is kept: the
 * overlay in apl_dispsrv (which outlives the helper) may hold it open. A
 * stale overlay marker is removed, so no key is held back. */
static void start(struct ub_module *m){
 struct ub_stat st;
 (void)m;
 unlink(UB_REMOTE_STATUS);unlink(UB_OVERLAY_OPEN);
 if(ub_stat(UB_KEYS_FIFO,&st)||!S_ISFIFO(st.mode)){
  unlink(UB_KEYS_FIFO);
  if(mkfifo(UB_KEYS_FIFO,0600))ub_log(ID,UB_ALWAYS,"cannot make " UB_KEYS_FIFO "; the Netflix button cannot open the overlay");
 }
 {char h[65];keys_ok=!ub_hash_file(APL_MGR_PATH,h)&&ub_payload_variant(&ub_payload_remote_keys,h);}
 if(!keys_ok){ub_log(ID,UB_ALWAYS,APL_MGR_PATH " is not a supported 1.82 or 1.85 build, or is missing; the Netflix button option is off");netflix_key=NETFLIX_STOCK;}
 marker_state=-1;sync_marker();
}
struct ub_module ub_module_remote={
 ID,"Remote: what the Netflix button does (netflix_key: Netflix, nothing, or the UB820 Helper overlay)",
 1,options,start,parts,sizeof parts/sizeof *parts,0
};
