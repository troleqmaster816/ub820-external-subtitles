/* Host test of core/config.c's player path (the stick reached through the
 * storage layer, stubbed here): a failed read of sub/ub820_menu.cfg at start
 * never lets a later menu choice replace that file with only the choices
 * known in RAM; the choice waits and is merged into the file once it reads.
 * A stick without the file (absence shown by a listing, code 12) is written
 * at once. ub_setting_set: 0 saved, -1 invalid, >0 applied but not saved. */
#define _GNU_SOURCE
#include "ub.h"
#include <stdio.h>
static FILE *config_fopen(const char *path,const char *mode);
#define fopen config_fopen
#undef UB_HOST_TEST
#include "../core/config.c"
#undef fopen
#include <sys/wait.h>

static int brightness=100,auto_off=1,armed;
static const struct ub_option brightness_options[]={{"brightness",0,100,&brightness,0},{0,0,0,0,0}};
static const struct ub_option power_options[]={{"auto_off",0,1,&auto_off,0},{0,0,0,0,0}};
static struct ub_module subtitles={"external_subtitles","",1,brightness_options,0,0,0,0,0};
static struct ub_module power={"power","",1,power_options,0,0,0,0,0};
static const struct ub_option cpu_options[]={{"auto_on_armed",0,1,&armed,0},{0,0,0,0,0}};
static struct ub_module cpu={"cpu","",1,cpu_options,0,0,0,0,0};
struct ub_module *const ub_modules[]={&subtitles,&power,&cpu,0};
static int rejected_whole;
void ub_log(const char *module,int level,const char *format,...){(void)module;(void)level;if(strstr(format,"entire file ignored"))rejected_whole++;}

static int read_rc,write_rc,writes,absent;
static char stick[8192]="[external_subtitles]\nbrightness=50\n[power]\nauto_off=0\n";
static int fail_ram_read;
static ssize_t read_prefix(void *cookie,char *buf,size_t size){
 FILE *f=cookie;
 if(ftell(f)>0){errno=EIO;return -1;}
 size_t prefix=strlen("[external_subtitles]\nbrightness=50\n");
 return (ssize_t)fread(buf,1,size<prefix?size:prefix,f);
}
static int close_prefix(void *cookie){return fclose(cookie);}
static FILE *config_fopen(const char *path,const char *mode){
 FILE *f=fopen(path,mode);
 if(f&&fail_ram_read&&!strcmp(mode,"r")&&!strcmp(path,UB_CONFIG_DIR "/" UB_MENU_FILE)){
  FILE *stream=fopencookie(f,"r",(cookie_io_functions_t){.read=read_prefix,.close=close_prefix});
  if(!stream)fclose(f);
  return stream;
 }
 return f;
}
static int fake_read(const char *from,const char *to){
 FILE *f;
 if(!strstr(from,UB_MENU_FILE))return UB_STICK_ABSENT;
 if(read_rc)return read_rc;
 if(absent)return UB_STICK_ABSENT;
 f=fopen(to,"w");if(!f)return 5;fputs(stick,f);fclose(f);return 0;
}
int ub_stick_read(const char *from,const char *to){return fake_read(from,to);}
int ub_stick_read_quiet(const char *from,const char *to){return fake_read(from,to);}
static int loads;
int ub_stick_load(const char *from,const char *to,int quiet){(void)quiet;loads++;return fake_read(from,to);}
int ub_stick_write(const char *from,const char *to){
 FILE *f;size_t n;
 if(!strstr(to,UB_MENU_FILE))return 5;
 writes++;if(write_rc)return write_rc;
 f=fopen(from,"r");if(!f)return 4;n=fread(stick,1,sizeof stick-1,f);stick[n]=0;fclose(f);absent=0;return 0;
}
int ub_stick_write_quiet(const char *from,const char *to){return ub_stick_write(from,to);}
const char *ub_stick_error(int code){(void)code;return "fixture";}

