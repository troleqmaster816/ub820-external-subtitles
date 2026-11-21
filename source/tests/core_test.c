/* Host test of core/: config, process tracking, hash gate, circuit breaker.
 * Fake modules run against forked children named "fake_ui", whose
 * /proc/<pid>/exe is this test binary. Built by tests/run_tests.py. */
#define _GNU_SOURCE
#include "ub.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);failures++;}}while(0)
static char self_sha[65];

/* Options for the config test, under the ids the legacy keys map to. */
static int font=122,position=-8,usb=1,level=1,count=3;
static const char *const levels[]={"errors","info","debug",0};
static const struct ub_option ext_options[]={{"font_percent",90,150,&font,0},{"position",-10,10,&position,0},{0,0,0,0,0}};
static const struct ub_option log_options[]={{"save",0,2,&usb,0},{"level",0,2,&level,levels},{"count",0,9,&count,0},{0,0,0,0,0}};

static int steps[4];
static int clock_steps;
static int enabled_starts,disabled_starts,disabled_calls,enabled_disabled_calls;
static int disabled_part_calls,disabled_pending,disabled_completed;
static void enabled_start(struct ub_module *m){CHECK(m->enabled);enabled_starts++;}
static void forbidden_disabled_start(struct ub_module *m){(void)m;disabled_starts++;}
static void disabled_part(struct ub_part *p,struct ub_target *t){(void)p;(void)t;disabled_part_calls++;}
static void disabled_housekeeping(struct ub_module *m){
 if(m->enabled){enabled_disabled_calls++;return;}
 disabled_calls++;
 if(disabled_calls==1){disabled_pending=1;return;}
 if(disabled_pending){disabled_pending=0;disabled_completed++;}
}
static void step_a(struct ub_part *p,struct ub_target *t){steps[0]++;if(!t->state[0]++)ub_changed(p);}
static void step_b(struct ub_part *p,struct ub_target *t){steps[1]++;if(t->state[0]++==1)ub_changed(p);}
static void step_c(struct ub_part *p,struct ub_target *t){(void)p;(void)t;steps[2]++;}
static void step_d(struct ub_part *p,struct ub_target *t){steps[3]++;if(!t->state[0]++)ub_changed(p);}
static void clock_step(struct ub_part *p,struct ub_target *t){clock_steps++;if(!t->state[0]++)ub_changed(p);}
static void prepare_defer(struct ub_part *p,struct ub_target *t){
 (void)p;if(!t->state[0]++)t->retry=1;else t->failed=1;
}
static void prepare_fail(struct ub_part *p,struct ub_target *t){(void)p;t->failed=1;}
static const struct ub_payload_variant fixture_variant={.firmware="1.82",.sha256=self_sha};
static const struct ub_payload fixture_payload={.process="fake_ui",.sha256=self_sha,.variants=&fixture_variant,.variant_count=1};

static struct ub_part ext_parts[]={
 {"a","fake_ui",self_sha,0,1,0,step_a,{0},0,0},
 {"b","fake_ui",self_sha,0,1,0,step_b,{0},0,0},
 {"c","fake_ui","0000000000000000000000000000000000000000000000000000000000000000",0,1,0,step_c,{0},0,0},
};
static struct ub_part plain_parts[]={
 {"d","fake_ui",self_sha,0,0,0,step_d,{0},0,0},
 {"defer","fake_ui",self_sha,&fixture_payload,0,prepare_defer,0,{0},0,0},
 {"refuse","fake_ui",self_sha,&fixture_payload,0,prepare_fail,0,{0},0,0},
};
static struct ub_part disabled_parts[]={{"disabled",0,0,0,0,0,disabled_part,{0},0,0}};
/* Same lifecycle policy as the per-disc precise clock; the real module's
 * descriptor is checked by subtitle_startup_test. */
static struct ub_part clock_parts[]={{"per-disc clock","fake_clock",self_sha,0,0,0,clock_step,{0},0,0}};
static struct ub_module ext={"external_subtitles","test",1,ext_options,enabled_start,ext_parts,3,0,0};
static struct ub_module logm={"log","test",1,log_options,0,0,0,0,0};
static struct ub_module plain={"plain","test",1,0,0,plain_parts,3,0,disabled_housekeeping};
static struct ub_module off={"other","test",1,0,forbidden_disabled_start,disabled_parts,1,0,disabled_housekeeping};
static struct ub_module clockm={"clock","test",1,0,0,clock_parts,1,0,0};
/* Hook sites of a process that already carries changed code (an earlier
 * helper run): refused once, before any stop and before prepare. */
