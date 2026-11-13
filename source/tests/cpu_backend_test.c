/* Local fixtures: all module/node/kcore/proc reads are synthetic own files.
 * No init_module syscall, device access, SSH, firmware or media. */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ub.h"
#include "helper_menu.h"
#undef UB_CPU_ENABLED
#undef UB_CPU_REQUEST
#undef UB_CPU_STATUS
#define UB_CPU_ENABLED "./menu.enabled"
#define UB_CPU_REQUEST "./menu.request"
#define UB_CPU_STATUS "./menu.status"
#include "action_ipc.h"
#include "storage.h"
#define UB_CPU_DIR "."
#define UB_CPU_NODE "./native"
#define UB_CPU_MODULES "./modules"
#include "../modules/cpu/control.h"

static int fixture_open(const char *,int,...);
static ssize_t fixture_write(int,const void *,size_t);
static int fixture_close(int);
static long fixture_syscall(long,...);
static int fixture_uname(struct utsname *);
static int fixture_statfs(const char *,struct statfs *);
static pid_t fixture_fork(void);
static pid_t fixture_waitpid(pid_t,int *,int);
static int fixture_flock(int,int);
#define open fixture_open
#define write fixture_write
#define close fixture_close
#define syscall fixture_syscall
#define uname fixture_uname
#define statfs(...) fixture_statfs(__VA_ARGS__)
#define fork fixture_fork
#define waitpid fixture_waitpid
#define flock fixture_flock
#include "../modules/cpu/module.c"
#undef open
#undef write
#undef close
#undef syscall
#undef uname
#undef statfs
#undef fork
#undef waitpid
#undef flock

