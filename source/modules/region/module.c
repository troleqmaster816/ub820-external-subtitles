/* Region: BD region chosen per disc, and region diagnostics.
 *
 * A disc's region check is the disc's own program reading PSR20 (1 A, 2 B,
 * 4 C); the player has no check to remove, so this module chooses the value.
 * BD (apl_bdv, started for each disc): FUN_0004a5b8 asks the drive for its
 *   region (SDL_GetRegionInfo) as apl_bdv starts and turns the answer into
 *   1, 2 or 4 with one movs each; FUN_0004aea0 keeps it at 0x00aeaec0,
 *   GetMM_PlayerConfig (FUN_00049148) hands it to the engine, and
 *   BDROM_PCE_LB_SetPlayerConfigPSR (FUN_000a3e14) copies it into PSR20
 *   (0x00b35b6c), which the disc's HDMV or BD-J program reads. A verified
 *   launcher hook in apl_medm adds a private preloader only to future BD
 *   launches. Its normal ELF constructor changes those three immediates
 *   before native main and region initialization. The old watcher/PSR20
 *   correction remains a best-effort fallback if the launcher cannot arm.
 *   bd_region: stock leave the player alone (default)
 *              auto  only with bd_auto=1 (off by default, experimental):
 *                    keep the player's region unless the disc's HDMV
 *                    program only accepts other regions; then use one it
 *                    accepts. Discs that check their region in Java (BD-J)
 *                    keep the player's region. With bd_auto=0, auto is
 *                    treated as stock and the menu does not offer it.
 *              A, B, C, ABC  use that region for every BD (ABC = 7, all
 *                    three bits; accepted by discs that test a bit mask).
 * DVD (apl_meda_dvdv + libnavi_api.so, prelinked at 0x2a340000). The player
 *   region mask SPRM20 (u16 at 0x2a3b48c8) is set in dvd_nv_start from
 *   FUN_2a3748f4, which asks the drive (SDL_GetRegionInfo). On the first
 *   sector read, FUN_2a38a6d4 asks the drive whether the disc's region
 *   matches (SDL_GetMediaInfo) and returns DVD_ERR_REGION (0xa0080018) if
 *   not, else caches "passed" in 0x2a3aa414. dvd_region (best effort, RAM
 *   only, in that process's own copy of the library code):
 *     stock  leave the player alone (default)
 *     free   the gate never refuses: "bmi" at 0x2a38a704 becomes "b"
 *     1..8   the gate, and FUN_2a3748f4 returns that region (movs r0,#mask;
 *            bx lr), SPRM20 set as well for the running disc
 *     auto   the gate; a disc whose VIDEO_TS.IFO excludes the player's own
 *            region gets the first region it allows
 *   A disc-start guard (like the BD one) puts them in place within 20 ms of
 *   apl_meda_dvdv mapping the library; the main loop keeps them and takes
 *   them out again for stock. The drive keeps its own region: whether it
 *   still refuses an encrypted (CSS) foreign disc is not known.
 * Disc files: index.bdmv (First Play HDMV or BD-J), the HDMV commands in
 *   MovieObject.bdmv that read PSR20, and the prohibited-region mask in
 *   VIDEO_TS.IFO (byte 0x23).
 * Menu: the UB820 Helper menu (ui module) shows bd_region and what the
 *   current Blu-ray sees, and can change bd_region (helper_menu.h); the choice
 *   is saved to the stick and applies from the next disc on. The same for
 *   dvd_region, which applies within a second. */
#define _GNU_SOURCE
#include "ub.h"
#include "payloads.h"
#include "helper_menu.h"
#include "storage.h"
#include "action_ipc.h"
#include "assets.h"
#include "bd_launch.h"
#include "bd_policy.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ID "region"
#define BDV_SHA256 "0eed5038db1c678b14cf666752c1c02db6c884ec1229329b9867f4342544714d"
#define DVDV_SHA256 "8ab24aac32c99775c2452d87cd5864b25f66e092cdebef3411100735df75835e"
#define NAVI_SHA256 "a7692f0fa0e7e3dfb3cd90c69a7e9120204ab2c9b55ae3eea9b22e6425b6fbc3"
#define BDV_REGION_ADDR 0x00aeaec0u
#define BDV_185_SHIFT 0xf0u    /* apl_bdv 1.85: the region data slots moved +0xF0 */
#define NAVI_LINK_BASE 0x2a340000u
#define NAVI_SPRM20 0x2a3b48c8u
#define NAVI_REGION_PASSED 0x2a3aa414u
#define NAVI_GATE 0x2a38a704u        /* bmi -> read path, after SDL_GetMediaInfo */
#define NAVI_REGION_SOURCE 0x2a3748f4u /* FUN_2a3748f4: drive region as a mask */
#define DVDV_PATH "/usr/dtvrec/bin/apl_meda_dvdv"
#define NAVI_PATH "/usr/dtvrec/lib/libnavi_api.so"
#define DISC_FILE_LIMIT (1024*1024)
#define HDMV_LOG_LIMIT 24

enum { BD_AUTO, BD_STOCK, BD_A, BD_B, BD_C, BD_ABC };
static int bd_region=BD_STOCK;
static int bd_auto=0;   /* allows bd_region=auto */
static const char *const bd_region_names[]={"auto","stock","A","B","C","ABC",0};
static const uint32_t bd_region_value[]={0,0,1,2,4,7};
/* 1..8 are the regions themselves, so dvd_region=3 means region 3 */
enum { DVD_STOCK, DVD_R1, DVD_R8=8, DVD_AUTO, DVD_FREE };
static int dvd_region=DVD_STOCK;
static const char *const dvd_region_names[]={"stock","1","2","3","4","5","6","7","8","auto","free",0};

static const char *bd_letters(uint32_t v){
 switch(v){case 0:return "none";case 1:return "A";case 2:return "B";case 4:return "C";case 7:return "A|B|C";default:return "unusual";}
}
static void mask_letters(unsigned mask,char *out,size_t size){
 snprintf(out,size,"%s%s%s",mask&1?"A":"",mask&2?"B":"",mask&4?"C":"");
}

/* The decision for the BD in the drive: 0 none, 'K' keep the player's
 * region, 'F' fixed value, 'M' mask of regions the disc tests (keep the
 * player's region if it is in the mask, else the first one). */
static char decision_mode;
static unsigned decision_value;
/* For the menu: is a BD in the drive, and the player's own region (from
 * apl_bdv, 0 until known). */
static int bd_present;
static uint32_t bd_player;
#define region_for ub_bd_region_for

#define BDV_PATH "/usr/dtvrec/bin/apl_bdv"
#define BDV_PSR20_ADDR 0x00b35b6cu
/* The addresses actually used (set in start(): 1.82 as above, 1.85 +0xF0). */
static uint32_t bdv_region_addr=BDV_REGION_ADDR,bdv_psr20_addr=BDV_PSR20_ADDR;
/* The three movs in FUN_0004a5b8 that turn the drive's answer A, B, C into
 * the value apl_bdv keeps: moveq r3,#1 / movs r3,#2 / movs r3,#4 (bytes
 * imm, 0x23). Same bytes and addresses in 1.82 and 1.85. */
static const uint32_t bdv_source[3]={0x0004a738u,0x0004a7bcu,0x0004a7c2u};
static const unsigned char bdv_source_stock[3]={1,2,4};
#define GUARD_SECONDS 40
#define GUARD_POLL_MS 20
static struct { unsigned long long dev,ino; long long size,mtime; int ok; } bdv_file;
/* What the disc in the running apl_bdv sees (its PSR20), 0 if none. */
static uint32_t bd_psr;
/* Ready means both the exact asset and current launch policy are published.
 * Armed additionally means the media-manager hook has finished installing. */
static int bd_launch_ready,bd_launch_armed;
static int bd_launch_pid;
static struct ub_part *bd_launch_part;
static int bd_launch_publish(void);

