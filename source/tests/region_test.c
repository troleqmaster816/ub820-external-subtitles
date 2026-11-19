/* Host test of the region module's disc parsers on synthetic discs. */
#ifndef UB_BD_RAM_DIR
#define UB_BD_RAM_DIR UB_TEST_DIR "/region-ram"
#endif
#define UB_REGION_MOUNTS UB_TEST_DIR "/mounts"
#include <unistd.h>
/* The watcher's parent check sees the reaper when fake_reaper is set, and a
 * menu save can report a forced result after config.c applied the value. */
static int fake_reaper,forcing,forced_rc;
static pid_t test_getppid(void){return fake_reaper?1:getppid();}
#define getppid test_getppid
#define ub_setting_set test_setting_set
#include "../modules/region/module.c"
#undef getppid
#undef ub_setting_set
int ub_setting_set(const char *module,const char *key,const char *value);
int test_setting_set(const char *module,const char *key,const char *value){int rc=ub_setting_set(module,key,value);return forcing&&rc>=0?forced_rc:rc;}
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>

static int failures;
/* Host tests never install ARM payloads. Use a tiny explicit asset fixture
 * when exercising atomic configuration publication instead of an ARM DSO. */
const struct ub_payload ub_payload_native_events_launch={0};
const struct ub_payload ub_payload_native_events_mgr_launch={0};
const unsigned char ub_asset_native_events_bd_preload[]={'x'};
const size_t ub_asset_native_events_bd_preload_size=1;
const char ub_asset_native_events_bd_preload_sha256[]="2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881";
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);failures++;}}while(0)
struct ub_module *const ub_modules[]={&ub_module_region,0};
/* config.c saves menu choices; on the host the stick is not involved */
const char *ub_stick_error(int code){(void)code;return "test";}

static void put(const char *path,const unsigned char *data,size_t n){FILE *f=fopen(path,"wb");fwrite(data,1,n,f);fclose(f);}
static void be32w(unsigned char *b,uint32_t v){b[0]=(unsigned char)(v>>24);b[1]=(unsigned char)(v>>16);b[2]=(unsigned char)(v>>8);b[3]=(unsigned char)v;}
static char *log_text(void){
 static char buf[16384];FILE *f=fopen(UB_LOG_FILE,"r");size_t n=f?fread(buf,1,sizeof buf-1,f):0;
 if(f)fclose(f);
 buf[n]=0;return buf;
}

static void read_text(const char *path,char *buf,size_t cap){
 FILE *f=fopen(path,"r");size_t n=f?fread(buf,1,cap-1,f):0;if(f)fclose(f);buf[n]=0;
 if(strstr(path,".cfg"))return;
 char *state=strstr(buf,"\nSTATE ");unsigned sec,ns,retry;int end=0;
 CHECK(state&&sscanf(state,"\nSTATE %u %u %u%n",&sec,&ns,&retry,&end)==3&&
       (sec||ns)&&ns<1000000000u&&retry<=1u&&!strcmp(state+end,"\n"));
 if(state)state[1]=0;
}
static void menu_set(const char *text){FILE *f=fopen(UB_REGION_REQUEST,"w");fputs(text,f);fclose(f);}
/* Explicit host-only readiness fixture for the synthetic player below. It has
 * no vendor waits. The actual interposer/loader contract has a separate ARM
 * test with the unmodified vendor libev and destructively interrupted packets. */
static void protect_fake_bd(void){
 char stat[4096],path[200],line[128];int fd;unsigned long long born;
 fd=open(UB_BD_PRELOAD,O_RDONLY);if(fd<0)_exit(103);
 if(mmap(0,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE,fd,0)==MAP_FAILED)_exit(104);
 close(fd);
 if(ub_native_wait_read("/proc/self/stat",stat,sizeof stat)<0||!(born=ub_native_wait_birth(stat)))_exit(105);
 snprintf(path,sizeof path,UB_NATIVE_WAIT_DIR "/%d.ready",(int)getpid());
 int lease=open(path,O_WRONLY|O_CREAT|O_CLOEXEC,0600);if(lease<0)_exit(106);
 snprintf(line,sizeof line,"EVW2 %d %llu %d\n",(int)getpid(),born,lease);
 put(path,(const unsigned char *)line,strlen(line));
}
static void test_bd_launch_ownership(void){
 struct ub_part part={0};struct ub_bd_launch_config c;int fd,saved=bd_region;unsigned saved_ok=bdv_file.ok;
 bd_region=BD_A;bdv_file.ok=1;bd_launch_armed=0;bd_launch_part=&part;
 part.target.verified=1;part.target.pid=getpid();part.target.installed=0;
 CHECK(!bd_launch_publish());CHECK(bd_launch_ready);
 fd=open(UB_BD_LAUNCH_CONFIG,O_RDONLY);CHECK(fd>=0);if(fd>=0){CHECK(read(fd,&c,sizeof c)==sizeof c);close(fd);CHECK(c.policy==1&&c.magic==UB_BD_LAUNCH_MAGIC);}
 /* A pending hook already excludes the legacy source writer. */
 watcher_start();CHECK(!watcher_pid);
 CHECK(!part.target.retry&&!watcher_pid);
 part.target.installed=1;launch_step(&part,&part.target);CHECK(bd_launch_armed&&!watcher_pid);
 /* Make atomic policy publication fail. It must not leave an old choice,
  * and must not introduce a second source writer beside the existing hook. */
 unlink(UB_BD_LAUNCH_CONFIG);CHECK(!mkdir(UB_BD_LAUNCH_CONFIG,0700));
 CHECK(bd_launch_publish()<0&&!bd_launch_ready);watcher_start();CHECK(!watcher_pid);
 CHECK(!rmdir(UB_BD_LAUNCH_CONFIG));
 menu_set("set A 301\n");menu_request();CHECK(bd_region==BD_A&&bd_launch_ready); /* same-choice retry */
 launch_step(&part,&part.target);CHECK(bd_launch_armed&&!watcher_pid);
 bd_launch_part=0;bd_launch_armed=bd_launch_ready=0;bdv_file.ok=(int)saved_ok;bd_region=saved;
}
/* The UB820 Helper menu: status for the menu, a choice applied and saved to
 * sub/ub820_menu.cfg (read back over modules.cfg), invalid choices refused. */
