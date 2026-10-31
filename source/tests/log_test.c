/* Host test of the log module: level gate, saving through the storage layer
 * (stubbed here: the real tool is an ARM program for the player's libufs) on
 * request and continuously, keeping the previous run's log, trimming the RAM
 * log, the mount record, and the diagnostics parsers: ARM core notes, memory
 * maps, process exit events, /proc stat lines, the kernel log, and a busy
 * thread found by the scan. */
#define DIAG_BUSY_SECONDS 2
#include "../modules/log/module.c"
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);failures++;}}while(0)
struct ub_module *const ub_modules[]={&ub_module_log,0};

/* Storage stub: records the last copy in each direction. */
static int stub_result,stub_read_result=4,writes;
static char saved_from[256],saved_to[256],read_from[256];
int ub_stick_write(const char *tmp_path,const char *stick_path){
 writes++;snprintf(saved_from,sizeof saved_from,"%s",tmp_path);snprintf(saved_to,sizeof saved_to,"%s",stick_path);return stub_result;
}
int ub_stick_read(const char *stick_path,const char *tmp_path){
 snprintf(read_from,sizeof read_from,"%s",stick_path);
 if(!stub_read_result){FILE *f=fopen(tmp_path,"w");fputs("old run\n",f);fclose(f);}
 return stub_read_result;
}
const char *ub_stick_error(int code){return code?"destination cannot be created":"done";}

static size_t slurp(const char *path,char *buf,size_t cap){FILE *f=fopen(path,"r");size_t n=f?fread(buf,1,cap-1,f):0;if(f)fclose(f);buf[n]=0;return n;}
static unsigned count(const char *text,const char *needle){unsigned n=0;for(const char *p=text;(p=strstr(p,needle));p++)n++;return n;}
static void request(const char *text){FILE *f=fopen(UB_STORAGE_REQUEST,"w");fputs(text,f);fclose(f);}
static char ram[1<<20];
static const char *log_text(void){slurp(UB_LOG_FILE,ram,sizeof ram);return ram;}

