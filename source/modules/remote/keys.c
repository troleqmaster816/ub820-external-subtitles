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
#include "helper_menu.h"

extern int open(const char *,int,...);
extern int close(int);
extern int write(int,const void *,unsigned int);

/* Loader fills this before the branch is installed. */
int (*ub_original_key_filter)(int *);

#define CTX_PASS_ALL (0x64/4)
#define CTX_EVENT (0xe0/4)  /* 1 for a key press (message type 2), 2 otherwise */
#define CTX_KEY (0xe4/4)
#define KEY_NETFLIX 0x168
#define O_WRONLY_NONBLOCK 04001  /* O_WRONLY|O_NONBLOCK on ARM Linux */

static int exists(const char *path){int fd=open(path,0);if(fd<0)return 0;close(fd);return 1;}
/* Sends a press to the overlay; 0 when its reader is there (opening a FIFO
 * for writing without blocking fails while nobody reads it). */
static int to_overlay(int key,int press){
 uint32_t record=(uint32_t)key;int fd=open(UB_KEYS_FIFO,O_WRONLY_NONBLOCK);
 if(fd<0)return -1;
 if(press&&write(fd,&record,sizeof record)){}
 close(fd);return 0;
}

int ub_hook_key_filter(int *ctx){
 if(ctx&&ctx[CTX_PASS_ALL]!=2){
  int key=ctx[CTX_KEY],press=ctx[CTX_EVENT]==1;
  if(key==KEY_NETFLIX){
   if(exists(UB_NETFLIX_HELPER)){to_overlay(key,press);return 1;}
   if(exists(UB_NETFLIX_OFF))return 1;
  }
  if(exists(UB_OVERLAY_OPEN)&&to_overlay(key,press)==0)return 1;
 }
 return ub_original_key_filter(ctx);
}
