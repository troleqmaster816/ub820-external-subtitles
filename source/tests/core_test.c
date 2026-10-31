/* Host test of core/: config, process tracking, hash gate, circuit breaker.
 * Fake modules run against forked children named "fake_ui", whose
 * /proc/<pid>/exe is this test binary. Built by tests/run_tests.py. */
#define _GNU_SOURCE
#include "ub.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static void step_a(struct ub_part *p,struct ub_target *t){steps[0]++;if(!t->state[0]++)ub_changed(p);}
static void step_b(struct ub_part *p,struct ub_target *t){steps[1]++;if(t->state[0]++==1)ub_changed(p);}
static void step_c(struct ub_part *p,struct ub_target *t){(void)p;(void)t;steps[2]++;}
static void step_d(struct ub_part *p,struct ub_target *t){steps[3]++;if(!t->state[0]++)ub_changed(p);}

static struct ub_part ext_parts[]={
 {"a","fake_ui",self_sha,0,1,0,step_a,{0},0,0},
 {"b","fake_ui",self_sha,0,1,0,step_b,{0},0,0},
 {"c","fake_ui","0000000000000000000000000000000000000000000000000000000000000000",0,1,0,step_c,{0},0,0},
};
static struct ub_part plain_parts[]={{"d","fake_ui",self_sha,0,0,0,step_d,{0},0,0}};
static struct ub_module ext={"external_subtitles","test",1,ext_options,0,ext_parts,3,0};
static struct ub_module logm={"log","test",1,log_options,0,0,0,0};
static struct ub_module plain={"plain","test",1,0,0,plain_parts,1,0};
static struct ub_module off={"other","test",1,0,0,0,0,0};
struct ub_module *const ub_modules[]={&logm,&ext,&plain,&off,0};

static int child(void){
 pid_t pid=fork();
 if(!pid){prctl(PR_SET_NAME,"fake_ui");for(;;)pause();}
 usleep(100000);return pid;
}
static void stop(int pid){kill(pid,SIGKILL);waitpid(pid,0,0);}
static void write_file(const char *path,const char *text){FILE *f=fopen(path,"w");fputs(text,f);fclose(f);}

int main(void){
 int pid;
 if(ub_hash_file("/proc/self/exe",self_sha)){puts("cannot hash self");return 1;}
 /* Config: legacy keys first, modules.cfg wins; names, ranges, unknown keys. */
 write_file(UB_CONFIG_DIR "/disc_style.cfg","font_percent=130\nposition=-5 # comment\nusb_log=0\nnative_options=1\n");
 write_file(UB_CONFIG_DIR "/modules.cfg","# test\n[external_subtitles]\nposition=-3\nfont_percent=500\n[log]\nlevel=debug\ncount=7\nbogus=1\n[other]\nenabled=0\n[missing]\nenabled=1\n");
 ub_log_open();
 ub_core_start();
 CHECK(font==130);CHECK(position==-3);CHECK(usb==0);CHECK(level==2);CHECK(count==7);CHECK(!off.enabled);CHECK(ext.enabled);
 CHECK(ub_module_enabled("plain"));CHECK(!ub_module_enabled("other"));CHECK(!ub_module_enabled("missing"));

 /* First instance: a then b change it; c's hash does not match. */
 pid=child();
 for(int i=0;i<3;i++)ub_core_tick();
 CHECK(steps[0]==3);CHECK(steps[1]==3);CHECK(steps[2]==0);CHECK(steps[3]==3);
 CHECK(ext_parts[2].target.verified==-1);
 CHECK(ext_parts[0].target.changed&&ext_parts[1].target.changed>ext_parts[0].target.changed);
 CHECK(ub_settled("fake_ui"));
 stop(pid);ub_core_tick();
 /* b changed it last, so b is switched off; a and the unguarded d stay. */
 CHECK(ext_parts[1].tripped);CHECK(!ext_parts[0].tripped);CHECK(!plain_parts[0].tripped);

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

 printf("%s (%d failures)\n",failures?"core_test FAILED":"core_test passed",failures);
 return failures!=0;
}