static void put32(unsigned char *p,uint32_t v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);p[2]=(unsigned char)(v>>16);p[3]=(unsigned char)(v>>24);}
static void put16(unsigned char *p,uint16_t v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static size_t note(unsigned char *p,uint32_t type,const void *desc,uint32_t size){
 put32(p,5);put32(p+4,size);put32(p+8,type);memcpy(p+12,"CORE\0\0\0",8);memcpy(p+20,desc,size);
 return 20+((size+3)&~3u);
}
/* An ARM ELF core as the kernel writes it: header, one PT_NOTE, notes, then
 * memory (here: filler). */
static size_t make_core(unsigned char *core,size_t filler){
 unsigned char prs[148],psi[124],sig[128];size_t off=52+32;
 memset(core,0,84);memcpy(core,"\177ELF\1\1\1",7);put16(core+16,4);put16(core+18,40);put32(core+28,52);put16(core+42,32);put16(core+44,1);
 memset(prs,0,sizeof prs);put16(prs+12,11);put32(prs+24,4321);
 for(unsigned r=0;r<18;r++)put32(prs+72+4*r,0x1000+r);
 put32(prs+72+4*15,0x2a38a6d4);put32(prs+72+4*14,0x0000a010);
 memset(psi,0,sizeof psi);memcpy(psi+28,"apl_test",8);memcpy(psi+44,"/usr/dtvrec/bin/apl_test -x",27);
 memset(sig,0,sizeof sig);put32(sig,11);put32(sig+8,1);put32(sig+12,0x10);
 off+=note(core+off,1,prs,sizeof prs);
 off+=note(core+off,3,psi,sizeof psi);
 off+=note(core+off,0x53494749,sig,sizeof sig);
 put32(core+52,4);put32(core+52+4,84);put32(core+52+16,(uint32_t)(off-84));
 memset(core+off,0xab,filler);
 return off+filler;
}

static void test_saving(void){
 static char status[256];
 unlink(UB_STORAGE_REQUEST);unlink(UB_STORAGE_STATUS);
 /* No request: nothing happens. */
 step(0,0);CHECK(!saved_to[0]);
 /* A request is consumed once and answered with its sequence number. */
 request("save-log 7\n");step(0,0);
 CHECK(!strcmp(saved_from,UB_LOG_FILE));CHECK(!strcmp(saved_to,UB_STICK_LOG));
 CHECK(access(UB_STORAGE_REQUEST,F_OK)!=0);
 slurp(UB_STORAGE_STATUS,status,sizeof status);CHECK(!strcmp(status,"7 0 saved as sub/ub820_submenu.log\n"));
 saved_to[0]=0;step(0,0);CHECK(!saved_to[0]);
 /* A failure is reported with the storage code and logged as an error. */
 stub_result=5;request("save-log 8\n");step(0,0);
 slurp(UB_STORAGE_STATUS,status,sizeof status);CHECK(!strcmp(status,"8 5 destination cannot be created\n"));
 CHECK(strstr(log_text(),"cannot save the log to the stick: destination cannot be created (code 5)"));
 /* save=off refuses without touching storage. */
 save=SAVE_OFF;stub_result=0;saved_to[0]=0;request("save-log 9\n");step(0,0);
 slurp(UB_STORAGE_STATUS,status,sizeof status);CHECK(!strncmp(status,"9 10 ",5));CHECK(!saved_to[0]);
 /* Garbage is dropped. */
 save=SAVE_MENU;request("rm -rf /\n");step(0,0);CHECK(access(UB_STORAGE_REQUEST,F_OK)!=0);
}

static void test_continuous(void){
 int before;
 /* The previous run's log is copied aside when the helper starts. */
 save=SAVE_ALWAYS;stub_read_result=0;writes=0;
 start(&ub_module_log);
 CHECK(!strcmp(read_from,UB_STICK_LOG));CHECK(!strcmp(saved_from,UB_PREV_TMP));CHECK(!strcmp(saved_to,UB_STICK_PREV));
 CHECK(access(UB_PREV_TMP,F_OK)!=0);CHECK(strstr(log_text(),"previous run's log kept as"));
 /* No earlier log: nothing is copied. */
 stub_read_result=4;writes=0;keep_previous();CHECK(!writes);
 /* First step saves; a change waits for the pause... */
 step(0,0);CHECK(writes==1);CHECK(!strcmp(saved_to,UB_STICK_LOG));
 ub_log("t",UB_ALWAYS,"something happened");
 for(int i=0;i<SAVE_PAUSE-1;i++)step(0,0);
 CHECK(writes==1);
 step(0,0);CHECK(writes==2);
 /* an unchanged log is not saved again */
 step(0,0);CHECK(writes==2);
 /* ...unless something urgent was logged (here: by the crash reporter). */
 ub_log("t",UB_ALWAYS,"crash");close(open(UB_LOG_URGENT,O_WRONLY|O_CREAT,0600));
 step(0,0);CHECK(writes==3);CHECK(access(UB_LOG_URGENT,F_OK)!=0);
 /* A failing stick is reported once and retried after a minute. */
 stub_result=7;ub_log("t",UB_ALWAYS,"more");
 for(int i=0;i<SAVE_PAUSE;i++)step(0,0);
 CHECK(writes==4);before=(int)count(log_text(),"trying again every minute");CHECK(before==1);
 for(int i=0;i<SAVE_RETRY-1;i++)step(0,0);
 CHECK(writes==4);
 step(0,0);CHECK(writes==5);CHECK(count(log_text(),"trying again every minute")==1);
 stub_result=0;for(int i=0;i<SAVE_RETRY;i++)step(0,0);
 CHECK(writes==6);CHECK(strstr(log_text(),"saved to the stick again (after 2 failed attempts)"));
 save=SAVE_MENU;
}

static void test_trim(void){
 char line[200];size_t n;
 memset(line,'x',sizeof line-1);line[sizeof line-1]=0;
 for(int i=0;i<1600;i++)ub_log("t",UB_ALWAYS,"%04d %s",i,line);
 n=strlen(log_text());
 CHECK(n<=256*1024);CHECK(n>=128*1024);
 CHECK(strstr(ram,"log trimmed to the newest 128 KB"));CHECK(strstr(ram,"1599 xxx"));CHECK(!strstr(ram,"t: 0000 "));
 /* the kept part starts at a line */
 {const char *p=strstr(ram,"128 KB\n");CHECK(p&&strchr("0123456789 ",p[7]));}
}

static void test_crash(void){
 static unsigned char core[300000];
 static const char maps_text[]="00008000-0000c000 r-xp 00000000 1f:06 123 /usr/dtvrec/bin/apl_test\n"
  "2a340000-2a3a0000 r-xp 00000000 1f:06 5   /usr/dtvrec/lib/libnavi_api.so\n"
  "2a3a0000-2a3b0000 rw-p 00060000 1f:06 5   /usr/dtvrec/lib/libnavi_api.so\n"
  "be800000-be821000 rw-p 00000000 00:00 0   [stack]\n";
 struct crash_map maps[8];struct crash_info ci;char at[80];unsigned n;size_t len=make_core(core,200000);
 int fds[2],status;pid_t victim,feeder;
 CHECK(crash_need(core,52)==84);CHECK(crash_need(core,84)==len-200000);
 CHECK(!crash_parse(core,len,&ci));
 CHECK(ci.tid==4321);CHECK(ci.signo==11);CHECK(ci.code==1);CHECK(ci.have_addr&&ci.addr==0x10);
 CHECK(ci.reg[15]==0x2a38a6d4);CHECK(ci.reg[0]==0x1000);CHECK(!strcmp(ci.fname,"apl_test"));CHECK(!strcmp(ci.args,"/usr/dtvrec/bin/apl_test -x"));
 core[18]=3;CHECK(crash_parse(core,len,&ci));core[18]=40; /* not ARM */
 n=maps_parse(maps_text,maps,8);CHECK(n==4);
 where(maps,n,0x2a38a6d4,at,sizeof at);CHECK(!strcmp(at,"0x2a38a6d4 libnavi_api.so+0x4a6d4"));
 where(maps,n,0x2a3a0010,at,sizeof at);CHECK(!strcmp(at,"0x2a3a0010 libnavi_api.so+0x60010"));
 where(maps,n,0xbe810000,at,sizeof at);CHECK(!strcmp(at,"0xbe810000 [stack]+0x10000"));
 where(maps,n,0x100,at,sizeof at);CHECK(!strcmp(at,"0x00000100"));
 CHECK(!strcmp(signal_name(11),"SIGSEGV"));CHECK(!strcmp(signal_name(77),"signal 77"));
 /* The whole command, as the kernel runs it: core on stdin, for a process
  * that still exists. */
 victim=fork();if(!victim){pause();_exit(0);}
 if(pipe(fds))return;
 feeder=fork();if(!feeder){close(fds[0]);for(size_t o=0;o<len;){ssize_t w=write(fds[1],core+o,len-o);if(w<=0)_exit(1);o+=(size_t)w;}_exit(0);}
 close(fds[1]);
 {
  int saved=dup(0);char pid[16];char *argv[]={"engage","--core",pid,"11","apl","test",0};
  snprintf(pid,sizeof pid,"%d",victim);unlink(UB_LOG_URGENT);
  dup2(fds[0],0);close(fds[0]);
  CHECK(ub_module_log.command(6,argv)==0);
  alarm(0);
  dup2(saved,0);close(saved);
  argv[1]="--other";CHECK(ub_module_log.command(6,argv)==-1);
 }
 waitpid(feeder,&status,0);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status)); /* the whole core was read */
 kill(victim,SIGKILL);waitpid(victim,0,0);
 log_text();
 {char want[120];snprintf(want,sizeof want,"crash: apl test PID%d crashed: SIGSEGV at address 0x00000010 (code 1), thread '' TID4321",victim);CHECK(strstr(ram,want));}
 CHECK(strstr(ram,"crash:   pc 0x2a38a6d4"));CHECK(strstr(ram,"crash:   r0 00001000 r1 00001001"));CHECK(strstr(ram,"crash:   command: /usr/dtvrec/bin/apl_test -x"));
 CHECK(access(UB_LOG_URGENT,F_OK)==0);unlink(UB_LOG_URGENT);
}