#define SITE 0x13000000u
static const unsigned char stock_site[4]={0x2d,0xe9,0xf0,0x41},hooked_site[4]={0xdf,0xf8,0x00,0xf0};
static const struct ub_hook_spec site_hook={SITE,4,"r","o",stock_site,stock_site,0};
static const struct ub_payload_variant site_variant={.firmware="1.82",.sha256=self_sha,.hooks=&site_hook,.hook_count=1};
static const struct ub_payload site_payload={.process="fake_hooked",.sha256=self_sha,.variants=&site_variant,.variant_count=1};
static int site_prepares;
static void prepare_count(struct ub_part *p,struct ub_target *t){(void)p;(void)t;site_prepares++;}
static struct ub_part site_parts[]={{"engine","fake_hooked",self_sha,&site_payload,1,prepare_count,0,{0},0,0}};
static struct ub_module sitem={"sites","test",1,0,0,site_parts,1,0,0};
struct ub_module *const ub_modules[]={&logm,&ext,&plain,&off,&clockm,&sitem,0};
int ub_core_lock(const char *path);

static int named_child(const char *name){
 pid_t pid=fork();
 if(!pid){prctl(PR_SET_NAME,name);for(;;)pause();}
 usleep(100000);return pid;
}
static int child(void){return named_child("fake_ui");}
static int log_has(const char *text){
 static char buf[65536];FILE *f=fopen(UB_LOG_FILE,"r");size_t n=f?fread(buf,1,sizeof buf-1,f):0;if(f)fclose(f);buf[n]=0;return strstr(buf,text)!=0;
}
static void stop(int pid){kill(pid,SIGKILL);waitpid(pid,0,0);}
/* A ready handshake avoids timing assumptions; EOF requests an ordinary
 * successful exit, just as ending a disc session ends its player process. */