static int bdv_identical(int pid){
 char p[40];struct ub_stat st;
 snprintf(p,sizeof p,"/proc/%d/exe",pid);
 return bdv_file.ok&&!ub_stat(p,&st)&&st.dev==bdv_file.dev&&st.ino==bdv_file.ino&&st.size==bdv_file.size&&st.mtime==bdv_file.mtime;
}
static long since_ms(const struct timespec *from){
 struct timespec now;ub_now(&now);
 return (long)(now.tv_sec-from->tv_sec)*1000+(now.tv_nsec-from->tv_nsec)/1000000;
}
static void decide_clear(void){decision_mode=0;bd_psr=0;}

static int dvd_apply(int pid,uint32_t base,int quiet);
static void dvd_describe(char *out,size_t size);
/* The DVD guard: started when a DVD is found and dvd_region is not stock.
 * It looks for apl_meda_dvdv every 20 ms and puts the DVD region code in
 * place as soon as the verified libnavi_api.so is mapped, before the first
 * sector read if it can; the main loop (dvd_step) keeps it afterwards. */
static pid_t dvd_guard_pid;
static volatile sig_atomic_t dvd_guard_cancel;
static void dvd_guard_cancelled(int sig){(void)sig;dvd_guard_cancel=1;}
static struct { unsigned long long dev,ino; long long size,mtime; int ok; } dvdv_file,navi_file;
static int same_file(const char *path,unsigned long long dev,unsigned long long ino,long long size,long long mtime){
 struct ub_stat st;return !ub_stat(path,&st)&&st.dev==dev&&st.ino==ino&&st.size==size&&st.mtime==mtime;
}
static int find_dvdv(void){
 static struct ub_dir d;const char *e;int found=0;
 if(ub_dir_open(&d,"/proc"))return 0;
 while(!found&&(e=ub_dir_next(&d))){
  char p[64],comm[32];int pid=atoi(e),fd;ssize_t n;
  if(pid<=0)continue;
  snprintf(p,sizeof p,"/proc/%d/comm",pid);fd=open(p,O_RDONLY);if(fd<0)continue;
  n=read(fd,comm,sizeof comm-1);close(fd);if(n<=0)continue;
  comm[n]=0;comm[strcspn(comm,"\n")]=0;
  snprintf(p,sizeof p,"/proc/%d/exe",pid);
  if(!strcmp(comm,"apl_meda_dvdv")&&same_file(p,dvdv_file.dev,dvdv_file.ino,dvdv_file.size,dvdv_file.mtime))found=pid;
 }
 ub_dir_close(&d);return found;
}
static void dvd_guard_run(void){
 struct timespec start,seen;int pid=0;char path[600],text[64];uint32_t base;
 ub_now(&start);
 while(!dvd_guard_cancel&&since_ms(&start)<GUARD_SECONDS*1000){
  if(!pid){if((pid=find_dvdv()))ub_now(&seen);else{ub_sleep_ms(GUARD_POLL_MS);continue;}}
  if(kill(pid,0))break;
  base=ub_map_base(pid,"/libnavi_api.so",path,sizeof path);
  if(!base||!same_file(path,navi_file.dev,navi_file.ino,navi_file.size,navi_file.mtime)){ub_sleep_ms(GUARD_POLL_MS);continue;}
  if(dvd_apply(pid,base,0)>=0){dvd_describe(text,sizeof text);ub_log(ID,UB_ALWAYS,"DVD region in apl_meda_dvdv PID%d: %s (%ld ms after it appeared)",pid,text,since_ms(&seen));}
  _exit(0);
 }
 if(!pid)ub_log(ID,UB_INFO,"DVD region guard: no apl_meda_dvdv appeared within %d s",GUARD_SECONDS);
 _exit(0);
}
/* A guard may own a stopped patch transaction. Never SIGKILL it midway:
 * request cancellation, then let it restore/detach before handing ownership
 * to the main loop or publishing a new configuration. */
static int dvd_guard_stop(void){
 if(dvd_guard_pid>0){
  unsigned long long until=ub_now_ms()+1000;int status;pid_t r;
  do{r=waitpid(dvd_guard_pid,&status,WNOHANG);}while(r<0&&errno==EINTR);
  if(!r){
   kill(dvd_guard_pid,SIGTERM);
   do{
    r=waitpid(dvd_guard_pid,&status,WNOHANG);
    if(r>0||(r<0&&errno==ECHILD))break;
    if(r<0&&errno!=EINTR)return 0;
    if(ub_now_ms()>=until)return 0; /* retain ownership and retry next tick */
    ub_sleep_ms(1);
   }while(1);
  }else if(r<0&&errno!=ECHILD)return 0;
 }
 dvd_guard_pid=0;return 1;
}
static void dvd_guard_start(void){
 pid_t pid;sigset_t term,old;
 if(!dvd_guard_stop())return;
 if(dvd_region==DVD_STOCK)return;
 if(!dvdv_file.ok||!navi_file.ok){ub_log(ID,UB_ALWAYS,"apl_meda_dvdv or libnavi_api.so not verified; the DVD region is set from the main loop only (up to 1 s late)");return;}
 sigemptyset(&term);sigaddset(&term,SIGTERM);
 if(sigprocmask(SIG_BLOCK,&term,&old))return;
 pid=fork();
 if(!pid){
  struct sigaction action;memset(&action,0,sizeof action);action.sa_handler=dvd_guard_cancelled;sigemptyset(&action.sa_mask);
  dvd_guard_cancel=0;sigaction(SIGTERM,&action,0);
  sigdelset(&old,SIGTERM);sigprocmask(SIG_SETMASK,&old,0);
  dvd_guard_run();
 }
 sigprocmask(SIG_SETMASK,&old,0);
 dvd_guard_pid=pid>0?pid:0;
}
static void decide_set(char mode,unsigned value,const char *why){
 char letters[8];
 decision_mode=mode;decision_value=value;
 if(mode=='F')ub_log(ID,UB_INFO,"BD region for this disc: %s (%s)",bd_letters(value),why);
 else if(mode=='M'){mask_letters(value,letters,sizeof letters);ub_log(ID,UB_INFO,"BD region for this disc: player's region if it is one of %s, else the first of them (%s)",letters,why);}
 else ub_log(ID,UB_INFO,"BD region for this disc: player's region (%s)",why);
}

/* 1 when the three movs of FUN_0004a5b8 in pid are the stock ones, so the
 * region apl_bdv keeps is the drive's own. */
static int bd_source_stock_in(int pid){
 unsigned char b[2];unsigned i;
 for(i=0;i<3;i++)if(ub_mem_read(pid,bdv_source[i],b,2)||b[1]!=0x23||b[0]!=bdv_source_stock[i])return 0;
 return 1;
}
/* apl_bdv (verified by the core): the player's region (only where the
 * watcher left the source alone), what the disc sees, and PSR20 kept at the
 * decision for the life of the process, in case the engine restores its
 * registers (FUN_0009ab1c). state: [0] 1 once the region was logged, [1] 1
 * once a main-loop correction was logged */
static void bd_step(struct ub_part *p,struct ub_target *t){
 uint32_t player,psr=0,want;int stock;(void)p;
 if(ub_mem_read(t->pid,bdv_region_addr,&player,4))return;
 stock=bd_source_stock_in(t->pid);
 if(stock&&(player==1||player==2||player==4))bd_player=player;
 if(!ub_mem_read(t->pid,bdv_psr20_addr,&psr,4))bd_psr=psr;
 if(!t->state[0]&&player){t->state[0]=1;ub_log(ID,UB_INFO,stock?"BD player region from the drive in apl_bdv PID%d: 0x%x (%s)":"BD region in apl_bdv PID%d: 0x%x (%s), set at the source",t->pid,player,bd_letters(player));}
 if(!decision_mode||decision_mode=='K'||!psr)return;
 want=region_for(decision_mode,decision_value,player);
 if(psr==want||ub_mem_write(t->pid,bdv_psr20_addr,&want,4))return;
 bd_psr=want;
 if(!t->state[1]){t->state[1]=1;ub_log(ID,UB_ALWAYS,"PSR20 in apl_bdv PID%d was %s; set to %s by the main loop",t->pid,bd_letters(psr),bd_letters(want));}
}