static void test_events(void){
 unsigned char buf[256] __attribute__((aligned(8)));struct nlmsghdr *h=(void *)buf;struct cn_msg *cn;struct proc_event *ev;int status;char how[64];
 memset(buf,0,sizeof buf);
 h->nlmsg_len=NLMSG_LENGTH(sizeof *cn+sizeof *ev);h->nlmsg_type=NLMSG_DONE;
 cn=NLMSG_DATA(h);cn->id.idx=CN_IDX_PROC;cn->id.val=CN_VAL_PROC;cn->len=sizeof *ev;
 ev=(void *)cn->data;ev->what=PROC_EVENT_EXIT;ev->event_data.exit.process_pid=777;ev->event_data.exit.process_tgid=777;ev->event_data.exit.exit_code=0x8b;
 CHECK(netlink_parse(buf,h->nlmsg_len)==1);
 ev->event_data.exit.process_pid=778; /* a thread, not a process */
 CHECK(netlink_parse(buf,h->nlmsg_len)==0);
 CHECK(netlink_parse(buf,10)==0);
 CHECK(exit_take(777,&status)&&status==0x8b);CHECK(!exit_take(777,&status));
 exit_text(0x8b,how,sizeof how);CHECK(!strcmp(how,"killed by SIGSEGV (core dumped)"));
 exit_text(0x100,how,sizeof how);CHECK(!strcmp(how,"exit code 1"));
 exit_text(9,how,sizeof how);CHECK(!strcmp(how,"killed by SIGKILL"));
}

