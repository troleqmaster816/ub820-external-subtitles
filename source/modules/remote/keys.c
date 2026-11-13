/* UB820 1.82 remote keys (apl_mgr). A freestanding PIC DSO installed into
 * apl_mgr, which hands every remote key to the running application; the
 * loader bindings and the original are described in keys_hooks.json.
 *
 * FUN_000202ec translates a remote key into ctx+0xe4 and asks FUN_0002e4f0
 * (also asked for HDMI-CEC keys, FUN_0001b4a4, and by FUN_0001d9b8) whether
 * to drop it ("Key Cancel (...)" in the player's debug log: 1 = drop). The
 * stock filter already drops the Netflix key (0x168) on models without
 * Netflix. The hook, before the original:
 *   - Netflix key while UB_NETFLIX_HELPER exists: sent to the UB820 Helper
 *     overlay in apl_dispsrv (UB_KEYS_FIFO, it opens or closes the overlay)
 *     and dropped;
 *   - Netflix key while UB_NETFLIX_OFF exists: dropped;
 *   - any key while UB_OVERLAY_OPEN exists and the overlay reads the FIFO:
 *     presses are sent to it and every event (press, repeat, release) is
 *     dropped, so the application underneath gets none. If nobody reads the
 *     FIFO (apl_dispsrv or its payload is gone), every key goes to the
 *     original as usual: the remote cannot get stuck.
 * The state in which the stock filter lets every key pass (ctx+0x64 == 2)
 * is left to the original. The marker files are kept by the remote module
 * (Netflix choice) and by the overlay (open). */
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include "payload_time.h"
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include "helper_menu.h"


/* Loader fills this before the branch is installed. */
int (*ub_original_key_filter)(int *);

#define CTX_PASS_ALL (0x64/4)
#define CTX_EVENT (0xe0/4)  /* 1 for a key press (message type 2), 2 otherwise */
#define CTX_KEY (0xe4/4)
#define KEY_NETFLIX 0x168
#define O_WRONLY_NONBLOCK (O_WRONLY|O_NONBLOCK|O_CLOEXEC)

static int exists(const char *path){int fd=open(path,0);if(fd<0)return 0;close(fd);return 1;}
/* Reader-open alone does not prove a functioning UI: it refreshes a lease
 * from its normal timer. Once expired, unrelated keys return to stock. */
static int overlay_live(void){
 uint32_t expires,now;int fd=open(UB_OVERLAY_OPEN,0),n;
 if(fd<0)return 0;
 n=read(fd,&expires,sizeof expires);close(fd);
 return n==(int)sizeof expires&&!ub_payload_now(&now)&&(int32_t)(expires-now)>0;
}
static void abort_overlay(void){int fd=open(UB_OVERLAY_ABORT,0101,0600);if(fd>=0)close(fd);}
/* A temporary read descriptor pins a reader through write(), so a UI exit
 * between the first open and the write cannot deliver SIGPIPE to apl_mgr.
 * It never reads/steals data; no reader at the first open still fails. */
static int to_overlay(int key,int press){
 char descriptor[40];uint32_t record=(uint32_t)key;int fd=open(UB_KEYS_FIFO,O_WRONLY_NONBLOCK),reader,n=0;
 if(fd<0)return -1;
 if(press){
  snprintf(descriptor,sizeof descriptor,"/proc/self/fd/%d",fd);
  reader=open(descriptor,O_RDONLY|O_NONBLOCK|O_CLOEXEC); /* pin this exact FIFO inode */
  if(reader<0){close(fd);return -1;}
  do{n=write(fd,&record,sizeof record);}while(n<0&&errno==EINTR);
  close(reader);
 }
 close(fd);return !press||n==(int)sizeof record?0:-1;
}

int ub_hook_key_filter(int *ctx){
 if(ctx&&ctx[CTX_PASS_ALL]!=2){
  int key=ctx[CTX_KEY],press=ctx[CTX_EVENT]==1;
  if(key==KEY_NETFLIX){
   if(exists(UB_NETFLIX_HELPER)){if(to_overlay(key,press)&&overlay_live())abort_overlay();return 1;}
   if(exists(UB_NETFLIX_OFF))return 1;
  }
  if(overlay_live()){
   if(to_overlay(key,press))abort_overlay();
   return 1; /* failed press closes overlay; never pass it to hidden stock */
  }
 }
 return ub_original_key_filter(ctx);
}