static void dvd_regions(unsigned mask,char *out,size_t size){
 size_t n=0;out[0]=0;
 for(unsigned r=1;r<=8;r++)if(mask&(1u<<(r-1)))n+=(size_t)snprintf(out+n,size>n?size-n:0,"%s%u",n?" ":"",r);
 if(!n)snprintf(out,size,"none");
}
/* ---- DVD region (libnavi_api.so in apl_meda_dvdv) ---- */
/* The disc's allowed regions from VIDEO_TS.IFO (0 = no DVD or not known),
 * and the player's own region mask (SPRM20 as the drive gave it). */
static unsigned dvd_allowed;
static uint16_t dvd_player;
static const unsigned char gate_stock[2]={0x02,0xd4},gate_free[2]={0x02,0xe0};
static const unsigned char source_stock[4]={0x1f,0x48,0x00,0x22};
/* The region mask the disc should see, 0 = the player's own. */
static unsigned dvd_mask(void){
 if(dvd_region>=DVD_R1&&dvd_region<=DVD_R8)return 1u<<(dvd_region-DVD_R1);
 if(dvd_region==DVD_AUTO&&dvd_allowed&&dvd_player&&!(dvd_allowed&dvd_player)){
  unsigned r;for(r=0;r<8;r++)if(dvd_allowed&(1u<<r))return 1u<<r;
 }
 return 0;
}
static int source_ours(const unsigned char *b){return b[0]&&!(b[0]&(b[0]-1))&&b[1]==0x20&&b[2]==0x70&&b[3]==0x47;}
/* One stopped transaction validates BOTH sites before modifying either.
 * -1 unknown bytes (permanent), -2 safely deferred, -3 quarantined. */
static int dvd_apply(int pid,uint32_t base,int quiet){
 unsigned mask=dvd_mask();unsigned char source[4],old_gate[2],old_source[4];uint16_t sprm,want;int rc,changed,source_changed;
 uint32_t gate=base+(NAVI_GATE-NAVI_LINK_BASE),src=base+(NAVI_REGION_SOURCE-NAVI_LINK_BASE);
 const unsigned char *gate_want=dvd_region==DVD_STOCK?gate_stock:gate_free;
 struct ub_code_patch patches[2];
 if(ub_mem_read(pid,gate,old_gate,2)||ub_mem_read(pid,src,old_source,4))return -2;
 if((memcmp(old_gate,gate_stock,2)&&memcmp(old_gate,gate_free,2))||
    (memcmp(old_source,source_stock,4)&&!source_ours(old_source))){
  if(!quiet)ub_log(ID,UB_ALWAYS,"apl_meda_dvdv PID%d: unexpected code in libnavi_api.so; DVD region left alone",pid);
  return -1;
 }
 if(mask){source[0]=(unsigned char)mask;source[1]=0x20;source[2]=0x70;source[3]=0x47;}
 else memcpy(source,source_stock,4);
 source_changed=memcmp(old_source,source,4)!=0;
 changed=memcmp(old_gate,gate_want,2)!=0||source_changed;
 if(changed){
  patches[0]=(struct ub_code_patch){gate,2,old_gate,0,gate_want};
  patches[1]=(struct ub_code_patch){src,4,old_source,0,source};
  rc=ub_patch_code(pid,patches,2);
  if(rc){
   if(rc==-2)return -3;
   if(rc==1)return -1;
   return -2;
  }
 }
 want=(uint16_t)(mask?mask:dvd_player);
 if(want&&!ub_mem_read(pid,base+(NAVI_SPRM20-NAVI_LINK_BASE),&sprm,2)&&sprm&&sprm!=want&&(mask||source_changed))
  if(!ub_mem_write(pid,base+(NAVI_SPRM20-NAVI_LINK_BASE),&want,2))changed=1;
 return changed;
}
static void dvd_describe(char *out,size_t size){
 char regions[32];unsigned mask=dvd_mask();
 if(dvd_region==DVD_STOCK){snprintf(out,size,"player default");return;}
 if(mask){dvd_regions(mask,regions,sizeof regions);snprintf(out,size,"no region error, region %s",regions);}
 else snprintf(out,size,"no region error, the player's own region");
}

/* apl_meda_dvdv. state: [0] 1 value logged, [1] last SPRM20, [2] libnavi
 * 1 ok / -1 rejected, [3] last drive-check flag + 1, [4] last applied choice
 * + 1 (with the mask in [5]), -1 unexpected code */
static void dvd_step(struct ub_part *p,struct ub_target *t){
 static char checked_path[600];static int checked;
 char path[600],regions[32],text[64];uint32_t base;uint16_t v;unsigned char passed,source[4];(void)p;
 if(t->state[2]<0)return;
 if(!dvd_guard_stop())return; /* the guard retains ownership until reaped */
 base=ub_map_base(t->pid,"/libnavi_api.so",path,sizeof path);if(!base)return;
 if(!t->state[2]){
  char h[65];
  if(!checked||strcmp(path,checked_path)){
   checked=ub_hash_file(path,h)?-1:ub_firmware_match(h,NAVI_SHA256)?1:-1;
   snprintf(checked_path,sizeof checked_path,"%s",path);
  }
  t->state[2]=checked;
  if(checked<0){ub_log(ID,UB_ALWAYS,"%s differs from 1.82 and 1.85; DVD region not read",path);return;}
 }
 /* the player's own region: SPRM20 while the region source is the stock code */
 if(!ub_mem_read(t->pid,base+(NAVI_REGION_SOURCE-NAVI_LINK_BASE),source,4)&&!memcmp(source,source_stock,4)&&
    !ub_mem_read(t->pid,base+(NAVI_SPRM20-NAVI_LINK_BASE),&v,2)&&v)dvd_player=v;
 if(t->state[4]>=0&&(t->state[4]!=dvd_region+1||t->state[5]!=(int)dvd_mask())){
  int r=dvd_apply(t->pid,base,0);
  if(r==-2)return; /* busy PC/stop failure: retry without caching success */
  if(r<0)t->state[4]=-1;
  else{
   t->state[4]=dvd_region+1;t->state[5]=(int)dvd_mask();
   if(r>0){dvd_describe(text,sizeof text);ub_log(ID,UB_ALWAYS,"DVD region in apl_meda_dvdv PID%d: %s",t->pid,text);}
  }
 }
 if(!ub_mem_read(t->pid,base+(NAVI_SPRM20-NAVI_LINK_BASE),&v,2)&&!(t->state[0]&&(uint16_t)t->state[1]==v)){
  t->state[0]=1;t->state[1]=v;dvd_regions(v,regions,sizeof regions);
  ub_log(ID,UB_INFO,"DVD player region mask SPRM20 in apl_meda_dvdv PID%d: 0x%02x (region %s)",t->pid,v,regions);
 }
 /* 1 once the drive has reported the disc's region as matching. It stays 0
  * for a foreign disc the drive refuses (DVD_ERR_REGION). */
 if(!ub_mem_read(t->pid,base+(NAVI_REGION_PASSED-NAVI_LINK_BASE),&passed,1)&&t->state[3]!=passed+1){
  t->state[3]=passed+1;
  ub_log(ID,UB_INFO,"DVD drive region check in PID%d: %s",t->pid,passed?"passed":"not passed (yet)");
 }
}