static void test_menu(void){
 char buf[512];int saved_region=bd_region;
 unlink(UB_REGION_STATUS);unlink(UB_REGION_REQUEST);unlink(UB_CONFIG_DIR "/ub820_menu.cfg");
 bd_auto=1;bd_region=BD_AUTO;bd_present=0;bd_player=0;decision_mode=0;menu_seq=0;menu_message[0]=0;
 menu_status();read_text(UB_REGION_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"0 auto 0 0 1 \n"));
 /* a BD with the player's region known: auto keeps B */
 bd_present=1;bd_player=2;decision_mode='K';
 menu_status();read_text(UB_REGION_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"0 auto 2 2 1 \n"));
 menu_set("set A 5\n");menu_request();menu_status();
 CHECK(bd_region==BD_A);CHECK(access(UB_REGION_REQUEST,F_OK)!=0);
 read_text(UB_REGION_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"5 A 2 2 1 saved\n")); /* this disc keeps its decision */
 read_text(UB_CONFIG_DIR "/ub820_menu.cfg",buf,sizeof buf);CHECK(strstr(buf,"[region]\nbd_region=A\n"));
 CHECK(strstr(log_text(),"Blu-ray region chosen in the menu: A (saved); it applies from the next disc on"));
 menu_set("set Z 6\n");menu_request();menu_status();
 CHECK(bd_region==BD_A);read_text(UB_REGION_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"6 A 2 2 1 unknown choice Z\n"));
 /* without bd_auto, auto is refused and not saved */
 bd_auto=0;menu_set("set auto 8\n");menu_request();menu_status();CHECK(bd_region==BD_A);
 read_text(UB_REGION_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"8 A 2 2 0 auto is off (bd_auto=0)\n"));
 read_text(UB_CONFIG_DIR "/ub820_menu.cfg",buf,sizeof buf);CHECK(!strstr(buf,"bd_region=auto"));
 bd_auto=1;
 menu_set("set stock 7\n");menu_request();CHECK(bd_region==BD_STOCK);
 read_text(UB_CONFIG_DIR "/ub820_menu.cfg",buf,sizeof buf);CHECK(strstr(buf,"bd_region=stock\n"));CHECK(!strstr(buf,"bd_region=A"));
 /* read back at start: the menu file wins over modules.cfg */
 {FILE *f=fopen(UB_CONFIG_DIR "/modules.cfg","w");fputs("[region]\nbd_region=C\n",f);fclose(f);}
 bd_region=BD_AUTO;ub_config_load();CHECK(bd_region==BD_STOCK);
 /* the next disc follows the new choice */
 bd_region=BD_A;decide_bd(0,1);CHECK(decision_mode=='F'&&decision_value==1);
 decision_mode=0;bd_present=0;bd_region=saved_region;
}

/* DVD region: the gate and region source in a copy of libnavi's layout (a
 * buffer below 4 GB in this process, written through /proc/self/mem like
 * apl_meda_dvdv's), and the DVD menu files. */