static void test_stat_and_kernel(void){
 struct diag_stat s;const char *line="<3>[  12.5] mmc0: timeout";
 CHECK(!stat_parse("123 (a) b (c)) D 45 1 1 0 -1 4194560 10 0 0 0 7 3 0 0 20 0 5 0 999 1048576 100 4294967295 32768 40000 3200000000 3199999000 717344 0 0 0 0 0 0 0 0 17 1 0 0 0 0 0",&s));
 CHECK(!strcmp(s.comm,"a) b (c)"));CHECK(s.state=='D');CHECK(s.ppid==45);CHECK(s.ticks==10);CHECK(s.born==999);CHECK(s.vsize==1048576);CHECK(s.pc==717344);
 CHECK(stat_parse("garbage",&s));
 CHECK(kernel_level(&line)==3&&!strcmp(line,"[  12.5] mmc0: timeout"));
 {
  static const char boot[]="<6>[    0.0] Booting Linux\n<4>[    1.0] warning at boot\n<6>[    2.0] usb ready\n";
  static const char later[]="<6>[    0.0] Booting Linux\n<4>[    1.0] warning at boot\n<6>[    2.0] usb ready\n<6>[    3.0] new info\n<7>[    3.1] debug noise\n<3>[    4.0] I/O error\n<6>[    4.5] unfinished";
  static const char wrapped[]="<6>[    9.0] after wrap\n";
  urgent=0;
  CHECK(klog_lines(boot,sizeof boot-1,1)==1);CHECK(strstr(log_text(),"kernel: [    1.0] warning at boot"));CHECK(!strstr(ram,"Booting Linux"));
  CHECK(klog_lines(later,sizeof later-1,0)==2);log_text();
  CHECK(strstr(ram,"kernel: [    3.0] new info"));CHECK(strstr(ram,"kernel: [    4.0] I/O error"));CHECK(!strstr(ram,"debug noise"));CHECK(!strstr(ram,"unfinished"));
  CHECK(urgent==1);CHECK(count(ram,"usb ready")==0);
  CHECK(klog_lines(later,sizeof later-1,0)==0);
  {static const char finished[]="<6>[    0.0] Booting Linux\n<4>[    1.0] warning at boot\n<6>[    2.0] usb ready\n<6>[    3.0] new info\n<7>[    3.1] debug noise\n<3>[    4.0] I/O error\n<6>[    4.5] unfinished line\n";
   CHECK(klog_lines(finished,sizeof finished-1,0)==1);CHECK(strstr(log_text(),"kernel: [    4.5] unfinished line"));}
  CHECK(klog_lines(wrapped,sizeof wrapped-1,0)==1);CHECK(strstr(log_text(),"newest lines follow"));CHECK(strstr(ram,"after wrap"));
 }
}