static const char fixture_boot[]="00000000-0000-4000-8000-000000000001";
static int module_present,native_mode,node_fd[4096],loads,ons,offs,short_on,short_off,partial_on,stale_on,bad_state,save_calls,save_rc,forks,wait_ready;
static int off_no_effect,manual_off_during_on,disable_during_on;
static int lease_signal_fd=-1,blocking_locks,cleanup_advance_time;
static uint64_t now_ms=100000;
const unsigned char ub_tool_cpu_wait[UB_CPU_ARTIFACT_BYTES]={0};
const size_t ub_tool_cpu_wait_size=sizeof ub_tool_cpu_wait;
static void put16(unsigned char *p,uint16_t n){p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
static void put32(unsigned char *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i));}
static void put64(unsigned char *p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=(unsigned char)(n>>(8*i));}
static void fixture_kcore(void){
 unsigned char h[64]={0},ph[56]={0},word[4];int fd=open("./kcore",O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);
 memcpy(h,"\177ELF\2\1\1",7);put16(h+16,4);put16(h+18,183);put16(h+52,64);put16(h+54,56);put16(h+56,1);put64(h+32,64);
 put32(ph,1);put64(ph+8,256);put64(ph+16,sites[0]);put64(ph+32,sites[1]-sites[0]+4);
 assert(write(fd,h,sizeof h)==sizeof h);assert(write(fd,ph,sizeof ph)==sizeof ph);
 for(unsigned i=0;i<2;i++){uint32_t value=CPU_STOCK_WORD;if(native_mode==1||(native_mode==2&&!i)){int64_t delta=(int64_t)(UINT64_C(0xffffffbffc0d0000)-sites[i]);value=0x14000000u|((uint32_t)(delta/4)&0x03ffffffu);}put32(word,value);assert(pwrite64(fd,word,4,256+(off64_t)(sites[i]-sites[0]))==4);}assert(!close(fd));
}
static void fixture_status_file(void){
 char text[512];int phase=native_mode;
 int n=snprintf(text,sizeof text,"cpu_wait=1 build=1.82 phase=%d pinned_until_reset=%d intended_enabled=%d rc=0 fault=%d begin_rc=0\nfpd pid=9001 bypass=%d rejected=0 observed_task_state=0\nantena pid=9002 bypass=%d rejected=0 observed_task_state=0\n",phase,phase!=0,phase==1,bad_state,phase!=1,phase!=1);
 int fd=open("./native",O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0&&write(fd,text,(size_t)n)==n&&!close(fd));
}
static int fixture_open(const char *path,int flags,...){
 mode_t mode=0;if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=(mode_t)va_arg(ap,int);va_end(ap);}
 int node=!strcmp(path,UB_CPU_NODE);
 if(node){if(!module_present){errno=ENOENT;return -1;}fixture_status_file();}
 int fd=open(!strcmp(path,"/proc/kcore")?"./kcore":path,flags,mode);if(fd>=0){assert(fd<4096);node_fd[fd]=node;}return fd;
}
static int fixture_close(int fd){if(fd>=0&&fd<4096)node_fd[fd]=0;return close(fd);}
static ssize_t fixture_write(int fd,const void *data,size_t n){
 if(fd>=0&&fd<4096&&node_fd[fd]){
  if(n==3&&!memcmp(data,"ON\n",3)){ons++;native_mode=partial_on?2:1;if(partial_on)bad_state=1;fixture_kcore();if(stale_on){struct cpu_owner latest;assert(!read_record(OWNER,&latest,sizeof latest));latest.enabled=0;latest.desired=0;latest.token[0]^=1;assert(!atomic_file(OWNER,&latest,sizeof latest,1));}if(manual_off_during_on)handle_request(CPU_OFF,2,"./reply.2",112);if(disable_during_on)disabled_step(0);if(partial_on){errno=EIO;return -1;}return short_on?2:3;}
  if(n==4&&!memcmp(data,"OFF\n",4)){offs++;if(off_no_effect){errno=EINTR;return off_no_effect==1?-1:2;}native_mode=3;fixture_kcore();return short_off?2:4;}
  assert(!"unexpected native command");return -1;
 }
 return write(fd,data,n);
}
static long fixture_syscall(long number,...){assert(number==SYS_init_module);loads++;module_present=1;native_mode=0;fixture_status_file();fixture_kcore();return 0;}
static int fixture_uname(struct utsname *u){memset(u,0,sizeof *u);strcpy(u->release,"4.4.8");strcpy(u->machine,"armv8l");return 0;}
static int fixture_statfs(const char *p,struct statfs *s){assert(!strcmp(p,"/tmp"));memset(s,0,sizeof *s);s->f_type=0x01021994;return 0;}
static pid_t fixture_fork(void){forks++;wait_ready=0;return 40000+forks;}
static pid_t fixture_waitpid(pid_t pid,int *status,int options){assert(pid==pending.pid&&options==WNOHANG);if(!wait_ready)return 0;*status=0;return pid;}
static int fixture_flock(int fd,int operation){
 if(operation==LOCK_EX){blocking_locks++;if(lease_signal_fd>=0){assert(write(lease_signal_fd,"w",1)==1);assert(!close(lease_signal_fd));lease_signal_fd=-1;}}
 int rc=flock(fd,operation);if(operation==LOCK_EX&&cleanup_advance_time)now_ms=200000;return rc;
}
int ub_fstat(int fd,struct ub_stat *out){struct stat st;if(fstat(fd,&st))return -1;memset(out,0,sizeof *out);out->mode=st.st_mode;out->uid=st.st_uid;out->gid=st.st_gid;out->size=st.st_size;if(fd>=0&&fd<4096&&node_fd[fd]){out->mode=S_IFCHR|0600;out->uid=0;}return 0;}
int ub_stat(const char *path,struct ub_stat *out){int fd=open(path,O_RDONLY);if(fd<0)return -1;int rc=ub_fstat(fd,out);close(fd);return rc;}
unsigned long long ub_now_ms(void){return now_ms;}
void ub_now(struct timespec *t){t->tv_sec=(time_t)(now_ms/1000);t->tv_nsec=0;}
void ub_hash_bytes(const void *p,size_t n,char out[65]){assert(p==ub_tool_cpu_wait&&n==sizeof ub_tool_cpu_wait);memcpy(out,UB_CPU_ARTIFACT_SHA256,65);}
ssize_t ub_read_file(const char *path,char *out,size_t cap){
 char text[4096];int n;
 if(!strcmp(path,"/proc/sys/kernel/random/boot_id"))n=snprintf(text,sizeof text,"%s\n",fixture_boot);
 else if(!strcmp(path,UB_CPU_MODULES)){n=module_present?snprintf(text,sizeof text,"%s %u %u - Live 0xffffffbffc0d0000 (O)\n",UB_CPU_MODULE_NAME,CPU_RESIDENT_BYTES,native_mode?1:0):0;text[n]=0;}
 else if(!strncmp(path,"/proc/",6)&&strstr(path,"/stat")){
  unsigned pid;assert(sscanf(path,"/proc/%u/stat",&pid)==1);n=snprintf(text,sizeof text,"%u (fixture worker) S",pid);
  for(unsigned field=4;field<22;field++)n+=snprintf(text+n,sizeof text-(size_t)n," 0");
  n+=snprintf(text+n,sizeof text-(size_t)n," 500 0\n");
 }else{int fd=open(path,O_RDONLY);if(fd<0)return -1;ssize_t got=read(fd,out,cap-1);close(fd);if(got<0)return -1;out[got]=0;return got;}
 assert((size_t)n<cap);memcpy(out,text,(size_t)n+1);return n;
}
/* Unused parent/IPC dependencies are section-collected; these retain enough
 * implementations to run isolated parent completion fixtures later. */
