/* Actual supervisor state machine; only own temporary files and mocked child,
 * library identity and configuration functions. No EV/native code execution. */
#define _GNU_SOURCE
#ifndef UB_HOST_TEST
#error "Power supervisor fixtures require UB_HOST_TEST"
#endif
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ub.h"
#include "helper_menu.h"
#include "../modules/power/power_protocol.h"
#undef UB_POWER_ENABLED
#undef UB_POWER_REQUEST
#undef UB_POWER_STATUS
#undef UB_POWER_TOKEN
#undef UB_POWER_NATIVE
#undef UB_POWER_PACKET
#undef UB_POWER_TOOL
static char directory[96],enabled_file[128],request_file[128],status_file[128],token_file[128],native_file[128],packet_file[128],tool_file[128];
#define UB_POWER_ENABLED enabled_file
#define UB_POWER_REQUEST request_file
#define UB_POWER_STATUS status_file
#define UB_POWER_TOKEN token_file
#define UB_POWER_NATIVE native_file
#define UB_POWER_PACKET packet_file
#define UB_POWER_TOOL tool_file
static unsigned seconds=1000,forks,reaps,saves,hashes,maps;
static int fork_failure,wait_ready,wait_code,wait_missing,cohort185,wrong_manager,wrong_base,wrong_path,wrong_library,save_error;
static pid_t virtual_child=10000;
static void fixture_now(struct timespec *out){out->tv_sec=seconds;out->tv_nsec=0;}
static unsigned long long fixture_now_ms(void){return (unsigned long long)seconds*1000;}
static pid_t fixture_fork(void){forks++;if(fork_failure){errno=EAGAIN;return -1;}return ++virtual_child;}
static pid_t fixture_waitpid(pid_t pid,int *status,int options){
 assert(pid>10000&&options==WNOHANG);
 if(wait_missing){errno=ECHILD;return -1;}
 if(!wait_ready)return 0;
 reaps++;*status=wait_code<<8;return pid;
}
#define ub_now fixture_now
#define ub_now_ms fixture_now_ms
#define fork fixture_fork
#define waitpid fixture_waitpid
#include "../modules/power/module.c"
#undef waitpid
#undef fork
#undef ub_now_ms
#undef ub_now

static const char SHA185[]="1111111111111111111111111111111111111111111111111111111111111111";
static const struct ub_payload_variant variants[]={
 {"1.82",UB_SHA256_APL_MGR,0,0,0,0,0},{"1.85",SHA185,0,0,0,0,0}
};
const struct ub_payload ub_payload_power_policy={"apl_mgr",UB_SHA256_APL_MGR,0,0,variants,2};
const unsigned char ub_tool_events[]={0x7f,'O','W','N'};
const size_t ub_tool_events_size=sizeof ub_tool_events;
const struct ub_payload_variant *ub_payload_variant(const struct ub_payload *p,const char *sha){
 assert(p==&ub_payload_power_policy);
 for(unsigned i=0;i<2;i++)if(!strcmp(sha,variants[i].sha256))return &variants[i];
 return 0;
}
int ub_hash_file(const char *path,char sha[65]){
 hashes++;
 if(!strcmp(path,"/usr/dtvrec/bin/apl_mgr")){strcpy(sha,wrong_manager?SHA185:(cohort185?SHA185:UB_SHA256_APL_MGR));if(wrong_manager)sha[0]='x';return 0;}
 const char *name=strrchr(path,'/');assert(name);name++;
 for(unsigned i=0;i<UB_POWER_LIBRARY_GUARD_COUNT;i++)if(!strcmp(name,ub_power_library_guards[i].name)){
  const struct ub_power_library_guard *g=&ub_power_library_guards[i];strcpy(sha,cohort185?g->sha185:g->sha182);
  if(wrong_library)sha[0]='x';
  return 0;
 }
 assert(!"unexpected hash path");return -1;
}
uint32_t ub_map_base(int pid,const char *suffix,char *path,size_t size){
 assert(pid==77);maps++;
 for(unsigned i=0;i<UB_POWER_LIBRARY_GUARD_COUNT;i++)if(!strcmp(suffix+1,ub_power_library_guards[i].name)){
  snprintf(path,size,"%s/%s",wrong_path?"/unexpected":"/usr2/dtvrec/lib",ub_power_library_guards[i].name);
  return ub_power_library_guards[i].base+(wrong_base?4096u:0u);
 }
 assert(!"unexpected library suffix");return 0;
}
int ub_setting_set(const char *module,const char *key,const char *value){
 assert(!strcmp(module,"power")&&!strcmp(key,"auto_off"));assert(!strcmp(value,"on")||!strcmp(value,"off"));saves++;return save_error;
}
const char *ub_stick_error(int code){assert(code);return "fixture storage error";}
void ub_log(const char *module,int level,const char *format,...){(void)module;(void)level;(void)format;}

