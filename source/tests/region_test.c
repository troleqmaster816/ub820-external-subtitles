/* Host test of the region module's disc parsers on synthetic discs. */
#include "../modules/region/module.c"
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>

static int failures;
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

static void read_text(const char *path,char *buf,size_t cap){FILE *f=fopen(path,"r");size_t n=f?fread(buf,1,cap-1,f):0;if(f)fclose(f);buf[n]=0;}
static void menu_set(const char *text){FILE *f=fopen(UB_REGION_REQUEST,"w");fputs(text,f);fclose(f);}
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
 size_t size=0x80000;unsigned char *lib=mmap(0,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 uint32_t base=(uint32_t)(uintptr_t)lib;int pid=getpid();uint16_t sprm;char buf[256];
 unsigned char *gate=lib+(NAVI_GATE-NAVI_LINK_BASE),*src=lib+(NAVI_REGION_SOURCE-NAVI_LINK_BASE);
 uint16_t *sprm20=(uint16_t *)(lib+(NAVI_SPRM20-NAVI_LINK_BASE));
 int saved=dvd_region;
 CHECK(lib!=MAP_FAILED);if(lib==MAP_FAILED)return;
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
 munmap(lib,size);dvd_region=saved;dvd_allowed=0;
}

int main(void){
 char root[]=UB_TEST_DIR "/bd",dvd[]=UB_TEST_DIR "/dvd",path[512];unsigned long long seen=0;
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
 disc_root(root,&seen);
 CHECK(seen!=0);
 disc_root(root,&seen); /* same disc: analysed once */
 seen=0;disc_root(dvd,&seen);
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
 seen=0x8000000000000001ull;disc_root(root,&seen);
 CHECK(seen==0);CHECK(!decision_mode);
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
 watcher_stop();
 printf("%s (%d failures)\n",failures?"region_test FAILED":"region_test passed",failures);
 return failures!=0;
}