/* Disc files ---------------------------------------------------------------- */
#define be32 ub_bd_be32
#define be16 ub_bd_be16
static int disc_path(char *out,size_t size,const char *root,const char *tail){
 int n=snprintf(out,size,"%s%s",root,tail);return n<0||(size_t)n>=size?-1:0;
}
static unsigned char *load(const char *path,size_t *size){
 struct ub_stat st;unsigned char *b;int fd=open(path,O_RDONLY);size_t got=0;ssize_t n;
 if(fd<0)return 0;
 if(ub_fstat(fd,&st)||st.size<=0||st.size>DISC_FILE_LIMIT){close(fd);return 0;}
 b=malloc((size_t)st.size);if(!b){close(fd);return 0;}
 while(got<(size_t)st.size&&(n=read(fd,b+got,(size_t)st.size-got))>0)got+=(size_t)n;
 close(fd);if(got!=(size_t)st.size){free(b);return 0;}
 *size=got;return b;
}
static void object_text(const unsigned char *o,char *out,size_t size){
 unsigned type=o[0]>>6;
 if(type==1)snprintf(out,size,"HDMV object %u",be16(o+6));
 else if(type==2)snprintf(out,size,"BD-J %.5s",(const char *)o+6);
 else snprintf(out,size,"type %u",type);
}
/* Returns 1 when First Play is a BD-J title. */
static int bd_index(const char *root){
 char path[512],first[32],top[32];size_t size;unsigned char *b;uint32_t at;unsigned titles,hdmv=0,bdj=0,i;
 if(disc_path(path,sizeof path,root,"/BDMV/index.bdmv"))return 0;
 if(!(b=load(path,&size))){ub_log(ID,UB_INFO,"cannot read %s",path);return 0;}
 if(size<12||memcmp(b,"INDX",4)){ub_log(ID,UB_INFO,"%s: not an index.bdmv",path);free(b);return 0;}
 at=be32(b+8);
 if(at>size||size-at<4+12+12+2){ub_log(ID,UB_INFO,"%s: index out of range",path);free(b);return 0;}
 object_text(b+at+4,first,sizeof first);object_text(b+at+16,top,sizeof top);
 titles=be16(b+at+28);
 for(i=0;i<titles&&at+30+12*(i+1)<=size;i++){unsigned type=b[at+30+12*i]>>6;if(type==1)hdmv++;else if(type==2)bdj++;}
 ub_log(ID,UB_INFO,"BD %.4s: First Play %s, Top Menu %s, %u titles (%u HDMV, %u BD-J)",b+4,first,top,titles,hdmv,bdj);
 i=(b[at+4]>>6)==2;
 free(b);
 return (int)i;
}
static void operand(int immediate,uint32_t v,char *out,size_t size){
 if(immediate)snprintf(out,size,"0x%x",v);
 else if(v&0x80000000u)snprintf(out,size,"PSR%u",v&0x7f);
 else snprintf(out,size,"r%u",v&0xfff);
}
static void command_text(const unsigned char *c,char *out,size_t size){
 static const char *const cmp[]={"?","BC","EQ","NE","GE","GT","LE","LT"};
 static const char *const set[]={"?","MOVE","SWAP","ADD","SUB","MUL","DIV","MOD","RND","AND","OR","XOR","BITSET","BITCLR","SHL","SHR"};
 unsigned grp=(c[0]>>3)&3,sub=c[0]&7,imm1=c[1]>>7,imm2=(c[1]>>6)&1,branch=c[1]&15,compare=c[2]&15,setop=c[3]&31;
 char a[24],b[24];
 operand(imm1,be32(c+4),a,sizeof a);operand(imm2,be32(c+8),b,sizeof b);
 if(grp==1)snprintf(out,size,"%s %s, %s",compare<8?cmp[compare]:"CMP?",a,b);
 else if(grp==2&&sub==0)snprintf(out,size,"%s %s, %s",setop<16?set[setop]:"SET?",a,b);
 else if(grp==2)snprintf(out,size,"SETSYSTEM.%u %s, %s",setop,a,b);
 else snprintf(out,size,"%s.%u %s, %s",sub==0?"GOTO":sub==1?"JUMP":sub==2?"PLAY":"BRANCH",branch,a,b);
}
static int reads_psr20(const unsigned char *c){
 unsigned imm1=c[1]>>7,imm2=(c[1]>>6)&1;uint32_t d=be32(c+4),s=be32(c+8);
 return (!imm1&&d==0x80000014u)||(!imm2&&s==0x80000014u);
}
/* Region bits an HDMV program tests PSR20 against: a register loaded from
 * PSR20 (MOVE rX, PSR20, or copied on with MOVE rY, rX), or PSR20 itself,
 * compared (BC/EQ/NE) or ANDed with an immediate of 1..7. A heuristic: the
 * values a disc compares its player region with are the regions it plays in. */
#define hdmv_mask_of_object ub_bd_hdmv_mask
/* Logs the PSR20 commands and returns the region mask (0 = no HDMV check). */
static unsigned bd_movie_objects(const char *root){
 char path[512],text[80];size_t size,at;unsigned char *b;unsigned objects,o,logged=0,found=0,mask=0;
 if(disc_path(path,sizeof path,root,"/BDMV/MovieObject.bdmv"))return 0;
 if(!(b=load(path,&size))){ub_log(ID,UB_INFO,"cannot read %s",path);return 0;}
 if(size<50||memcmp(b,"MOBJ",4)){ub_log(ID,UB_INFO,"%s: not a MovieObject.bdmv",path);free(b);return 0;}
 objects=be16(b+48);at=50;
 for(o=0;o<objects;o++){
  if(at+4>size){mask=0;break;}
  unsigned cmds=be16(b+at+2),i;at+=4;
  if(size-at<12*(size_t)cmds){mask=0;break;}
  for(i=0;i<cmds;i++){
   const unsigned char *c=b+at+12*i;
   if(!reads_psr20(c))continue;
   found++;
   /* The reading command and the two after it show the mask a disc tests. */
   for(unsigned k=i;k<cmds&&k<i+3&&logged<HDMV_LOG_LIMIT;k++,logged++){
    command_text(b+at+12*k,text,sizeof text);
    ub_log(ID,UB_INFO,"HDMV object %u cmd %u: %s",o,k,text);
   }
  }
  mask|=hdmv_mask_of_object(b+at,cmds);
  at+=12*(size_t)cmds;
 }
 if(mask){char letters[8];mask_letters(mask,letters,sizeof letters);ub_log(ID,UB_INFO,"HDMV: %u movie objects, %u commands read PSR20; tested regions: %s",objects,found,letters);}
 else ub_log(ID,UB_INFO,"HDMV: %u movie objects, %u commands read PSR20%s",objects,found,found?" (no region value found)":" (region not checked in HDMV)");
 free(b);
 return mask;
}
/* The decision for a BD: 0 stock (leave the player alone), 'F' fixed
 * value, 'M' mask the disc's HDMV program tests, 'K' keep the player's. */
static char bd_decide(unsigned hdmv_mask,int first_play_bdj,unsigned *value,const char **why){
 *value=0;
 if(bd_region==BD_STOCK){*why="bd_region=stock";return 0;}
 if(bd_region!=BD_AUTO){*value=bd_region_value[bd_region];*why="bd_region setting";return 'F';}
 if(hdmv_mask){*value=hdmv_mask;*why="auto: the disc's HDMV program tests these regions";return 'M';}
 *why=first_play_bdj?"auto: the disc starts in BD-J, whose region check is not analysed yet":"auto: no region check found";
 return 'K';
}
/* The decision for a newly inserted BD (menu and main loop). */
static void decide_bd(unsigned hdmv_mask,int first_play_bdj){
 unsigned value;const char *why;char mode=bd_decide(hdmv_mask,first_play_bdj,&value,&why);
 if(mode)decide_set(mode,value,why);
}

/* ---- BD region at the source ----
 * The watcher, a child running while bd_region is not stock, catches each
 * new apl_bdv: inotify reports the program file being opened by its exec,
 * and /proc is also checked every 20 ms in case that report does not come.
 * It stops the process (ptrace, all threads), checks that its region slot
 * is still 0 (an early-start heuristic), sets the
 * three movs of FUN_0004a5b8 so that the drive's answer becomes the chosen
 * region, and lets it go on. Auto disc analysis is prepared before stopping.
 * A zero slot cannot rule out a value already computed in a register. Afterwards it checks PSR20; only if it was too
 * late to change the source does it set PSR20 instead, as the old guard did.
 * Every catch is logged with how it went. */