static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL config_stick_test:%d: %s\n",__LINE__,#c);failures++;}}while(0)
/* Each case in its own process: config.c keeps its state for one run. */
static void run(void (*test)(void)){
 pid_t pid;int status;fflush(stdout);
 pid=fork();if(!pid){failures=0;mkdir(UB_CONFIG_DIR,0700);test();fflush(stdout);_exit(failures?1:0);}
 waitpid(pid,&status,0);if(!WIFEXITED(status)||WEXITSTATUS(status))failures++;
}
static void unreadable_then_readable(void){
 read_rc=4;ub_config_load();
 CHECK(auto_off==1&&brightness==100&&!ub_menu_known_at_start());
 CHECK(ub_setting_set("external_subtitles","brightness","70")==UB_STICK_UNREAD);
 CHECK(brightness==70&&!writes&&strstr(stick,"auto_off=0")&&strstr(stick,"brightness=50"));
 read_rc=14;CHECK(ub_setting_set("external_subtitles","brightness","75")==UB_STICK_UNREAD);CHECK(!writes);
 /* Readable again: merged, this run's choice wins, the older one is kept. */
 read_rc=0;CHECK(ub_setting_set("external_subtitles","brightness","80")==0);
 CHECK(writes==1&&strstr(stick,"auto_off=0")&&strstr(stick,"brightness=80")&&!strstr(stick,"brightness=50"));
 CHECK(!ub_menu_known_at_start()); /* a later save does not change what startup knew */
 CHECK(auto_off==1); /* merged into the file only; it applies at the next start */
 /* Later saves need no read. */
 read_rc=4;CHECK(ub_setting_set("power","auto_off","1")==0);CHECK(writes==2&&strstr(stick,"auto_off=1")&&strstr(stick,"brightness=80"));
}
static void new_stick(void){
 absent=1;ub_config_load();
 CHECK(ub_menu_known_at_start());
 CHECK(ub_setting_set("power","auto_off","0")==0);CHECK(writes==1&&strstr(stick,"auto_off=0")&&!strstr(stick,"brightness"));
}
static void read_at_start(void){
 ub_config_load();CHECK(auto_off==0&&brightness==50&&ub_menu_known_at_start());
 CHECK(ub_setting_set("external_subtitles","brightness","60")==0);CHECK(strstr(stick,"auto_off=0")&&strstr(stick,"brightness=60"));
}
static void storage_failures(void){
 ub_config_load();
 /* Tool failures are "applied, not saved", never "invalid". */
 write_rc=UB_STICK_TIMEOUT;CHECK(ub_setting_set("external_subtitles","brightness","61")==UB_STICK_TIMEOUT&&brightness==61);
 write_rc=UB_STICK_NO_TOOL;CHECK(ub_setting_set("external_subtitles","brightness","62")==UB_STICK_NO_TOOL&&brightness==62);
 write_rc=-2;CHECK(ub_setting_set("external_subtitles","brightness","63")>0&&brightness==63); /* an old storage layer */
 write_rc=0;CHECK(ub_setting_set("external_subtitles","brightness","101")==-1&&brightness==63);
 CHECK(ub_setting_set("external_subtitles","brightness","64")==0&&strstr(stick,"brightness=64"));
}
/* Repeated keys: the last entry wins, at start and when the file is merged
 * after a failed read; only this run's choices win over the file. */