int ub_dir_open(struct ub_dir *d,const char *p){(void)d;(void)p;return -1;}
const char *ub_dir_next(struct ub_dir *d){(void)d;return 0;}
void ub_dir_close(struct ub_dir *d){(void)d;}
void ub_log(const char *m,int l,const char *f,...){(void)m;(void)l;(void)f;}
int ub_setting_set(const char *m,const char *k,const char *v){assert(!strcmp(m,"cpu")&&!strcmp(k,"optimize"));save_calls++;optimize=!strcmp(v,"on");return save_rc;}
const char *ub_stick_error(int n){(void)n;return "fixture";}

static void reset_fixture(void){
 if(pending.job[0])unlink(pending.job);
 if(pending.result[0])unlink(pending.result);
 memset(&pending,0,sizeof pending);
 unlink(OWNER);unlink(RECEIPT);unlink(LATCH);unlink(LOCK);module_present=native_mode=loads=ons=offs=short_on=short_off=partial_on=stale_on=bad_state=save_calls=save_rc=forks=wait_ready=blocking_locks=cleanup_advance_time=0;lease_signal_fd=-1;
 off_no_effect=manual_off_during_on=disable_during_on=0;
 module_enabled=started=1;effective=-1;optimize=automatic_pending=automatic_blocked=cleanup_pending=0;now_ms=100000;
 memset(node_fd,0,sizeof node_fd);memset(&owner,0,sizeof owner);owner.magic=UB_CPU_MAGIC;owner.version=UB_CPU_VERSION;owner.pid=(unsigned)getpid();owner.born=500;owner.enabled=1;owner.desired=1;memcpy(owner.boot,fixture_boot,37);owner.token[0]=17;assert(!atomic_file(OWNER,&owner,sizeof owner,1));fixture_kcore();
}
static struct cpu_result trial(int op){
 struct cpu_job j;struct cpu_result r;unsigned char raw[16];char nonce[33],path[192];assert(!random_bytes(raw,16));hex_nonce(raw,nonce);
 memset(&j,0,sizeof j);j.magic=UB_CPU_MAGIC;j.version=UB_CPU_VERSION;j.operation=(unsigned)op;j.manual=op==CPU_RECONCILE?0:1;j.deadline=now_ms+15000;j.owner=owner;memcpy(j.nonce,raw,16);
 snprintf(path,sizeof path,"./job.%s",nonce);assert(!atomic_file(path,&j,sizeof j,0));assert(!worker(nonce));assert(!unlink(path));
 snprintf(path,sizeof path,"./result.%s",nonce);assert(!read_record(path,&r,sizeof r));assert(!unlink(path));return r;
}
static void owned_inert(void){struct cpu_instance m;struct cpu_native s;struct cpu_receipt r;assert(!load_inert(owner.boot,&m,&s,&r));loads=0;}
static void parser_tests(void){
 struct cpu_native s;const char *good="cpu_wait=1 build=1.82 phase=3 pinned_until_reset=1 intended_enabled=0 rc=0 fault=0 begin_rc=0\nfpd pid=11 bypass=1 rejected=0 observed_task_state=0\nantena pid=12 bypass=1 rejected=0 observed_task_state=0\n";
 assert(!native_parse(good,&s)&&cpu_native_mode(&s)==0);s.rejected[0]=1;assert(cpu_native_mode(&s)==-1);s.rejected[0]=0;s.pinned=0;assert(cpu_native_mode(&s)==-1);
 char duplicate[1024];snprintf(duplicate,sizeof duplicate,"%sfpd pid=11 bypass=1 rejected=0 observed_task_state=0\n",good);assert(native_parse(duplicate,&s));
 assert(native_parse("cpu_wait=1 build=1.84 phase=0",&s));assert(native_parse("",&s));
 assert(native_parse("cpu_wait=1 build=1.82 phase=4294967296 pinned_until_reset=0 intended_enabled=0 rc=0 fault=0 begin_rc=0\nfpd pid=11 bypass=1 rejected=0 observed_task_state=0\nantena pid=12 bypass=1 rejected=0 observed_task_state=0\n",&s));
 assert(native_parse("cpu_wait=1 build=1.82 phase=0 pinned_until_reset=0 intended_enabled=0 rc=4294967296 fault=0 begin_rc=0\nfpd pid=11 bypass=1 rejected=0 observed_task_state=0\nantena pid=12 bypass=1 rejected=0 observed_task_state=0\n",&s));
 unsigned seq;int op;assert(!parse_request("get 1\n",&op,&seq)&&op==CPU_QUERY&&seq==1);assert(!parse_request("set off 4294967295\n",&op,&seq)&&op==CPU_OFF&&seq==UINT_MAX);
 assert(parse_request("get1\n",&op,&seq));assert(parse_request("seton 1\n",&op,&seq));assert(parse_request("set on 0\n",&op,&seq));assert(parse_request("set on 1 extra\n",&op,&seq));assert(parse_request("set on 4294967296\n",&op,&seq));
 assert(parse_request("set on -1\n",&op,&seq));assert(parse_request("set on 01\n",&op,&seq));
}
static void completed(int mode,int code,int suppress){
 struct cpu_result r;memset(&r,0,sizeof r);r.magic=UB_CPU_MAGIC;r.version=UB_CPU_VERSION;r.mode=mode;r.code=code;r.suppress_auto_on=(unsigned)suppress;memcpy(r.nonce,pending.nonce,16);strcpy(r.message,"fixture native acknowledgement");assert(!atomic_file(pending.result,&r,sizeof r,0));wait_ready=1;
}
static void execute_pending(void){char nonce[33];hex_nonce(pending.nonce,nonce);assert(!worker(nonce));wait_ready=1;}
static void combined_uncertain_off_test(int disable){
 reset_fixture();optimize=1;assert(!begin(CPU_ON,1,1,"./reply.1",110));off_no_effect=disable?2:1;manual_off_during_on=!disable;disable_during_on=disable;
 execute_pending();struct cpu_result r;assert(!read_record(pending.result,&r,sizeof r));assert(r.off_attempted==1&&r.mode==-1&&r.code==CPU_UNCERTAIN&&ons==1&&offs==1&&native_mode==1&&latch_state(owner.boot)==LATCH_OFF);
 poll_worker();assert(!pending.active&&forks==1&&!save_calls&&optimize==1&&effective==-1);
 for(unsigned i=0;i<5;i++)if(disable)disabled_step(0);
 assert(forks==1&&offs==1);
 /* A new helper's saved-Off/disabled startup may query, but must not repeat
  * the previous generation's uncertain native Off automatically. */
 owner.token[0]^=1;assert(!atomic_file(OWNER,&owner,sizeof owner,1));r=trial(CPU_RECONCILE);assert(r.mode==-1&&r.code==CPU_UNCERTAIN&&!r.off_attempted&&offs==1&&native_mode==1&&latch_state(owner.boot)==LATCH_OFF);
 /* A NEW explicit user Off is a separate authorized attempt. */
 manual_off_during_on=disable_during_on=off_no_effect=0;module_enabled=1;assert(!publish_owner(1,0));handle_request(CPU_OFF,3,"./reply.3",112);assert(pending.active&&forks==2);execute_pending();poll_worker();assert(!pending.active&&effective==0&&offs==2&&save_calls==1&&!optimize&&!latch_pending(owner.boot));
 puts(disable?"PASS disabled handoff plus uncertain short Off never repeats automatically; explicit new Off recovers":"PASS deferred manual Off plus EINTR stays unknown without resend/save; explicit new Off recovers");
 unlink("./reply.3");
}
static void corrupt_latch_test(void){
 reset_fixture();owned_inert();struct cpu_result r=trial(CPU_ON);assert(r.mode==1);assert(!publish_owner(1,0));assert(!atomic_file(LATCH,"bad",3,1));r=trial(CPU_RECONCILE);assert(r.mode==-1&&!r.off_attempted&&!offs&&latch_state(owner.boot)==LATCH_INVALID);
 struct cpu_latch l;memset(&l,0,sizeof l);l.magic=UB_CPU_MAGIC;l.version=UB_CPU_VERSION;l.pending=1;l.operation=CPU_QUERY;memcpy(l.boot,owner.boot,40);assert(!atomic_file(LATCH,&l,sizeof l,1));r=trial(CPU_RECONCILE);assert(r.mode==-1&&!r.off_attempted&&!offs&&latch_state(owner.boot)==LATCH_INVALID);
 r=trial(CPU_OFF);assert(r.mode==0&&r.off_attempted==1&&offs==1);assert(!atomic_file(LATCH,"bad",3,1));r=trial(CPU_RECONCILE);assert(r.mode==0&&!r.off_attempted&&offs==1&&latch_state(owner.boot)==LATCH_NONE);puts("PASS corrupt/invalid-operation latch blocks automatic Off; trusted stock Off clears without write");
}
static void parent_tests(void){
 reset_fixture();assert(!begin(CPU_ON,1,1,"./reply.1",110));assert(!save_calls);completed(1,CPU_OK,0);poll_worker();assert(!pending.active&&effective==1&&save_calls==1&&optimize==1);puts("PASS parent saves preference only after exact native acknowledgement");
 reset_fixture();assert(!begin(CPU_ON,1,1,"./reply.1",110));completed(-1,CPU_UNCERTAIN,1);poll_worker();assert(effective==-1&&!save_calls&&automatic_blocked);puts("PASS unknown completion never persists a requested state");
 reset_fixture();assert(!begin(CPU_ON,1,1,"./reply.1",110));now_ms=111000;poll_worker();assert(pending.active&&pending.timed_out&&effective==-1&&forks==1&&!save_calls);completed(1,CPU_OK,0);poll_worker();assert(!pending.active&&effective==1&&!save_calls&&forks==1);puts("PASS deadline remains responsive; late result no resend/no save");
 reset_fixture();assert(!begin(CPU_QUERY,0,1,"./reply.1",110));completed(1,CPU_OK,0);poll_worker();assert(effective==1&&!save_calls&&!optimize);puts("PASS GET completion cannot change persisted preference");
 reset_fixture();optimize=1;automatic_pending=1;assert(!begin(CPU_QUERY,0,0,0,0));completed(0,CPU_OK,0);poll_worker();assert(pending.active&&pending.operation==CPU_ON&&forks==2&&!save_calls);puts("PASS saved On starts only after a conclusive initial query");
 reset_fixture();optimize=1;automatic_pending=1;assert(!begin(CPU_QUERY,0,0,0,0));completed(0,CPU_OK,1);poll_worker();assert(!pending.active&&forks==1&&automatic_blocked);puts("PASS unresolved latch blocks saved-On replay after restart");
 reset_fixture();module_enabled=0;optimize=1;assert(!begin(CPU_RECONCILE,0,0,0,0));completed(0,CPU_OK,0);poll_worker();assert(!pending.active&&optimize==1&&!save_calls&&forks==1);puts("PASS disabled lifecycle verifies Off without changing independent saved On");
 reset_fixture();assert(!begin(CPU_ON,1,1,"./reply.1",110));handle_request(CPU_OFF,2,"./reply.2",112);assert(pending.deferred_off&&pending.expected==0&&!owner.desired);completed(0,CPU_OK,0);poll_worker();assert(!pending.active&&save_calls==1&&!optimize&&effective==0&&forks==1);puts("PASS superseding Off uses the existing worker acknowledgement and saves Off");
 reset_fixture();optimize=1;assert(!begin(CPU_QUERY,0,1,"./reply.1",110));handle_request(CPU_OFF,2,"./reply.2",112);completed(1,CPU_OK,0);poll_worker();assert(pending.active&&pending.operation==CPU_OFF&&forks==2);completed(0,CPU_OK,0);poll_worker();assert(!pending.active&&save_calls==1&&!optimize);puts("PASS Off following a busy read gets one explicit Off, never On");
 reset_fixture();save_rc=3;assert(!begin(CPU_ON,1,1,"./reply.1",110));completed(1,CPU_OK,0);poll_worker();assert(effective==1&&save_calls==1&&strstr(message,"not saved"));puts("PASS USB save failure keeps verified actual state and reports unsaved");
 reset_fixture();module_enabled=0;optimize=1;assert(!begin(CPU_RECONCILE,0,0,0,0));now_ms=200000;for(unsigned i=0;i<10;i++)disabled_step(0);assert(pending.active&&pending.timed_out&&forks==1);completed(0,CPU_OK,0);poll_worker();assert(!pending.active&&effective==0&&optimize==1&&!save_calls);for(unsigned i=0;i<10;i++)disabled_step(0);assert(forks==1);puts("PASS one cleanup waiter survives parent deadline, is reaped, and never resends");
 reset_fixture();assert(!begin(CPU_ON,1,1,"./reply.1",110));handle_request(CPU_OFF,2,"./reply.2",112);completed(-1,CPU_UNCERTAIN,1);int bad=open(pending.result,O_WRONLY|O_TRUNC);assert(bad>=0&&!close(bad));poll_worker();assert(!pending.active&&forks==1&&effective==-1&&!save_calls);puts("PASS missing/invalid old result cannot authorize a deferred Off resend");
 unlink("./reply.1");unlink("./reply.2");unlink(UB_CPU_ENABLED);unlink(UB_CPU_STATUS);
}
static void blocking_cleanup_test(int supersede_on){
 reset_fixture();owned_inert();struct cpu_result r=trial(CPU_ON);assert(r.mode==1&&ons==1);owner.enabled=0;owner.desired=0;assert(!atomic_file(OWNER,&owner,sizeof owner,1));
 int lease=open(LOCK,O_RDWR);assert(lease>=0&&!flock(lease,LOCK_EX|LOCK_NB));int notify[2];assert(!pipe(notify));pid_t child=fork();assert(child>=0);
 if(!child){close(notify[1]);char c;assert(read(notify[0],&c,1)==1);close(notify[0]);if(supersede_on){struct cpu_owner newer=owner;newer.enabled=1;newer.desired=1;newer.token[0]^=1;assert(!atomic_file(OWNER,&newer,sizeof newer,1));}usleep(20000);assert(!flock(lease,LOCK_UN));close(lease);_exit(0);}
 close(notify[0]);lease_signal_fd=notify[1];cleanup_advance_time=1;r=trial(CPU_RECONCILE);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);close(lease);
 assert(blocking_locks==1&&loads==0&&ons==1&&!r.code);
 if(supersede_on){assert(r.mode==1&&offs==0&&native_mode==1);puts("PASS cleanup respects a newer On policy published while waiting for the lease");}
 else{assert(r.mode==0&&offs==1&&native_mode==3&&now_ms==200000);puts("PASS single cleanup waits for lease release then verifies Off after its parent deadline");}
}
int main(void){
 char dir[]="/tmp/ub820-cpu-own-fixture-XXXXXX";assert(mkdtemp(dir)&&!chdir(dir));parser_tests();struct cpu_result r;
 reset_fixture();owner.enabled=0;owner.desired=0;assert(!atomic_file(OWNER,&owner,sizeof owner,1));r=trial(CPU_QUERY);assert(r.mode==0&&!r.code&&!loads&&!ons&&!offs);r=trial(CPU_OFF);assert(r.mode==0&&!r.code&&!loads&&!ons&&!offs);puts("PASS disabled absent query/off: zero load/On");
 reset_fixture();owner.desired=0;assert(!atomic_file(OWNER,&owner,sizeof owner,1));r=trial(CPU_QUERY);assert(r.mode==0&&!loads&&!ons);puts("PASS enabled Off stays lazy");
 reset_fixture();r=trial(CPU_ON);assert(r.mode==1&&!r.code&&loads==1&&ons==1&&!offs&&!latch_pending(owner.boot));r=trial(CPU_ON);assert(r.mode==1&&!r.code&&loads==1&&ons==1);puts("PASS one load/On and native-active adoption without repeat On");
 r=trial(CPU_QUERY);assert(r.mode==1&&!r.code&&ons==1&&!offs);puts("PASS GET is kernel read-only");
 owner.desired=0;assert(!atomic_file(OWNER,&owner,sizeof owner,1));r=trial(CPU_OFF);assert(r.mode==0&&!r.code&&offs==1&&native_mode==3);puts("PASS existing Off restores without unload/reinit");
 reset_fixture();short_on=1;r=trial(CPU_ON);assert(r.mode==-1&&r.code==CPU_UNCERTAIN&&ons==1&&latch_pending(owner.boot));r=trial(CPU_ON);assert(r.mode==-1&&ons==1&&loads==1);owner.desired=0;assert(!atomic_file(OWNER,&owner,sizeof owner,1));r=trial(CPU_OFF);assert(!r.code&&r.mode==0&&offs==1&&!latch_pending(owner.boot));puts("PASS short On latches uncertainty/no resend; explicit verified Off recovers");
 reset_fixture();partial_on=1;r=trial(CPU_ON);assert(r.mode==-1&&r.code==CPU_UNCERTAIN&&ons==1&&native_mode==2&&latch_pending(owner.boot)&&strstr(r.message,"restart player required"));owner.desired=0;assert(!atomic_file(OWNER,&owner,sizeof owner,1));r=trial(CPU_OFF);assert(r.mode==-1&&offs==0&&strstr(r.message,"restart player required")&&latch_pending(owner.boot));r=trial(CPU_QUERY);assert(r.mode==-1&&strstr(r.message,"restart player required")&&!strstr(r.message,"select Off"));puts("PASS true partial/fault with mixed code words stays fail-closed and requires restart");
 reset_fixture();r=trial(CPU_ON);assert(r.mode==1);short_off=1;owner.desired=0;assert(!atomic_file(OWNER,&owner,sizeof owner,1));r=trial(CPU_OFF);assert(r.mode==-1&&r.code==CPU_UNCERTAIN&&r.off_attempted==1&&offs==1&&latch_pending(owner.boot));short_off=0;r=trial(CPU_RECONCILE);assert(r.mode==0&&!r.code&&!r.off_attempted&&offs==1&&!latch_pending(owner.boot));puts("PASS short Off remains unknown and latched; recovery confirms stock words without resend");
 reset_fixture();stale_on=1;r=trial(CPU_ON);assert(!r.code&&r.mode==0&&ons==1&&offs==1&&native_mode==3&&!latch_pending(owner.boot));puts("PASS stale-generation On completion performs Off under same lease");
 reset_fixture();owned_inert();r=trial(CPU_ON);assert(r.mode==1&&ons==1);owner.enabled=0;owner.desired=0;assert(!atomic_file(OWNER,&owner,sizeof owner,1));r=trial(CPU_QUERY);assert(r.mode==1&&offs==0);r=trial(CPU_OFF);assert(r.mode==0&&!r.code&&offs==1);puts("PASS disabled QUERY never mutates; explicit reconciliation Off works");
 reset_fixture();owned_inert();assert(!unlink(RECEIPT));r=trial(CPU_QUERY);assert(r.mode==-1&&!ons&&!offs&&!loads);r=trial(CPU_ON);assert(r.mode==-1&&!ons&&!offs&&!loads);puts("PASS untrusted resident instance never adopted or reloaded");
 reset_fixture();owned_inert();int lock=open(LOCK,O_RDWR|O_CREAT,0600);assert(lock>=0&&!flock(lock,LOCK_EX|LOCK_NB));r=trial(CPU_ON);assert(r.code==CPU_BUSY&&!ons&&!loads);close(lock);puts("PASS worker lease prevents competing mutation");
 reset_fixture();owned_inert();bad_state=1;r=trial(CPU_QUERY);assert(r.mode==-1&&!ons&&!offs);puts("PASS malformed/unhealthy native status never reported Off");
 reset_fixture();struct cpu_owner other=owner;other.token[0]^=1;assert(!atomic_file(OWNER,&other,sizeof other,1));r=trial(CPU_ON);assert(r.code==CPU_STALE&&!loads&&!ons);puts("PASS superseded generation cannot activate");
 blocking_cleanup_test(0);blocking_cleanup_test(1);
 combined_uncertain_off_test(0);combined_uncertain_off_test(1);corrupt_latch_test();
 parent_tests();
 unlink(OWNER);unlink(RECEIPT);unlink(LATCH);unlink(LOCK);unlink("./native");unlink("./kcore");assert(!chdir("/tmp")&&!rmdir(dir));puts("cpu_backend_test passed (31 scenarios)");return 0;
}