#define WATCH_POLL_MS 20
#define WATCH_BURST_MS 500     /* checks every 1 ms after an exec report */
#define WATCH_YOUNG_MS 3000    /* a process is looked at this long */
#define WATCH_DISC_WAIT_MS 1000
/* Tests may add a disc root to look at first. */
static const char *bd_test_root;
/* The byte each movs gets for decision (mode,value): the region a player
 * whose drive says A, B or C keeps. */
static void bd_source_bytes(char mode,unsigned value,unsigned char out[3]){
 unsigned i;for(i=0;i<3;i++)out[i]=(unsigned char)region_for(mode,value,bdv_source_stock[i]);
}
/* Writes the three immediates through mem (an open /proc/<pid>/mem): 1
 * changed, 0 already so, -1 one of the instructions is not "movs r3,#imm"
 * (nothing is written then). */
static int bd_source_patch(int mem,const unsigned char want[3]){
 unsigned char now[3][2],verify[2];unsigned i,written=0;int changed=0;
 for(i=0;i<3;i++)if(pread(mem,now[i],2,(off_t)bdv_source[i])!=2||now[i][1]!=0x23||
    !(now[i][0]==1||now[i][0]==2||now[i][0]==4||now[i][0]==7))return -1;
 /* Each site remains the same single 16-bit movs instruction; no instruction
  * boundary changes. All threads are already stopped by ub_while_stopped. */
 for(i=0;i<3;i++)if(now[i][0]!=want[i]){
  written=i+1;
  if(pwrite(mem,&want[i],1,(off_t)bdv_source[i])!=1||pread(mem,verify,2,(off_t)bdv_source[i])!=2||verify[0]!=want[i]||verify[1]!=0x23)goto rollback;
  changed=1;
 }
 return changed;
 rollback:
 for(i=written;i>0;i--)if(pwrite(mem,now[i-1],2,(off_t)bdv_source[i-1])!=2||
     pread(mem,verify,2,(off_t)bdv_source[i-1])!=2||memcmp(verify,now[i-1],2))return -2;
 return -1;
}
/* The mounted BD: /mnt/odd or a UDF/ISO 9660 mount with BDMV/index.bdmv. */
static int bd_root(char *out,size_t size){
 static char text[16384];char path[512],*buf,*save,dev[200],dir[200],type[40];struct ub_stat st;
 if(bd_test_root&&!disc_path(path,sizeof path,bd_test_root,"/BDMV/index.bdmv")&&!ub_stat(path,&st)){snprintf(out,size,"%s",bd_test_root);return 1;}
 if(!disc_path(path,sizeof path,"/mnt/odd","/BDMV/index.bdmv")&&!ub_stat(path,&st)){snprintf(out,size,"/mnt/odd");return 1;}
 if(ub_read_file("/proc/mounts",text,sizeof text)<0)return 0;
 for(buf=strtok_r(text,"\n",&save);buf;buf=strtok_r(0,"\n",&save)){
  if(sscanf(buf,"%199s %199s %39s",dev,dir,type)!=3||(strcmp(type,"udf")&&strcmp(type,"iso9660")))continue;
  if(!disc_path(path,sizeof path,dir,"/BDMV/index.bdmv")&&!ub_stat(path,&st)){snprintf(out,size,"%s",dir);return 1;}
 }
 return 0;
}
static volatile sig_atomic_t watcher_cancel;
static void watcher_cancelled(int sig){(void)sig;watcher_cancel=1;}
struct bd_catch {
 char mode; unsigned value; const char *why;
 uint32_t slot; int early,patched,no_disc,waited_ms; unsigned char bytes[3];
};
/* Only bounded memory checks and writes while stopped. Disc I/O is prepared
 * beforehand; a zero slot remains a best-effort early indicator. */
static int bd_catch_stopped(int pid,int mem,void *arg){
 struct bd_catch *c=arg;(void)pid;
 if(pread(mem,&c->slot,4,(off_t)bdv_region_addr)!=4)return -1;
 c->early=c->slot==0;
 if(!c->early||!c->mode||c->mode=='K')return 0;
 bd_source_bytes(c->mode,c->value,c->bytes);
 c->patched=bd_source_patch(mem,c->bytes);
 return c->patched==-2?-2:0; /* unverified rollback keeps the target stopped */
}
/* One new apl_bdv: change the source, then check (or, if too late, set)
 * PSR20 for the life of the process or GUARD_SECONDS. */
static void watcher_handle(int pid,unsigned long long started_ms,const char *how){
 struct bd_catch c;int rc,told=0;unsigned long long after,until;uint32_t slot,psr,want;
 memset(&c,0,sizeof c);
 if(bd_region==BD_AUTO){
  char root[200];
  while(!bd_root(root,sizeof root)){
   if(watcher_cancel)return;
   if(c.waited_ms>=WATCH_DISC_WAIT_MS){ub_log(ID,UB_ALWAYS,"BD region: no Blu-ray files within %d ms of apl_bdv PID%d starting; auto decides on the main loop",WATCH_DISC_WAIT_MS,pid);return;}
   ub_sleep_ms(10);c.waited_ms+=10;
  }
  {int bdj=bd_index(root);unsigned mask=bd_movie_objects(root);c.mode=bd_decide(mask,bdj,&c.value,&c.why);}
 }else c.mode=bd_decide(0,0,&c.value,&c.why);
 if(watcher_cancel)return;
 rc=ub_while_stopped(pid,bd_catch_stopped,&c);
 if(rc==-2){ub_log(ID,UB_ALWAYS,"BD source rollback failed; PID%d remains quarantined",pid);return;}
 after=ub_now_ms()-started_ms;
 if(rc<0){
  c.mode=bd_region==BD_AUTO?0:bd_decide(0,0,&c.value,&c.why);
  ub_log(ID,UB_ALWAYS,"BD region: apl_bdv PID%d could not be stopped; %s",pid,c.mode?"PSR20 is set when the engine fills it":"auto decides on the main loop");
 }else if(c.no_disc){ub_log(ID,UB_ALWAYS,"BD region: no Blu-ray files within %d ms of apl_bdv PID%d starting; auto decides on the main loop",WATCH_DISC_WAIT_MS,pid);return;}
 else if(!c.mode||c.mode=='K'){ub_log(ID,UB_INFO,"BD region for apl_bdv PID%d: the player's own (%s)",pid,c.why);return;}
 else if(!c.early)ub_log(ID,UB_ALWAYS,"BD region: apl_bdv PID%d %s and stopped %llu ms later had already asked the drive (%s); PSR20 is set instead",pid,how,after,bd_letters(c.slot));
 else if(c.patched<0)ub_log(ID,UB_ALWAYS,"BD region: unexpected code in apl_bdv PID%d; PSR20 is set instead",pid);
 else ub_log(ID,UB_ALWAYS,"BD region set at the source in apl_bdv PID%d (%s, stopped %llu ms later, before it asked the drive%s): drive A/B/C -> %s/%s/%s (%s)",
             pid,how,after,c.waited_ms?", disc files prepared before stopping":"",bd_letters(c.bytes[0]),bd_letters(c.bytes[1]),bd_letters(c.bytes[2]),c.why);
 if(!c.mode)return;
 for(until=ub_now_ms()+GUARD_SECONDS*1000ull;!watcher_cancel&&ub_now_ms()<until;ub_sleep_ms(GUARD_POLL_MS)){
  if(kill(pid,0)||ub_mem_read(pid,bdv_region_addr,&slot,4)||ub_mem_read(pid,bdv_psr20_addr,&psr,4))break; /* this disc's apl_bdv ended */
  if(!psr)continue;
  want=region_for(c.mode,c.value,slot);
  if(psr!=want){
   if(ub_mem_write(pid,bdv_psr20_addr,&want,4)){ub_log(ID,UB_ALWAYS,"cannot set PSR20 in apl_bdv PID%d",pid);break;}
   ub_log(ID,UB_ALWAYS,"PSR20 in apl_bdv PID%d was %s; set to %s",pid,bd_letters(psr),bd_letters(want));
  }else if(!told){
   told=1;ub_log(ID,UB_ALWAYS,"BD disc in apl_bdv PID%d sees region %s (PSR20)",pid,bd_letters(psr));
   if(c.early&&c.patched>=0)break;   /* set at the source: the engine keeps it */
  }
 }
}
/* New processes, each looked at for WATCH_YOUNG_MS: the pid of a new
 * apl_bdv (the verified file), or 0. A process is new when it was not in
 * the previous scan; those already there at the first scan are left to the
 * main loop, so a disc that is playing keeps the decision it started with. */