static void test_dvd(void){
 size_t size=0x80000;unsigned char *lib=mmap(0,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_SHARED|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 uint32_t base=(uint32_t)(uintptr_t)lib;pid_t pid;uint16_t sprm;char buf[256];
 unsigned char *gate=lib+(NAVI_GATE-NAVI_LINK_BASE),*src=lib+(NAVI_REGION_SOURCE-NAVI_LINK_BASE);
 uint16_t *sprm20=(uint16_t *)(lib+(NAVI_SPRM20-NAVI_LINK_BASE));
 int saved=dvd_region;
 CHECK(lib!=MAP_FAILED);if(lib==MAP_FAILED)return;
 pid=fork();CHECK(pid>=0);if(pid<0){munmap(lib,size);return;}if(!pid){prctl(PR_SET_PTRACER,PR_SET_PTRACER_ANY);for(;;)pause();}
 memcpy(gate,gate_stock,2);memcpy(src,source_stock,4);*sprm20=2;dvd_player=2;dvd_allowed=0;
 /* stock: nothing changes */
 dvd_region=DVD_STOCK;CHECK(dvd_apply(pid,base,1)==0);CHECK(!memcmp(gate,gate_stock,2)&&!memcmp(src,source_stock,4)&&*sprm20==2);
 /* free: only the gate */
 dvd_region=DVD_FREE;CHECK(dvd_apply(pid,base,1)==1);CHECK(gate[1]==0xe0&&gate[0]==0x02);CHECK(!memcmp(src,source_stock,4)&&*sprm20==2);
 /* region 1: the source returns mask 1 (movs r0,#1; bx lr) and SPRM20 follows */
 dvd_region=DVD_R1;CHECK(dvd_apply(pid,base,1)==1);
 CHECK(src[0]==1&&src[1]==0x20&&src[2]==0x70&&src[3]==0x47);CHECK(*sprm20==1);
 /* another region over our own patch */
 dvd_region=3;CHECK(dvd_apply(pid,base,1)==1);CHECK(src[0]==4&&*sprm20==4);
 /* auto: a disc for regions 1 and 4 on a region 2 player gets region 1;
  * a disc that allows 2 keeps the player's */
 dvd_region=DVD_AUTO;dvd_allowed=0x09;CHECK(dvd_mask()==1);dvd_apply(pid,base,1);CHECK(src[0]==1&&*sprm20==1);
 dvd_allowed=0x02;CHECK(dvd_mask()==0);dvd_apply(pid,base,1);CHECK(!memcmp(src,source_stock,4)&&*sprm20==2&&gate[1]==0xe0);
 /* back to stock: everything as the player had it */
 dvd_region=6;dvd_apply(pid,base,1);CHECK(*sprm20==0x20);
 dvd_region=DVD_STOCK;CHECK(dvd_apply(pid,base,1)==1);
 CHECK(!memcmp(gate,gate_stock,2)&&!memcmp(src,source_stock,4)&&*sprm20==2);
 /* A guard already in flight may finish its old patch, but the menu must
  * reap it BEFORE publishing stock. It cannot overwrite stock afterwards. */
 {
  int ready[2];char byte;sigset_t term,old;volatile unsigned *finished=(volatile unsigned *)lib;
  *finished=0;CHECK(!pipe(ready));sigemptyset(&term);sigaddset(&term,SIGTERM);CHECK(!sigprocmask(SIG_BLOCK,&term,&old));
  dvd_region=DVD_R1;dvd_guard_pid=fork();CHECK(dvd_guard_pid>=0);
  if(!dvd_guard_pid){
   struct sigaction action;memset(&action,0,sizeof action);action.sa_handler=dvd_guard_cancelled;sigemptyset(&action.sa_mask);sigaction(SIGTERM,&action,0);
   dvd_guard_cancel=0;sigprocmask(SIG_SETMASK,&old,0);close(ready[0]);
   if(write(ready[1],"r",1)!=1)_exit(90);
   close(ready[1]);while(!dvd_guard_cancel)pause();
   *finished=dvd_apply(pid,base,1)<0?2u:1u;_exit(0);
  }
  sigprocmask(SIG_SETMASK,&old,0);close(ready[1]);CHECK(read(ready[0],&byte,1)==1);close(ready[0]);
  {FILE *f=fopen(UB_DVD_REQUEST,"w");CHECK(f!=0);if(f){fputs("set stock 99\n",f);fclose(f);}}
  dvd_menu_request();CHECK(!dvd_guard_pid&&dvd_region==DVD_STOCK&&*finished==1);
  CHECK(src[0]==1&&source_ours(src)); /* old transaction finished first */
  CHECK(dvd_apply(pid,base,1)>=0);CHECK(!memcmp(gate,gate_stock,2)&&!memcmp(src,source_stock,4)&&*sprm20==2);
 }
 /* Neither site is modified if the other fails the preflight. */
 memcpy(gate,gate_stock,2);src[0]=0xff;dvd_region=DVD_FREE;
 CHECK(dvd_apply(pid,base,1)==-1);CHECK(!memcmp(gate,gate_stock,2));
 memcpy(src,source_stock,4);
 /* unknown code is left alone */
 gate[0]=0xff;dvd_region=DVD_FREE;CHECK(dvd_apply(pid,base,1)==-1);CHECK(gate[0]==0xff);
 (void)sprm;
 /* the menu: choice saved, status answered, unknown refused */
 unlink(UB_DVD_STATUS);unlink(UB_DVD_REQUEST);unlink(UB_CONFIG_DIR "/ub820_menu.cfg");
 dvd_region=DVD_STOCK;dvd_seq=0;dvd_message[0]=0;dvd_menu_status();read_text(UB_DVD_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"0 stock \n"));
 {FILE *f=fopen(UB_DVD_REQUEST,"w");fputs("set 3 4\n",f);fclose(f);}
 dvd_menu_request();dvd_menu_status();CHECK(dvd_region==3);
 read_text(UB_DVD_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"4 3 saved\n"));
 read_text(UB_CONFIG_DIR "/ub820_menu.cfg",buf,sizeof buf);CHECK(strstr(buf,"dvd_region=3\n"));
 {FILE *f=fopen(UB_DVD_REQUEST,"w");fputs("set world 5\n",f);fclose(f);}
 dvd_menu_request();CHECK(dvd_region==3);
 kill(pid,SIGKILL);waitpid(pid,0,0);
 munmap(lib,size);dvd_region=saved;dvd_allowed=0;
}