/* A process spinning on the CPU (not our child: diagnostics ignore the
 * helper's own children) is reported once it has been busy long enough, and
 * its end is noticed. */
static void test_busy(void){
 int fds[2],spinner=0;pid_t middle;struct timespec second={1,0};char want[96];
 if(pipe(fds))return;
 hz=sysconf(_SC_CLK_TCK);
 diag_scans=0;diag_scan(0);
 middle=fork();
 if(!middle){pid_t g=fork();if(!g){for(volatile unsigned long i=0;;i++){}}if(write(fds[1],&g,sizeof g)<0){}_exit(0);}
 waitpid(middle,0,0);if(read(fds[0],&spinner,sizeof spinner)!=sizeof spinner){CHECK(0);return;}
 for(int i=0;i<DIAG_BUSY_SECONDS+2;i++){unsigned long long t=now_ms();nanosleep(&second,0);diag_scan(now_ms()-t);}
 snprintf(want,sizeof want,"PID%d started",spinner);CHECK(strstr(log_text(),want));
 snprintf(want,sizeof want,"PID%d thread",spinner);CHECK(strstr(ram,want));CHECK(strstr(ram,"has used a whole CPU for 2 s"));
 kill(spinner,SIGKILL);nanosleep(&second,0);diag_scan(1000);
 snprintf(want,sizeof want,"PID%d ended after",spinner);CHECK(strstr(log_text(),want));
 snapshot(60000);CHECK(strstr(log_text(),"processes"));CHECK(strstr(ram,"CPU in the last 60 s:"));
}

/* A program starting itself over and over shows as one "runaway" line and a
 * summary instead of a line per process. */
__attribute__((noreturn)) static void chain(int depth){
 prctl(PR_SET_NAME,"chainproc");prctl(PR_SET_PDEATHSIG,SIGKILL);
 if(depth<14){struct timespec ms={0,1000000};nanosleep(&ms,0);if(!fork())chain(depth+1);}
 for(;;)pause();
}
static void test_chain(void){
 struct timespec second={1,0};pid_t top=fork();
 if(!top){prctl(PR_SET_PDEATHSIG,SIGKILL);if(!fork())chain(1);for(;;)pause();}
 nanosleep(&second,0);diag_scan(1000);nanosleep(&second,0);diag_scan(1000);
 log_text();
 CHECK(strstr(ram,"runaway: a chain of 14 'chainproc' processes, each started by the one before"));
 CHECK(strstr(ram,"... and 6 more processes started in the last second (chainproc x6)"));
 CHECK(count(ram,"diag: chainproc PID")==8);
 kill(top,SIGKILL);waitpid(top,0,0);
}

int main(void){
 ub_log_open();
 ub_log_level=UB_ALWAYS;ub_log("t",UB_INFO,"hidden info");ub_log("t",UB_ALWAYS,"kept error");
 start(&ub_module_log); /* level=info, save=menu, diagnostics=0 by default */
 ub_log("t",UB_INFO,"visible info");ub_log("t",UB_DEBUG,"hidden debug");
 log_text();
 CHECK(!strstr(ram,"hidden info"));CHECK(strstr(ram,"kept error"));CHECK(strstr(ram,"visible info"));CHECK(!strstr(ram,"hidden debug"));
 CHECK(!strstr(ram,"diag:"));
 test_saving();
 /* Mount table: listed on the first call, silent while unchanged. */
 mount_step(0,0);log_text();
 {unsigned listed=count(ram,"log: mount: ");CHECK(listed>0);mount_step(0,0);log_text();CHECK(count(ram,"log: mount: ")==listed);CHECK(!strstr(ram,"mounted: "));}
 test_continuous();
 test_crash();
 test_events();
 test_stat_and_kernel();
 test_busy();
 test_chain();
 test_trim();
 printf("%s (%d failures)\n",failures?"log_test FAILED":"log_test passed",failures);
 return failures!=0;
}