#define WATCH_MAX_PIDS 2048
#define WATCH_YOUNG 32
static int watch_prev[WATCH_MAX_PIDS],watch_prevn=-1;
static struct { int pid,done; unsigned long long first; } watch_young[WATCH_YOUNG];
static int pid_order(const void *a,const void *b){int x=*(const int *)a,y=*(const int *)b;return (x>y)-(x<y);}
static int pid_in(const int *list,int n,int pid){return n>0&&bsearch(&pid,list,(size_t)n,sizeof *list,pid_order)!=0;}
static int watch_scan(unsigned long long now,unsigned long long *first){
 static struct ub_dir d;static int cur[WATCH_MAX_PIDS];const char *e;int n=0,k,found=0;unsigned i;
 if(ub_dir_open(&d,"/proc"))return 0;
 while((e=ub_dir_next(&d))&&n<WATCH_MAX_PIDS){int pid=atoi(e);if(pid>0)cur[n++]=pid;}
 ub_dir_close(&d);
 qsort(cur,(size_t)n,sizeof *cur,pid_order);
 if(watch_prevn>=0)for(k=0;k<n;k++){
  if(pid_in(watch_prev,watch_prevn,cur[k]))continue;
  for(i=0;i<WATCH_YOUNG&&watch_young[i].pid;i++){}
  if(i==WATCH_YOUNG)break;
  watch_young[i].pid=cur[k];watch_young[i].first=now;watch_young[i].done=0;
 }
 memcpy(watch_prev,cur,(size_t)n*sizeof *cur);watch_prevn=n;
 for(i=0;i<WATCH_YOUNG;i++){
  char path[48],comm[32];int pid=watch_young[i].pid,fd;ssize_t len;
  if(!pid)continue;
  if(!pid_in(cur,n,pid)||now-watch_young[i].first>WATCH_YOUNG_MS){watch_young[i].pid=0;continue;}
  if(found||watch_young[i].done)continue;
  snprintf(path,sizeof path,"/proc/%d/comm",pid);fd=open(path,O_RDONLY);if(fd<0)continue;
  len=read(fd,comm,sizeof comm-1);close(fd);if(len<=0)continue;
  comm[len]=0;comm[strcspn(comm,"\n")]=0;
  if(!strcmp(comm,"apl_bdv")&&bdv_identical(pid)){watch_young[i].done=1;found=pid;*first=watch_young[i].first;}
 }
 return found;
}
static void watcher_run(void){
 int in=inotify_init1(IN_NONBLOCK|IN_CLOEXEC),wd=in>=0?inotify_add_watch(in,BDV_PATH,IN_OPEN):-1;
 unsigned long long burst=0,signalled=0,first;char buf[512];
 ub_log(ID,UB_INFO,"BD region watcher: %s",wd>=0?"apl_bdv starts are reported (inotify); /proc is also checked every 20 ms":"/proc is checked every 20 ms");
 while(!watcher_cancel){
  unsigned long long now=ub_now_ms();int pid,wait=now<burst?1:WATCH_POLL_MS;
  if(wd>=0){
   struct pollfd pf={in,POLLIN,0};
   if(poll(&pf,1,wait)>0&&(pf.revents&POLLIN)){while(read(in,buf,sizeof buf)>0){}signalled=ub_now_ms();burst=signalled+WATCH_BURST_MS;}
  }else ub_sleep_ms((unsigned)wait);
  now=ub_now_ms();
  if((pid=watch_scan(now,&first))){
   int reported=signalled&&now-signalled<=WATCH_BURST_MS;
   watcher_handle(pid,reported?signalled:first,reported?"reported at its exec":"found by the /proc check");
   burst=0;signalled=0;
  }
 }
}
static pid_t watcher_pid;
static unsigned watcher_restarts;
static int watcher_stop(void){
 if(watcher_pid>0){
  unsigned long long until=ub_now_ms()+1000;int status;pid_t r;
  do{r=waitpid(watcher_pid,&status,WNOHANG);}while(r<0&&errno==EINTR);
  if(!r){
   kill(watcher_pid,SIGTERM);
   do{
    r=waitpid(watcher_pid,&status,WNOHANG);
    if(r>0||(r<0&&errno==ECHILD))break;
    if(r<0&&errno!=EINTR)return 0;
    if(ub_now_ms()>=until)return 0;
    ub_sleep_ms(1);
   }while(1);
  }else if(r<0&&errno!=ECHILD)return 0;
 }
 watcher_pid=0;return 1;
}
static void watcher_start(void){
 pid_t pid;sigset_t term,old;
 if(!watcher_stop())return;
 /* Never race a pending/installed native launcher. Even unavailable assets
  * use only the PSR20 fallback once a hook might exist. Source polling is
  * retained solely when exact verification or installation failed, or the
  * long-lived launcher process ended and its part tripped. */
 if(bd_launch_part&&!bd_launch_part->tripped&&bd_launch_part->target.verified!=-1&&!bd_launch_part->target.failed)return;
 if(bd_region==BD_STOCK||!bdv_file.ok)return;
 sigemptyset(&term);sigaddset(&term,SIGTERM);
 if(sigprocmask(SIG_BLOCK,&term,&old))return;
 pid=fork();
 if(!pid){
  struct sigaction action;pid_t parent=getppid();
  memset(&action,0,sizeof action);action.sa_handler=watcher_cancelled;sigemptyset(&action.sa_mask);
  watcher_cancel=0;sigaction(SIGTERM,&action,0);prctl(PR_SET_PDEATHSIG,SIGTERM);
  sigdelset(&old,SIGTERM);sigprocmask(SIG_SETMASK,&old,0);
  if(getppid()!=parent)_exit(0);
  watcher_run();_exit(0);
 }
 sigprocmask(SIG_SETMASK,&old,0);
 watcher_pid=pid>0?pid:0;
 if(pid<0)ub_log(ID,UB_ALWAYS,"cannot start the BD region watcher; the BD region is set from the main loop only");
}

/* Publish native-loader assets only in RAM. Atomic policy replacement makes
 * each future fork use one complete choice; a failed update removes stale
 * policy rather than silently launching with the previous selection. */