static unsigned log_count(const char *what){
 static char buf[300000];unsigned n=0;FILE *f=fopen(UB_LOG_FILE,"r");size_t got=f?fread(buf,1,sizeof buf-1,f):0;
 if(f)fclose(f);
 buf[got]=0;for(const char *q=buf;(q=strstr(q,what));q++)n++;return n;
}
static int launch_policy(void){struct ub_bd_launch_config c;int fd=open(UB_BD_LAUNCH_CONFIG,O_RDONLY),ok;if(fd<0)return -100;ok=read(fd,&c,sizeof c)==sizeof c;close(fd);return ok?c.policy:-101;}
/* The parent can end between fork and the child's first check; then both
 * getppid() reads see the reaper. The owner is taken before fork instead. */
static void test_watcher_owner(void){
 int saved=bd_region,status=0;pid_t r=0;unsigned i;
 bd_region=BD_A;bd_launch_part=0;bdv_file.ok=1;fake_reaper=1;
 watcher_start();fake_reaper=0;CHECK(watcher_pid>0);
 for(i=0;i<100&&watcher_pid>0&&!(r=waitpid(watcher_pid,&status,WNOHANG));i++)usleep(10000);
 CHECK(r==watcher_pid&&WIFEXITED(status)&&!WEXITSTATUS(status)); /* reparented before its check: never watches */
 if(r!=watcher_pid)watcher_stop();
 watcher_pid=0;bd_region=saved;
}
/* Children forked without exec (DVD guard, BD watcher) must not keep the
 * helper's descriptors such as its single-instance lock: core.c refuses a
 * new helper while that lock is held. */
/* 1 when pid holds a descriptor for path (by target: freed numbers get reused). */
static int holds(pid_t pid,const char *path){
 char dir[48],link[320],target[512],canonical[1024];struct dirent *e;DIR *d;int found=0;ssize_t n;
 if(!realpath(path,canonical))return -1;
 snprintf(dir,sizeof dir,"/proc/%d/fd",(int)pid);if(!(d=opendir(dir)))return -1;
 while((e=readdir(d))){
  if(e->d_name[0]=='.')continue;
  snprintf(link,sizeof link,"%s/%s",dir,e->d_name);
  if((n=readlink(link,target,sizeof target-1))>0){target[n]=0;if(!strcmp(target,canonical))found=1;}
 }
 closedir(d);return found;
}
static void test_child_descriptors(void){
 static const char lockpath[]=UB_TEST_DIR "/instance.lock";
 char path[96],target[256];int lock=open(lockpath,O_RDWR|O_CREAT,0600);
 int saved_dvd=dvd_region,saved_bd=bd_region,saved_dvdv=dvdv_file.ok,saved_navi=navi_file.ok;
 CHECK(lock>2);CHECK(holds(getpid(),lockpath)==1);
 /* Reparenting between fork and arming the signal cannot leave a DVD writer. */
 dvd_region=DVD_R1;dvdv_file.ok=navi_file.ok=1;fake_reaper=1;
 dvd_guard_start();fake_reaper=0;
 {int status=0;pid_t r=0;CHECK(dvd_guard_pid>0);
  for(unsigned i=0;i<100&&!(r=waitpid(dvd_guard_pid,&status,WNOHANG));i++)usleep(10000);
  CHECK(r==dvd_guard_pid&&WIFEXITED(status)&&!WEXITSTATUS(status));
  if(r!=dvd_guard_pid)dvd_guard_stop();
  dvd_guard_pid=0;}
 dvd_region=DVD_R1;dvdv_file.ok=navi_file.ok=1;dvd_guard_start();CHECK(dvd_guard_pid>0);usleep(100000);
 CHECK(holds(dvd_guard_pid,lockpath)==0);
 snprintf(path,sizeof path,"/proc/%d/fd/2",(int)dvd_guard_pid);CHECK(readlink(path,target,sizeof target)>0); /* the log stays */
 CHECK(dvd_guard_stop());
 bd_region=BD_A;bd_launch_part=0;watcher_start();CHECK(watcher_pid>0);usleep(100000);
 CHECK(holds(watcher_pid,lockpath)==0);
 watcher_stop();
 close(lock);unlink(lockpath);
 dvd_region=saved_dvd;bd_region=saved_bd;dvdv_file.ok=saved_dvdv;navi_file.ok=saved_navi;
}
/* Extra UDF/ISO 9660 mounts: the previous disc state follows the full path
 * when the mount list is reordered, and a vanished root gets its eject. */