static int clock_child(int *finish){
 int ready[2],command[2];char value;pid_t pid;
 if(pipe(ready)||pipe(command))return -1;
 pid=fork();
 if(!pid){
  close(ready[0]);close(command[1]);prctl(PR_SET_NAME,"fake_clock");
  if(write(ready[1],"1",1)!=1)_exit(2);
  close(ready[1]);
  _exit(read(command[0],&value,1)==0?0:3);
 }
 close(ready[1]);close(command[0]);
 CHECK(pid>0);CHECK(read(ready[0],&value,1)==1);close(ready[0]);
 *finish=command[1];return pid;
}
static void clock_finish(int pid,int finish){
 int status;close(finish);CHECK(waitpid(pid,&status,0)==pid);
 CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
static void write_file(const char *path,const char *text){FILE *f=fopen(path,"w");fputs(text,f);fclose(f);}

int main(void){
 int pid,finish;
 if(ub_hash_file("/proc/self/exe",self_sha)){puts("cannot hash self");return 1;}
 /* Config: legacy keys first, modules.cfg wins; names, ranges, unknown keys. */
 write_file(UB_CONFIG_DIR "/disc_style.cfg","font_percent=130\nposition=-5 # comment\nusb_log=0\nnative_options=1\n");
 /* A 255-character comment line fills the parser's buffer; its remainder
  * "count=9" must not be read as a setting of its own. */
 {char cfg[1024],comment[256];memset(comment,'x',sizeof comment);comment[0]='#';comment[1]=' ';comment[255]=0;
  snprintf(cfg,sizeof cfg,"# test\n[external_subtitles]\nposition=-3\nfont_percent=500\n[log]\nlevel=debug\ncount=7\n%scount=9\nbogus=1\n[other]\nenabled=0\n[missing]\nenabled=1\n",comment);
  write_file(UB_CONFIG_DIR "/modules.cfg",cfg);}
 ub_log_open();
 signal(SIGCHLD,SIG_IGN);sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGCHLD);sigprocmask(SIG_BLOCK,&blocked,0);
 ub_core_start();
 struct sigaction disposition;sigaction(SIGCHLD,0,&disposition);CHECK(disposition.sa_handler==SIG_DFL);
 sigset_t current;sigprocmask(SIG_SETMASK,0,&current);CHECK(!sigismember(&current,SIGCHLD));
 CHECK(font==130);CHECK(position==-3);CHECK(usb==0);CHECK(level==2);CHECK(count==7);CHECK(!off.enabled);CHECK(ext.enabled);
 CHECK(ub_module_enabled("plain"));CHECK(!ub_module_enabled("other"));CHECK(!ub_module_enabled("missing"));
 /* Disabled housekeeping runs after configuration, without starting its
  * feature or parts. Enabled modules retain their normal lifecycle. */
 CHECK(enabled_starts==1);CHECK(!disabled_starts);CHECK(!disabled_part_calls);
 CHECK(disabled_calls==1&&disabled_pending&&!disabled_completed);CHECK(!enabled_disabled_calls);

 /* First instance: a then b change it; c's hash does not match. */
 pid=child();
 for(int i=0;i<3;i++)ub_core_tick();
 CHECK(steps[0]==3);CHECK(steps[1]==3);CHECK(steps[2]==0);CHECK(steps[3]==3);
 CHECK(disabled_calls==4&&disabled_completed==1&&!disabled_pending);
 CHECK(!disabled_starts&&!disabled_part_calls&&!enabled_disabled_calls&&enabled_starts==1);
 CHECK(ext_parts[2].target.verified==-1);
 CHECK(plain_parts[1].target.state[0]==2&&plain_parts[1].target.failed);
 CHECK(!plain_parts[1].target.attempts&&!plain_parts[1].target.installed);
 CHECK(plain_parts[2].target.failed&&!plain_parts[2].target.attempts&&!plain_parts[2].target.installed);
 CHECK(ext_parts[0].target.changed&&ext_parts[1].target.changed>ext_parts[0].target.changed);
 CHECK(ub_settled("fake_ui"));
 stop(pid);ub_core_tick();
 /* b changed it last, so b is switched off; a and the unguarded d stay. */
 CHECK(ext_parts[1].tripped);CHECK(!ext_parts[0].tripped);CHECK(!plain_parts[0].tripped);
 /* The end and the breaker's choice are separate log lines. */
 {char want[96];snprintf(want,sizeof want,"fake_ui PID%d ended (no longer running)",pid);CHECK(log_has(want));}
 CHECK(log_has("circuit breaker: part 'b' made the latest guarded change"));

 /* Second instance: fresh state for a, nothing for b; a is next in line. */
 pid=child();
 for(int i=0;i<2;i++)ub_core_tick();
 CHECK(steps[0]==5);CHECK(steps[1]==3);CHECK(ext_parts[0].target.state[0]==2);
 stop(pid);ub_core_tick();
 CHECK(ext_parts[0].tripped);CHECK(!plain_parts[0].tripped);

 /* Third instance: only the unguarded part still runs. */
 pid=child();ub_core_tick();
 CHECK(steps[0]==5);CHECK(steps[3]==6); /* d: 3 + 2 + 1 ticks */
 stop(pid);ub_core_tick();
 CHECK(!plain_parts[0].tripped); /* unguarded parts are never blamed */
 CHECK(disabled_completed==1&&!disabled_pending); /* finite cleanup stays finished */
 CHECK(!disabled_starts&&!disabled_part_calls&&!enabled_disabled_calls&&enabled_starts==1);

 /* An ordinary disc-process exit resets its instance without disabling the
  * unguarded clock. The next disc gets freshly verified state and runs. */
 pid=clock_child(&finish);CHECK(pid>0);
 ub_core_tick();ub_core_tick();
 CHECK(clock_steps==2&&clock_parts[0].target.pid==pid);
 CHECK(clock_parts[0].target.verified==1&&clock_parts[0].target.state[0]==2);
 CHECK(clock_parts[0].target.changed&&!clock_parts[0].tripped);
 clock_finish(pid,finish);ub_core_tick();
 CHECK(!clock_parts[0].target.pid&&!clock_parts[0].tripped);
 pid=clock_child(&finish);CHECK(pid>0);ub_core_tick();
 CHECK(clock_steps==3&&clock_parts[0].target.pid==pid);
 CHECK(clock_parts[0].target.verified==1&&clock_parts[0].target.state[0]==1);
 CHECK(clock_parts[0].target.changed&&!clock_parts[0].tripped);
 clock_finish(pid,finish);ub_core_tick();
 CHECK(!clock_parts[0].target.pid&&!clock_parts[0].tripped);

 /* Already hooked (not stock): one refusal, no prepare, no retries. */
 {unsigned char *site=mmap((void *)(uintptr_t)SITE,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(site==(void *)(uintptr_t)SITE);
  memcpy(site,hooked_site,4);pid=named_child("fake_hooked");
  for(int i=0;i<4;i++)ub_core_tick();
  CHECK(site_parts[0].target.pid==pid&&site_parts[0].target.failed);CHECK(!site_parts[0].target.attempts&&!site_parts[0].target.installed);
  CHECK(!site_prepares);CHECK(log_has("already has changed code (hook-sites: 13000000 is not stock code)"));
  stop(pid);ub_core_tick();
  /* Stock code: prepare runs and installation is attempted (this fixture has
   * no payload image, so the attempt fails and is retried). */
  memcpy(site,stock_site,4);pid=named_child("fake_hooked");ub_core_tick();
  CHECK(site_prepares==1);CHECK(site_parts[0].target.attempts==1&&!site_parts[0].target.failed);
  stop(pid);ub_core_tick();munmap(site,4096);}

 /* The single-instance lock is close-on-exec and exclusive. */
 {int fd=ub_core_lock(UB_CONFIG_DIR "/test.lock");CHECK(fd>=0);CHECK(fd>=0&&(fcntl(fd,F_GETFD)&FD_CLOEXEC));
  CHECK(ub_core_lock(UB_CONFIG_DIR "/test.lock")==-1);if(fd>=0)close(fd);
  fd=ub_core_lock(UB_CONFIG_DIR "/test.lock");CHECK(fd>=0);if(fd>=0)close(fd);}
 /* Started with stdin closed: fd 0 is filled, the lock lies above 2. */
 {int saved=dup(0),fd;close(0);fd=ub_core_lock(UB_CONFIG_DIR "/test.lock");
  CHECK(fd>2&&fcntl(0,F_GETFD)>=0&&!(fcntl(0,F_GETFD)&FD_CLOEXEC));if(fd>=0)close(fd);
  close(0);dup2(saved,0);close(saved);}

 printf("%s (%d failures)\n",failures?"core_test FAILED":"core_test passed",failures);
 return failures!=0;
}