static int bd_launch_blob(const char *path,const void *data,size_t size){
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
static int bd_launch_publish(void){
 struct ub_bd_launch_config c={UB_BD_LAUNCH_MAGIC,1,0,0};struct ub_stat st;char hash[65];
 bd_launch_ready=0;
 if(!bdv_file.ok)return -1;
 if(mkdir(UB_BD_RAM_DIR,0700)&&errno!=EEXIST)goto fail;
 if(ub_stat(UB_BD_RAM_DIR,&st)||!S_ISDIR(st.mode))goto fail;
 if(ub_hash_file(UB_BD_PRELOAD,hash)||strcmp(hash,ub_asset_region_bd_preload_sha256)){
  if(bd_launch_blob(UB_BD_PRELOAD,ub_asset_region_bd_preload,ub_asset_region_bd_preload_size)||
     ub_hash_file(UB_BD_PRELOAD,hash)||strcmp(hash,ub_asset_region_bd_preload_sha256))goto fail;
 }
 c.policy=bd_region==BD_AUTO?-1:(int32_t)bd_region_value[bd_region];
 if(bd_launch_blob(UB_BD_LAUNCH_CONFIG,&c,sizeof c))goto fail;
 bd_launch_ready=1;return 0;
 fail:
 unlink(UB_BD_LAUNCH_CONFIG);
 ub_log(ID,UB_ALWAYS,"BD pre-main policy/assets unavailable; native launch and best-effort fallback retained");return -1;
}
static void launch_step(struct ub_part *p,struct ub_target *t){
 bd_launch_part=p;
 if(!bd_launch_ready)return;
 if(!watcher_stop())return;
 if(bd_launch_armed)return;
 bd_launch_armed=1;bd_launch_pid=t->pid;
 ub_log(ID,UB_ALWAYS,"BD pre-main launcher armed in apl_medm PID%d; applies to subsequent BD starts, current playback unchanged",t->pid);
}
static void launch_prepare(struct ub_part *p,struct ub_target *t){
 bd_launch_part=p;
 /* prepare may defer installation through retry: the core checks this before
  * publishing any branch. Reap the previous source writer first, including
  * recovery from a failed/mismatched earlier media-manager process. */
 if(!watcher_stop())t->retry=1;
}

/* The region value the BD in the drive sees, 0 if none or not known yet. */
static uint32_t disc_sees(void){
 if(!bd_present)return 0;
 if(bd_psr==1||bd_psr==2||bd_psr==4||bd_psr==7)return bd_psr;
 if(!bd_player)return 0;
 return decision_mode?region_for(decision_mode,decision_value,bd_player):bd_player;
}
/* Status for the menu, rewritten only when it changes. */
static unsigned menu_seq;
static char menu_message[96]="";
static void menu_status(void){
 static char last[160];char line[160];int n,fd;
 n=snprintf(line,sizeof line,"%u %s %u %u %d %s\n",menu_seq,bd_region_names[bd_region],(unsigned)disc_sees(),(unsigned)bd_player,bd_auto,menu_message);
 if(n<0||n>=(int)sizeof line||!strcmp(line,last))return;
 fd=open(UB_REGION_STATUS ".tmp",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return;
 if(write(fd,line,(size_t)n)!=n){close(fd);unlink(UB_REGION_STATUS ".tmp");return;}
 if(close(fd)||rename(UB_REGION_STATUS ".tmp",UB_REGION_STATUS)){unlink(UB_REGION_STATUS ".tmp");return;}
 memcpy(last,line,sizeof last);
}
/* The same for dvd_region (UB_DVD_REQUEST / UB_DVD_STATUS); a change reaches
 * a running apl_meda_dvdv through dvd_step within a second. */
static unsigned dvd_seq;
static char dvd_message[96]="";
static void dvd_menu_status(void){
 static char last[160];char line[160];int n,fd;
 n=snprintf(line,sizeof line,"%u %s %s\n",dvd_seq,dvd_region_names[dvd_region],dvd_message);
 if(n<0||n>=(int)sizeof line||!strcmp(line,last))return;
 fd=open(UB_DVD_STATUS ".tmp",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return;
 if(write(fd,line,(size_t)n)!=n){close(fd);unlink(UB_DVD_STATUS ".tmp");return;}
 if(close(fd)||rename(UB_DVD_STATUS ".tmp",UB_DVD_STATUS)){unlink(UB_DVD_STATUS ".tmp");return;}
 memcpy(last,line,sizeof last);
}
static void dvd_menu_request(void){
 char buf[64],choice[16],reply[UB_ACTION_PATH],line[160];unsigned seq;int rc,n;
 if(!ub_action_receive(UB_DVD_REQUEST,buf,sizeof buf,reply,sizeof reply))return;
 if(sscanf(buf,"set %15s %u",choice,&seq)!=2){ub_log(ID,UB_INFO,"ignored DVD menu request");return;}
 if(!dvd_guard_stop()){
  dvd_seq=seq;snprintf(dvd_message,sizeof dvd_message,"busy stopping previous operation; try again");
  n=snprintf(line,sizeof line,"%u %s %s\n",dvd_seq,dvd_region_names[dvd_region],dvd_message);
  if(n>0&&(size_t)n<sizeof line)ub_action_reply(reply,line,(size_t)n);
  return;
 }
 rc=ub_setting_set(ID,"dvd_region",choice);
 dvd_seq=seq;
 if(rc<0)snprintf(dvd_message,sizeof dvd_message,"unknown choice %s",choice);
 else if(rc>0)snprintf(dvd_message,sizeof dvd_message,"not saved to USB (%s)",ub_stick_error(rc));
 else snprintf(dvd_message,sizeof dvd_message,"saved");
 n=snprintf(line,sizeof line,"%u %s %s\n",dvd_seq,dvd_region_names[dvd_region],dvd_message);
 if(n>0&&(size_t)n<sizeof line)ub_action_reply(reply,line,(size_t)n);
 ub_log(ID,UB_ALWAYS,"DVD region chosen in the menu: %s (%s)",dvd_region_names[dvd_region],dvd_message);
}
/* Each client receives its own acknowledgement; global status is observational. */
static void menu_request(void){
 char buf[64],choice[16],reply[UB_ACTION_PATH],line[200];unsigned seq;int rc,n;
 if(!ub_action_receive(UB_REGION_REQUEST,buf,sizeof buf,reply,sizeof reply))return;
 if(sscanf(buf,"set %15s %u",choice,&seq)!=2){ub_log(ID,UB_INFO,"ignored menu request");return;}
 menu_seq=seq;
 if(!bd_auto&&!strcmp(choice,"auto")){
  snprintf(menu_message,sizeof menu_message,"auto is off (bd_auto=0)");
  ub_log(ID,UB_ALWAYS,"Blu-ray region auto refused: bd_auto=0 in modules.cfg");
 }else{
  int before=bd_region;
  if(!watcher_stop()){
   snprintf(menu_message,sizeof menu_message,"busy stopping previous operation; try again");
  }else{
   rc=ub_setting_set(ID,"bd_region",choice);
   if(rc>=0&&(bd_region!=before||!bd_launch_ready)){bd_launch_publish();watcher_start();}
  if(rc<0)snprintf(menu_message,sizeof menu_message,"unknown choice %s",choice);
  else if(rc>0)snprintf(menu_message,sizeof menu_message,"not saved to USB (%s)",ub_stick_error(rc));
  else snprintf(menu_message,sizeof menu_message,"saved");
  }
  ub_log(ID,UB_ALWAYS,"Blu-ray region chosen in the menu: %s (%s); it applies from the next disc on",bd_region_names[bd_region],menu_message);
 }
 n=snprintf(line,sizeof line,"%u %s %u %u %d %s\n",menu_seq,bd_region_names[bd_region],(unsigned)disc_sees(),(unsigned)bd_player,bd_auto,menu_message);
 if(n>0&&(size_t)n<sizeof line)ub_action_reply(reply,line,(size_t)n);
}
static unsigned count_dir(const char *path){
 static struct ub_dir d;const char *e;unsigned n=0;
 if(ub_dir_open(&d,path))return 0;
 while((e=ub_dir_next(&d)))if(e[0]!='.')n++;
 ub_dir_close(&d);return n;
}
static void dvd_ifo(const char *root){
 char path[512],regions[32];size_t size;unsigned char *b;unsigned mask;
 if(disc_path(path,sizeof path,root,"/VIDEO_TS/VIDEO_TS.IFO"))return;
 if(!(b=load(path,&size))){ub_log(ID,UB_INFO,"cannot read %s",path);return;}
 if(size<0x24||memcmp(b,"DVDVIDEO-VMG",12)){ub_log(ID,UB_INFO,"%s: not a VMG IFO",path);free(b);return;}
 mask=b[0x23];dvd_regions(~mask&0xffu,regions,sizeof regions);
 dvd_allowed=~mask&0xffu;
 ub_log(ID,UB_INFO,"DVD: prohibited-region mask 0x%02x, playable in region %s",mask,regions);
 free(b);
}
/* A disc is identified by its index file's size, time and inode; it is
 * analysed once per insertion. The top bit marks a BD. */
#define DISC_BD (1ull<<63)
static void disc_root(const char *root,unsigned long long *seen){
 char path[512];struct ub_stat st;unsigned long long id;
 if(disc_path(path,sizeof path,root,"/BDMV/index.bdmv"))return;
 if(!ub_stat(path,&st)){
  id=(((unsigned long long)st.size<<40)^((unsigned long long)st.mtime<<8)^st.ino)|DISC_BD;
  if(id==*seen)return;
  *seen=id;bd_present=1;ub_log(ID,UB_INFO,"Blu-ray found at %s",root);
  {int bdj=bd_index(root);unsigned mask=bd_movie_objects(root);decide_bd(mask,bdj);}
  if(disc_path(path,sizeof path,root,"/BDMV/JAR"))return;
  ub_log(ID,UB_INFO,"BD-J: %u entries in BDMV/JAR",count_dir(path));
  return;
 }
 if(disc_path(path,sizeof path,root,"/VIDEO_TS/VIDEO_TS.IFO"))return;
 if(!ub_stat(path,&st)){
  id=((((unsigned long long)st.size<<40)^((unsigned long long)st.mtime<<8)^st.ino)&~DISC_BD)|1;
  if(id==*seen)return;
  *seen=id;ub_log(ID,UB_INFO,"DVD found at %s",root);dvd_ifo(root);dvd_guard_start();
  return;
 }
 if(*seen&DISC_BD){decide_clear();bd_present=0;} /* the BD that was here is gone */
 else if(*seen){dvd_guard_stop();dvd_allowed=0;}  /* the DVD */
 *seen=0;
}
#define DISC_ROOTS 4
static void disc_step(struct ub_part *p,struct ub_target *t){
 static char roots[DISC_ROOTS][200];static unsigned long long seen[DISC_ROOTS];static unsigned count,rescan;
 static unsigned launch_retry;
 (void)p;(void)t;
 if(bd_launch_armed&&bd_launch_part&&
    (bd_launch_part->tripped||bd_launch_part->target.pid!=bd_launch_pid||!bd_launch_part->target.installed)){
  bd_launch_armed=0;
  ub_log(ID,UB_ALWAYS,"BD pre-main launcher no longer active; best-effort watcher restored");watcher_start();
 }
 if(!watcher_pid&&bd_launch_part&&
    (bd_launch_part->tripped||bd_launch_part->target.failed||bd_launch_part->target.verified==-1))watcher_start();
 if(!bd_launch_ready&&bdv_file.ok){
  if(!launch_retry){bd_launch_publish();launch_retry=9;}
  else launch_retry--;
 }
 if(watcher_pid>0&&waitpid(watcher_pid,0,WNOHANG)==watcher_pid){
  watcher_pid=0;
  if(watcher_restarts++<5){ub_log(ID,UB_ALWAYS,"BD region watcher ended; started again");watcher_start();}
 }
 if(dvd_guard_pid>0&&waitpid(dvd_guard_pid,0,WNOHANG)==dvd_guard_pid)dvd_guard_pid=0;
 /* Try /mnt/odd and every UDF or ISO 9660 mount, refreshed every 10 s (the
  * log module records the mount table itself). */
 if(!rescan--){
  static char text[16384];char *buf,*save,dev[200],dir[200],type[40];
  rescan=9;count=0;
  snprintf(roots[count++],sizeof roots[0],"/mnt/odd");
  if(ub_read_file("/proc/mounts",text,sizeof text)>=0){
   for(buf=strtok_r(text,"\n",&save);buf;buf=strtok_r(0,"\n",&save)){
    if(sscanf(buf,"%199s %199s %39s",dev,dir,type)!=3)continue;
    if((!strcmp(type,"udf")||!strcmp(type,"iso9660"))&&count<DISC_ROOTS&&strcmp(dir,"/mnt/odd"))snprintf(roots[count++],sizeof roots[0],"%s",dir);
   }
  }
 }
 for(unsigned i=0;i<count;i++)disc_root(roots[i],&seen[i]);
 if(bdv_file.ok){menu_request();menu_status();} /* else no BD region: the menu says "not available" */
 dvd_menu_request();
 dvd_menu_status();
}

static const struct ub_option options[]={
 {"bd_region",BD_AUTO,BD_ABC,&bd_region,bd_region_names},
 {"bd_auto",0,1,&bd_auto,0},
 {"dvd_region",DVD_STOCK,DVD_FREE,&dvd_region,dvd_region_names},
 {0,0,0,0,0}
};

static struct ub_part parts[]={
 {"BD pre-main launcher","apl_medm", "c092d29f3a16df117ae36d9ac915dd2dad87d1ea1fb9ca29bfcc7bb48d9971a4",&ub_payload_region_launch,1,launch_prepare,launch_step,{0},0,0},
 {"bd","apl_bdv",BDV_SHA256,0,0,0,bd_step,{0},0,0},
 {"dvd","apl_meda_dvdv",DVDV_SHA256,0,0,0,dvd_step,{0},0,0},
 {"disc",0,0,0,0,0,disc_step,{0},0,0},
};
/* The guards compare each new apl_bdv / apl_meda_dvdv (and its
 * libnavi_api.so) with these verified files instead of hashing them while
 * the disc starts. Also with the stock choices: the menu may change them. */
#define REMEMBER(f,st) do{(f).dev=(st).dev;(f).ino=(st).ino;(f).size=(st).size;(f).mtime=(st).mtime;(f).ok=1;}while(0)
static void start(struct ub_module *m){
 char h[65];struct ub_stat st;
 (void)m;
 bd_launch_part=&parts[0];
 unlink(UB_REGION_STATUS);unlink(UB_DVD_STATUS);
 if(bd_region==BD_AUTO&&!bd_auto){bd_region=BD_STOCK;ub_log(ID,UB_ALWAYS,"bd_region=auto needs bd_auto=1; the Blu-ray region is left to the player (stock)");}
 if(ub_hash_file(DVDV_PATH,h)||!ub_firmware_match(h,DVDV_SHA256)||ub_stat(DVDV_PATH,&st))ub_log(ID,UB_ALWAYS,DVDV_PATH " differs from 1.82 and 1.85 or is missing; DVD region guard off");
 else REMEMBER(dvdv_file,st);
 if(ub_hash_file(NAVI_PATH,h)||!ub_firmware_match(h,NAVI_SHA256)||ub_stat(NAVI_PATH,&st))ub_log(ID,UB_ALWAYS,NAVI_PATH " differs from 1.82 and 1.85 or is missing; DVD region guard off");
 else REMEMBER(navi_file,st);
 {int v=ub_hash_file(BDV_PATH,h)?0:ub_firmware_match(h,BDV_SHA256);
  if(!v||ub_stat(BDV_PATH,&st)){ub_log(ID,UB_ALWAYS,BDV_PATH " differs from 1.82 and 1.85 or is missing; BD region guard off");return;}
  if(v==185){bdv_region_addr=BDV_REGION_ADDR+BDV_185_SHIFT;bdv_psr20_addr=BDV_PSR20_ADDR+BDV_185_SHIFT;ub_log(ID,UB_ALWAYS,BDV_PATH " is the 1.85 build; BD region uses the +0xF0 data addresses (not confirmed on a player)");}
  REMEMBER(bdv_file,st);}
 bd_launch_publish();watcher_start();
}
struct ub_module ub_module_region={
 ID,"Region: BD region chosen per disc (bd_region), DVD region (dvd_region, best effort), disc region diagnostics",
 1,options,start,parts,sizeof parts/sizeof *parts,0,0
};