static void write_mounts(const char *a,const char *b){
 FILE *f=fopen(UB_REGION_MOUNTS,"w");if(!f)return;
 if(a)fprintf(f,"/dev/sr0 %s udf ro 0 0\n",a);
 if(b)fprintf(f,"/dev/loop0 %s iso9660 ro 0 0\n",b);
 fclose(f);
}
static void rescan_discs(void){for(unsigned i=0;i<10;i++)disc_step(0,0);} /* one mount refresh per 10 ticks */
static void test_disc_roots(const char *bd,const char *dvd){
 unsigned found;int saved=bd_region,saved_ok=bdv_file.ok;
 bdv_file.ok=0;bd_region=BD_A;bd_present=0;decide_clear();
 write_mounts(bd,dvd);rescan_discs();
 CHECK(bd_present&&decision_mode=='F');found=log_count("Blu-ray found at");
 write_mounts(dvd,bd);rescan_discs();
 CHECK(log_count("Blu-ray found at")==found);CHECK(bd_present&&decision_mode=='F'); /* no re-analysis after reordering */
 write_mounts(dvd,0);rescan_discs();
 CHECK(!bd_present&&!decision_mode); /* the BD's root vanished from the mount list */
 write_mounts(0,0);rescan_discs();
 bdv_file.ok=saved_ok;bd_region=saved;
}
/* Two DVD roots: when the one analysed last leaves, the other's own regions
 * apply again. An unreadable mount table keeps the last list. */
static void test_two_dvds(void){
 struct disc_state gone;
 memset(disc_roots,0,sizeof disc_roots);memset(&gone,0,sizeof gone);
 strcpy(disc_roots[0].path,"/mnt/odd");disc_roots[0].seen=101;disc_roots[0].dvd_mask=1;disc_count=1;
 gone.seen=203;gone.dvd_mask=2;dvd_allowed=2;
 disc_gone(&gone);CHECK(dvd_allowed==1&&disc_roots[0].seen==101&&!gone.seen);
 disc_roots[0].dvd_mask=0;gone.seen=203;gone.dvd_mask=2;dvd_allowed=2;
 disc_gone(&gone);CHECK(dvd_allowed==0);
 dvd_allowed=8;dvd_ifo(UB_TEST_DIR "/absent");CHECK(dvd_allowed==0);
 disc_roots[0].dvd_mask=dvd_allowed=1;
 strcpy(disc_roots[1].path,"/mnt/extra");disc_roots[1].seen=303;disc_roots[1].dvd_mask=4;disc_count=2;
 unlink(UB_REGION_MOUNTS);disc_rescan();
 CHECK(disc_count==2&&disc_roots[1].seen==303&&!strcmp(disc_roots[1].path,"/mnt/extra")&&dvd_allowed==1);
 CHECK(!mkdir(UB_REGION_MOUNTS,0700));disc_rescan(); /* open succeeds, read fails */
 CHECK(disc_count==2&&disc_roots[1].seen==303&&dvd_allowed==1);CHECK(!rmdir(UB_REGION_MOUNTS));
 {FILE *f=fopen(UB_REGION_MOUNTS,"w");CHECK(f);if(f){
  for(unsigned i=0;i<1000;i++)fputs("none /mnt/a tmpfs ro 0 0\n",f);
  for(unsigned i=0;i<DISC_ROOTS+1;i++)fprintf(f,"/dev/d%u /mnt/new%u udf ro 0 0\n",i,i);
  fputs("/dev/sr1 /mnt/extra udf ro 0 0\n",f);fclose(f);
  disc_rescan();CHECK(disc_count==DISC_ROOTS&&disc_roots[1].seen==303&&dvd_allowed==1);
 }}
 unlink(UB_REGION_MOUNTS);
 memset(disc_roots,0,sizeof disc_roots);disc_count=0;dvd_allowed=0;
}
/* Applied, published in RAM and saved to USB are separate results. */
static void test_setting_results(void){
 char buf[256];int saved=bd_region,saved_dvd=dvd_region,saved_ok=bdv_file.ok;unsigned failed;
 bd_region=BD_A;bdv_file.ok=1;bd_launch_part=0;bd_launch_armed=0;bd_auto=1;
 CHECK(!bd_launch_publish());CHECK(launch_policy()==1);
 /* config.c applied B in RAM, then the USB copy failed with a negative tool code */
 forcing=1;forced_rc=-2;menu_set("set B 21\n");menu_request();menu_status();forcing=0;watcher_stop();
 CHECK(bd_region==BD_B);read_text(UB_REGION_STATUS,buf,sizeof buf);
 CHECK(!strncmp(buf,"21 B ",5)&&strstr(buf,"not saved to USB")&&!strstr(buf,"unknown"));
 CHECK(launch_policy()==2);
 forcing=1;forced_rc=5;menu_set("set C 22\n");menu_request();forcing=0;watcher_stop();
 CHECK(bd_region==BD_C&&launch_policy()==4);
 /* A choice applied outside the menu while the old policy is ready: the next
  * tick republishes it instead of keeping the previous selection in force. */
 bd_region=BD_ABC;disc_step(0,0);watcher_stop();CHECK(launch_policy()==7&&bd_launch_ready);
 /* A failing publication is retried, but logged once per failure streak. */
 failed=log_count("BD pre-main policy unavailable");
 unlink(UB_BD_LAUNCH_CONFIG);CHECK(!mkdir(UB_BD_LAUNCH_CONFIG,0700));
 CHECK(bd_launch_publish()<0);CHECK(bd_launch_publish()<0);
 CHECK(log_count("BD pre-main policy unavailable")==failed+1);
 CHECK(!rmdir(UB_BD_LAUNCH_CONFIG));CHECK(!bd_launch_publish()&&launch_policy()==7);
 /* DVD: applied in RAM, USB failed: not "unknown choice" */
 {FILE *f=fopen(UB_DVD_REQUEST,"w");fputs("set 5 23\n",f);fclose(f);}
 forcing=1;forced_rc=-2;dvd_menu_request();dvd_menu_status();forcing=0;
 CHECK(dvd_region==5);read_text(UB_DVD_STATUS,buf,sizeof buf);CHECK(strstr(buf,"23 5 not saved to USB")&&!strstr(buf,"unknown"));
 {FILE *f=fopen(UB_DVD_REQUEST,"w");fputs("set world 24\n",f);fclose(f);}
 dvd_menu_request();dvd_menu_status();read_text(UB_DVD_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"24 5 unknown choice world\n"));
 bd_region=saved;dvd_region=saved_dvd;bdv_file.ok=saved_ok;bd_launch_ready=0;
}
static void test_disabled_native_events(void){
 extern struct ub_module ub_module_native_events;
 int saved=ub_module_native_events.enabled,saved_region=bd_region,saved_ok=bdv_file.ok;
 struct ub_part *saved_part=bd_launch_part;
 ub_module_native_events.enabled=1;bd_region=BD_A;bdv_file.ok=1;bd_launch_part=ub_native_bd_part();
 CHECK(!bd_launch_publish());CHECK(!access(UB_BD_LAUNCH_CONFIG,F_OK));
 ub_module_native_events.enabled=0;ub_module_native_events.disabled_step(&ub_module_native_events);
 CHECK(access(UB_BD_LAUNCH_CONFIG,F_OK)<0);
 menu_set("set B 401\n");menu_request();
 CHECK(bd_region==BD_B&&!menu_publish_pending&&!strstr(menu_message,"retrying"));
 CHECK(bd_launch_publish()==1&&!bd_launch_ready);
 disc_step(0,0);CHECK(!bd_launch_part&&access(UB_BD_LAUNCH_CONFIG,F_OK)<0);
 watcher_stop();ub_module_native_events.enabled=saved;bd_region=saved_region;bdv_file.ok=saved_ok;
 bd_launch_part=saved_part;bd_launch_ready=bd_launch_armed=bd_launch_failed=0;
}