#define REPEATED "[power]\nauto_off=0\nauto_off=1\n[external_subtitles]\nbrightness=40\nbrightness=45\n"
static void repeated_keys(void){
 strcpy(stick,REPEATED);
 ub_config_load();CHECK(auto_off==1&&brightness==45);CHECK(loads==1);
 CHECK(ub_setting_set("external_subtitles","brightness","55")==0);CHECK(strstr(stick,"auto_off=1")&&strstr(stick,"brightness=55"));
}
static void repeated_keys_after_failed_read(void){
 strcpy(stick,REPEATED);
 read_rc=4;ub_config_load();read_rc=0;
 CHECK(ub_setting_set("external_subtitles","brightness","55")==0);
 CHECK(strstr(stick,"auto_off=1")&&!strstr(stick,"auto_off=0")&&strstr(stick,"brightness=55"));
}
/* A file longer than the parser reads is never replaced by its first part. */
static void too_long(void){
 char *p=stick;p+=sprintf(p,"[external_subtitles]\nbrightness=40\n");
 for(int i=0;i<300;i++)p+=sprintf(p,"#\n");
 sprintf(p,"[power]\nauto_off=0\n");
 ub_config_load();
 CHECK(brightness==100&&auto_off==1&&rejected_whole==1);
 CHECK(ub_setting_set("external_subtitles","brightness","55")==UB_STICK_UNREAD);CHECK(!writes&&strstr(stick,"auto_off=0"));
 read_rc=4;ub_config_load();read_rc=0; /* the same after a failed first read */
 CHECK(ub_setting_set("external_subtitles","brightness","56")==UB_STICK_UNREAD);CHECK(!writes&&strstr(stick,"auto_off=0"));
}
static void incomplete_ram_read(void){
 fail_ram_read=1;ub_config_load();CHECK(!ub_menu_known_at_start()&&brightness==100);
 CHECK(ub_setting_set("external_subtitles","brightness","62")==UB_STICK_UNREAD);
 CHECK(!writes&&strstr(stick,"auto_off=0"));
 fail_ram_read=0;
 CHECK(ub_setting_set("external_subtitles","brightness","62")==0);
 CHECK(writes==1&&strstr(stick,"auto_off=0")&&strstr(stick,"brightness=62"));
}
static void edited_after_partial_read(void){
 fail_ram_read=1;ub_config_load();
 CHECK(brightness==100);
 CHECK(ub_setting_set("power","auto_off","0")==UB_STICK_UNREAD);
 fail_ram_read=0;strcpy(stick,"[power]\nauto_off=1\n");
 CHECK(ub_setting_set("power","auto_off","0")==0);
 CHECK(writes==1&&strstr(stick,"auto_off=0")&&!strstr(stick,"brightness"));
}
static void deleted_after_partial_read(void){
 fail_ram_read=1;ub_config_load();CHECK(brightness==100);
 CHECK(ub_setting_set("power","auto_off","0")==UB_STICK_UNREAD);
 fail_ram_read=0;absent=1;
 CHECK(ub_setting_set("external_subtitles","enabled","0")==0);
 CHECK(writes==1&&strstr(stick,"auto_off=0")&&strstr(stick,"enabled=0")&&!strstr(stick,"brightness"));
}
static void failed_prerequisite(void){
 absent=1;ub_config_load();write_rc=7;
 CHECK(ub_setting_set_saved("cpu","auto_on_armed","1")==7&&!armed);
 write_rc=0;CHECK(!ub_setting_set("power","auto_off","0"));
 CHECK(!strstr(stick,"auto_on_armed=1"));
 CHECK(!ub_setting_set_saved("cpu","auto_on_armed","1")&&armed);
 write_rc=UB_STICK_TIMEOUT;CHECK(ub_setting_set_saved("cpu","auto_on_armed","0")==UB_STICK_TIMEOUT&&armed);
 write_rc=0;CHECK(!ub_setting_set("power","auto_off","1")&&strstr(stick,"auto_on_armed=1"));
}
int main(void){
 run(failed_prerequisite);
 run(unreadable_then_readable);
 run(new_stick);
 run(read_at_start);
 run(storage_failures);
 run(repeated_keys);
 run(repeated_keys_after_failed_read);
 run(too_long);
 run(incomplete_ram_read);
 run(edited_after_partial_read);
 run(deleted_after_partial_read);
 printf("config_stick_test %s (%d failures)\n",failures?"FAILED":"passed",failures);return failures!=0;
}