static void remove_files(void){
 struct ub_dir d;const char *name;char path[256];
 assert(!ub_dir_open(&d,directory));
 while((name=ub_dir_next(&d)))if(!strncmp(name,"power.",6)){
  snprintf(path,sizeof path,"%s/%s",directory,name);assert(!unlink(path));
 }
 ub_dir_close(&d);
}
static void reset(void){
 remove_files();memset(&pending,0,sizeof pending);memset(token,0,sizeof token);memset(token_hex,0,sizeof token_hex);
 memset(&parts[0].target,0,sizeof parts[0].target);parts[0].tripped=0;
 auto_off=1;effective=-1;available=blocked=initial_sent=owner_pid=0;owner_born=0;serial=menu_seq=0;menu_message[0]=0;
 fork_failure=wait_ready=wait_code=wait_missing=cohort185=wrong_manager=wrong_base=wrong_path=wrong_library=save_error=0;
 forks=reaps=saves=hashes=maps=0;seconds=1000;
}
static void native_ack(const char *ack_token,unsigned seq,unsigned mode,int result){
 char text[256];int n=snprintf(text,sizeof text,"UBPWR1 %s %u %u %d 1 0 1 0\n",ack_token,seq,mode,result);
 assert(n>0&&!ram_file(UB_POWER_NATIVE,text,(size_t)n,0600));
}
static void request(unsigned path_seq,const char *body,unsigned deadline){
 static const char sid[]="0123456789abcdef0123456789abcdef";char path[UB_ACTION_PATH];
 assert(!ub_action_path(path,sizeof path,UB_POWER_REQUEST,'q',sid,path_seq));
 assert(!ub_action_envelope(path,body,strlen(body),deadline));
}
static char *read_reply(unsigned seq,char *text,size_t size){
 static const char sid[]="0123456789abcdef0123456789abcdef";char path[UB_ACTION_PATH];uint32_t expires;
 assert(!ub_action_path(path,sizeof path,UB_POWER_REQUEST,'r',sid,seq));assert(ub_read_file(path,text,size)>0);
 return ub_action_body(text,&expires);
}
static void boot_ready(void){
 start(&ub_module_power);assert(available&&!access(UB_POWER_TOKEN,F_OK)&&!access(UB_POWER_TOOL,F_OK));
 parts[0].target=(struct ub_target){.pid=77,.born=99,.verified=1};
 prepare(&parts[0],&parts[0].target);assert(parts[0].target.state[0]&&!parts[0].target.failed);
 parts[0].target.installed=1;control_step(0,0);assert(pending.active&&pending.mode==1&&forks==1&&effective<0);
 native_ack(token_hex,pending.serial,1,UB_POWER_OK);finish_native();assert(effective<0&&access(UB_POWER_ENABLED,F_OK));
 wait_ready=1;control_step(0,0);wait_ready=0;
 assert(!pending.active&&effective==1&&!access(UB_POWER_ENABLED,F_OK)&&!access(UB_POWER_STATUS,F_OK)&&reaps==1);
}
int main(void){
 char template[]="/tmp/ub820-power-supervisor-XXXXXX",text[256];unsigned cases=0;
 assert(mkdtemp(template));snprintf(directory,sizeof directory,"%s",template);
 snprintf(enabled_file,sizeof enabled_file,"%s/power.enabled",directory);
 snprintf(request_file,sizeof request_file,"%s/power.request",directory);
 snprintf(status_file,sizeof status_file,"%s/power.status",directory);
 snprintf(token_file,sizeof token_file,"%s/power.token",directory);
 snprintf(native_file,sizeof native_file,"%s/power.native",directory);
 snprintf(packet_file,sizeof packet_file,"%s/power.packet",directory);
 snprintf(tool_file,sizeof tool_file,"%s/power.tool",directory);
 reset();boot_ready();cases++;
 for(unsigned i=0;i<100;i++)control_step(0,0);
 assert(forks==1);cases++;
 request(7,"set off 7\n",seconds+4);control_step(0,0);
 assert(pending.active&&pending.mode==0&&pending.menu_seq==7&&pending.expires==seconds+4&&effective==1&&forks==2);cases++;
 {struct ub_power_packet packet;int fd=open(UB_POWER_PACKET,O_RDONLY);assert(fd>=0&&read(fd,&packet,sizeof packet)==sizeof packet);close(fd);assert(packet.auto_off==0&&packet.expires==seconds+4&&packet.seq==pending.serial);}
 native_ack("ffffffffffffffffffffffffffffffff",pending.serial,0,UB_POWER_OK);control_step(0,0);assert(pending.active&&effective==1);cases++;
 native_ack(token_hex,pending.serial+1,0,UB_POWER_OK);control_step(0,0);assert(pending.active&&effective==1);cases++;
 native_ack(token_hex,pending.serial,1,UB_POWER_OK);control_step(0,0);assert(pending.active&&effective==1);cases++;
 native_ack(token_hex,pending.serial,0,UB_POWER_OK);control_step(0,0);assert(pending.active&&effective==1);
 wait_ready=1;control_step(0,0);wait_ready=0;
 assert(!pending.active&&effective==0&&saves==1&&strstr(read_reply(7,text,sizeof text),"7 off saved"));cases++;
 request(8,"set off 8\n",seconds+4);control_step(0,0);assert(forks==2&&!pending.active&&strstr(read_reply(8,text,sizeof text),"already applied"));cases++;
 request(9,"set on 9\n",seconds+4);control_step(0,0);assert(pending.active&&forks==3);
 native_ack(token_hex,pending.serial,1,UB_POWER_FPC_ERROR);control_step(0,0);
 assert(!pending.active&&blocked&&effective<0&&pending.producer>0&&access(UB_POWER_ENABLED,F_OK)&&access(UB_POWER_STATUS,F_OK));cases++;
 wait_ready=1;control_step(0,0);assert(pending.producer==0&&pending.producer_done&&reaps==3);cases++;
 reset();boot_ready();
 request(12,"set off 12\n",seconds+2);control_step(0,0);seconds+=2;control_step(0,0);
 assert(blocked&&pending.unknown&&pending.active&&effective<0&&access(UB_POWER_ENABLED,F_OK));cases++;
 native_ack(token_hex,pending.serial,0,UB_POWER_OK);wait_ready=1;control_step(0,0);wait_ready=0;
 assert(!pending.active&&!blocked&&effective==0&&saves==1&&forks==2);cases++;
 reset();boot_ready();
 request(3,"set off 3\n",seconds-1);control_step(0,0);assert(forks==1&&!pending.active);cases++;
 request(4,"set off 5\n",seconds+5);control_step(0,0);assert(forks==1&&!pending.active);cases++;
 request(5,"set off -1\n",seconds+5);control_step(0,0);assert(forks==1&&!pending.active);cases++;
 request(6,"set off 4294967296\n",seconds+5);control_step(0,0);assert(forks==1&&!pending.active);cases++;
 request(11,"set off 011\n",seconds+5);control_step(0,0);assert(forks==1&&!pending.active);cases++;
 assert(!ram_file(UB_POWER_REQUEST,"set off 15\n",11,0600));control_step(0,0);assert(forks==1&&!pending.active);cases++;
 request(7,"set off 7\n",seconds+5);control_step(0,0);assert(forks==2&&pending.active);wait_missing=1;producer_poll();assert(pending.producer_done&&pending.producer_status==-1);cases++;
 reset();parts[0].target=(struct ub_target){.pid=77,.born=99,.verified=1};prepare(&parts[0],&parts[0].target);assert(parts[0].target.failed&&!hashes&&!maps);cases++;
 for(unsigned kind=0;kind<4;kind++){
  reset();start(&ub_module_power);parts[0].target=(struct ub_target){.pid=77,.born=99,.verified=1};
  wrong_manager=kind==0;wrong_base=kind==1;wrong_path=kind==2;wrong_library=kind==3;
  prepare(&parts[0],&parts[0].target);assert(parts[0].target.failed&&!parts[0].target.state[0]&&!forks);cases++;
 }
 reset();cohort185=1;boot_ready();assert(!parts[0].target.failed&&effective==1);cases++;
 reset();start(&ub_module_power);parts[0].target=(struct ub_target){.pid=77,.born=99,.verified=1};
 control_step(0,0);assert(!forks&&!initial_sent&&effective<0);cases++;
 reset();boot_ready();parts[0].target.failed=1;control_step(0,0);
 assert(blocked&&effective<0&&access(UB_POWER_ENABLED,F_OK)&&forks==1);cases++;
 reset();boot_ready();request(10,"set off 10\n",seconds+5);fork_failure=1;control_step(0,0);
 assert(!pending.active&&effective==1&&strstr(read_reply(10,text,sizeof text),"request could not be sent"));cases++;
 reset();boot_ready();request(16,"set off 16\n",seconds+5);control_step(0,0);
 native_ack(token_hex,pending.serial,0,UB_POWER_OK);wait_ready=1;save_error=4;control_step(0,0);
 assert(effective==0&&!blocked&&!pending.active&&strstr(read_reply(16,text,sizeof text),"applied; not saved to USB"));cases++;
 reset();boot_ready();request(17,"set off 17\n",seconds+5);control_step(0,0);
 native_ack(token_hex,pending.serial,0,UB_POWER_OK);wait_ready=1;wait_code=7;control_step(0,0);
 assert(effective==0&&!blocked&&!pending.active&&pending.producer_status==7);cases++;
 reset();boot_ready();parts[0].target.born=100;request(13,"set off 13\n",seconds+5);control_step(0,0);
 assert(pending.active);parts[0].target.born=101;control_step(0,0);assert(blocked&&effective<0);cases++;
 remove_files();assert(!rmdir(directory));
 printf("power_test passed (%u scenarios)\n",cases);return 0;
}