int main(void){
 char root[]=UB_TEST_DIR "/bd",dvd[]=UB_TEST_DIR "/dvd",path[512];struct disc_state ds;memset(&ds,0,sizeof ds);
 unsigned char idx[94]={0},mobj[50+4+48]={0},ifo[0x30]={0};
 /* index.bdmv: First Play HDMV object 0, Top Menu BD-J 00001, 2 titles. */
 memcpy(idx,"INDX0200",8);be32w(idx+8,40);
 idx[44]=0x40;idx[56]=0x80;memcpy(idx+62,"00001",5);
 idx[69]=2;idx[70]=0x40;idx[82]=0x80;
 /* MovieObject.bdmv: MOVE r1, PSR20; AND r1, 0x2; BC r1, 0x2; GOTO. */
 static const unsigned char cmds[48]={
  0x50,0x00,0x00,0x01, 0,0,0,1, 0x80,0,0,0x14,
  0x50,0x40,0x00,0x09, 0,0,0,1, 0,0,0,2,
  0x48,0x40,0x01,0x00, 0,0,0,1, 0,0,0,2,
  0,0,0,0, 0,0,0,0, 0,0,0,0};
 memcpy(mobj,"MOBJ0200",8);mobj[49]=1;mobj[53]=4;memcpy(mobj+54,cmds,48);
 /* VIDEO_TS.IFO: every region but 2 prohibited. */
 memcpy(ifo,"DVDVIDEO-VMG",12);ifo[0x23]=0xfd;

 mkdir(root,0755);snprintf(path,sizeof path,"%s/BDMV",root);mkdir(path,0755);
 snprintf(path,sizeof path,"%s/BDMV/JAR",root);mkdir(path,0755);
 snprintf(path,sizeof path,"%s/BDMV/JAR/00001.jar",root);put(path,(const unsigned char *)"x",1);
 snprintf(path,sizeof path,"%s/BDMV/index.bdmv",root);put(path,idx,sizeof idx);
 snprintf(path,sizeof path,"%s/BDMV/MovieObject.bdmv",root);put(path,mobj,sizeof mobj);
 mkdir(dvd,0755);snprintf(path,sizeof path,"%s/VIDEO_TS",dvd);mkdir(path,0755);
 snprintf(path,sizeof path,"%s/VIDEO_TS/VIDEO_TS.IFO",dvd);put(path,ifo,sizeof ifo);

 ub_log_open();ub_log_level=UB_DEBUG;
 bd_region=BD_AUTO;bd_auto=1;   /* the disc analysis below decides as auto */
 disc_root(root,&ds);
 CHECK(ds.seen!=0);
 disc_root(root,&ds); /* same disc: analysed once */
 ds.seen=0;disc_root(dvd,&ds);
 {
  char *t=log_text();
  printf("%s",t);
  CHECK(strstr(t,"First Play HDMV object 0, Top Menu BD-J 00001, 2 titles (1 HDMV, 1 BD-J)"));
  CHECK(strstr(t,"HDMV object 0 cmd 0: MOVE r1, PSR20"));
  CHECK(strstr(t,"HDMV object 0 cmd 1: AND r1, 0x2"));
  CHECK(strstr(t,"HDMV object 0 cmd 2: BC r1, 0x2"));
  CHECK(strstr(t,"HDMV: 1 movie objects, 1 commands read PSR20"));
  CHECK(strstr(t,"BD-J: 1 entries in BDMV/JAR"));
  CHECK(strstr(t,"DVD: prohibited-region mask 0xfd, playable in region 2"));
  {char *a=strstr(t,"Blu-ray found");CHECK(a&&!strstr(a+1,"Blu-ray found"));}
  CHECK(strstr(t,"tested regions: B"));
 }
 /* auto: the HDMV program tests B, so the decision is the mask B; it is
  * forgotten when the BD leaves. */
 CHECK(decision_mode=='M'&&decision_value==2);
 snprintf(path,sizeof path,"%s/BDMV/index.bdmv",root);unlink(path);
 ds.seen=0x8000000000000001ull;disc_root(root,&ds);
 CHECK(ds.seen==0);CHECK(!decision_mode);
 snprintf(path,sizeof path,"%s/BDMV/index.bdmv",root);put(path,idx,sizeof idx);
 /* Fixed regions and the BD-J case. */
 bd_region=BD_A;decide_bd(2,0);CHECK(decision_mode=='F'&&decision_value==1);decide_clear();
 bd_region=BD_ABC;decide_bd(0,1);CHECK(decision_mode=='F'&&decision_value==7);decide_clear();
 bd_region=BD_AUTO;decide_bd(0,1);CHECK(decision_mode=='K');decide_clear();
 /* Region choice from a decision and the player's region. */
 CHECK(region_for('M',1,2)==1);CHECK(region_for('M',6,2)==2);CHECK(region_for('M',3,4)==1);
 CHECK(region_for('F',4,2)==4);CHECK(region_for('F',3,2)==2);CHECK(region_for('K',0,2)==2);
 /* The source movs: each drive answer becomes the decision's region. */
 {unsigned char b[3];bd_source_bytes('F',7,b);CHECK(b[0]==7&&b[1]==7&&b[2]==7);
  bd_source_bytes('M',3,b);CHECK(b[0]==1&&b[1]==2&&b[2]==1);
  bd_source_bytes('K',0,b);CHECK(b[0]==1&&b[1]==2&&b[2]==4);}
 /* The watcher against a fake apl_bdv (this test's executable named
  * apl_bdv): FUN_0004a5b8's three movs at their addresses, the region slot
  * at 0x00aeaec0 and PSR20 at 0x00b35b6c. It "asks the drive" (the slot
  * gets the immediate of the answer's movs) 300 ms after it starts, or at
  * once in the late rounds, and the engine copies the slot into PSR20 100
  * ms later; the exit status is the PSR20 the disc saw.
  * Rounds: A early, A|B|C early, auto early (the disc tests B, the drive
  * says A), A late (PSR20 set instead), auto with no disc (left alone),
  * A with apl_bdv already running when the watcher starts (left alone). */
 {
  static const struct { int region,late,drive,disc,before; uint32_t want; const char *log; } rounds[]={
   {BD_A,0,1,1,0,1,"drive A/B/C -> A/A/A (bd_region setting)"},
   {BD_ABC,0,1,1,0,7,"drive A/B/C -> A|B|C/A|B|C/A|B|C (bd_region setting)"},
   {BD_AUTO,0,0,1,0,2,"drive A/B/C -> B/B/B (auto: the disc's HDMV program tests these regions)"},
   {BD_A,1,1,1,0,1,"had already asked the drive (B); PSR20 is set instead"},
   {BD_AUTO,0,0,0,0,1,"no Blu-ray files within 1000 ms"},
   {BD_A,0,1,1,1,2,"BD region watcher"},
  };
  struct stat st;unsigned r;CHECK(!stat("/proc/self/exe",&st));
  bdv_file.dev=st.st_dev;bdv_file.ino=st.st_ino;bdv_file.size=st.st_size;bdv_file.mtime=st.st_mtime;bdv_file.ok=1;
  bd_test_root=root;
  mkdir(UB_BD_RAM_DIR,0700);mkdir(UB_NATIVE_WAIT_DIR,0700);put(UB_BD_PRELOAD,(const unsigned char *)"x",1);
  for(r=0;r<sizeof rounds/sizeof *rounds;r++){
   int status=0;pid_t fake;
   if(!rounds[r].disc)bd_test_root=dvd;
   bd_region=rounds[r].region;
   if(!rounds[r].before){watcher_start();CHECK(watcher_pid>0);usleep(100000);}   /* its first scan: what runs already is left alone */
   fake=fork();
   if(!fake){
    volatile uint32_t *slot,*psr;volatile unsigned char *code;
    static const uint32_t answer[3]={0x4a738,0x4a7bc,0x4a7c2};
    prctl(PR_SET_PTRACER,PR_SET_PTRACER_ANY);
    if(mmap((void *)0x4a000,0x1000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==MAP_FAILED)_exit(100);
    if(mmap((void *)0xaea000,0x1000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==MAP_FAILED)_exit(101);
    if(mmap((void *)0xb35000,0x1000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==MAP_FAILED)_exit(102);
    code=(volatile unsigned char *)0x4a000;slot=(volatile uint32_t *)0xaeaec0;psr=(volatile uint32_t *)0xb35b6c;
    code[0x738]=1;code[0x739]=0x23;code[0x7bc]=2;code[0x7bd]=0x23;code[0x7c2]=4;code[0x7c3]=0x23;
    *slot=0;*psr=0;
    if(rounds[r].late)*slot=code[answer[rounds[r].drive]-0x4a000];
    protect_fake_bd();
    prctl(PR_SET_NAME,"apl_bdv");
    usleep(300000);
    *slot=code[answer[rounds[r].drive]-0x4a000];   /* the drive's answer */
    usleep(100000);*psr=*slot;                      /* the engine fills PSR20 */
    usleep(400000);_exit((int)*psr);
   }
   if(rounds[r].before){usleep(100000);watcher_start();CHECK(watcher_pid>0);}
   waitpid(fake,&status,0);
   CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==(int)rounds[r].want);
   if(!WIFEXITED(status)||WEXITSTATUS(status)!=(int)rounds[r].want)printf("round %u: status %d\n",r,WIFEXITED(status)?WEXITSTATUS(status):-1);
   usleep(50000);
   /* Readiness I/O no longer freezes the fake player. It can exit before
    * the independent one-second no-disc deadline; await only that log. */
   if(!rounds[r].disc)for(unsigned spin=0;spin<120&&!strstr(log_text(),rounds[r].log);spin++)usleep(10000);
   CHECK(strstr(log_text(),rounds[r].log));
   watcher_stop();bd_test_root=root;
  }
  {char *t=log_text();
   CHECK(strstr(t,"BD region set at the source in apl_bdv PID"));CHECK(strstr(t,"before it asked the drive"));
   CHECK(strstr(t,"was B; set to A"));CHECK(strstr(t,"sees region A|B|C (PSR20)"));}
  bd_test_root=0;bd_region=BD_AUTO;
 }
 /* HDMV heuristic: copies of the register, NE and direct PSR20 compares. */
 {static const unsigned char c2[]={
   0x50,0x00,0x00,0x01, 0,0,0,3, 0x80,0,0,0x14,   /* MOVE r3, PSR20 */
   0x50,0x00,0x00,0x01, 0,0,0,4, 0,0,0,3,         /* MOVE r4, r3 */
   0x48,0x40,0x03,0x00, 0,0,0,4, 0,0,0,1,         /* NE r4, 0x1 */
   0x48,0x40,0x02,0x00, 0x80,0,0,0x14, 0,0,0,4,   /* EQ PSR20, 0x4 */
   0x48,0x40,0x02,0x00, 0,0,0,9, 0,0,0,2};        /* EQ r9, 0x2: untracked */
  CHECK(hdmv_mask_of_object(c2,5)==5);}
 decide_clear();
 test_menu();
 test_dvd();
 test_bd_launch_ownership();
 watcher_stop();
 {struct stat st;CHECK(!stat("/proc/self/exe",&st));bdv_file.dev=st.st_dev;bdv_file.ino=st.st_ino;bdv_file.size=st.st_size;bdv_file.mtime=st.st_mtime;}
 test_watcher_owner();
 test_child_descriptors();
 test_disc_roots(root,dvd);
 test_two_dvds();
 test_setting_results();
 test_disabled_native_events();
 watcher_stop();
 printf("%s (%d failures)\n",failures?"region_test FAILED":"region_test passed",failures);
 return failures!=0;
}
