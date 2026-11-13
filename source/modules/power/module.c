/* Auto power-off policy. All native changes execute on the manager's own EV
 * thread. The supervisor sends one command per change; no keep-alive timer,
 * repeated key, EEPROM write or periodic reapplication. */
#define _GNU_SOURCE
#include "ub.h"
#include "payloads.h"
#include "tools.h"
#include "helper_menu.h"
#include "action_ipc.h"
#include "storage.h"
#include "power_protocol.h"
#include "library_guards.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ID "power"
static int auto_off=1,effective=-1,available,blocked,initial_sent,owner_pid;
static unsigned long long owner_born;
static unsigned char token[16];static char token_hex[33];static uint32_t serial;
static unsigned menu_seq;static char menu_message[96]="";
static struct {
 int active,unknown,mode,producer_done,producer_status;uint32_t serial,expires;
 unsigned menu_seq;pid_t producer;char reply[UB_ACTION_PATH];
} pending;
static struct ub_part parts[];
static uint32_t now_seconds(void){return (uint32_t)(ub_now_ms()/1000u);}
static const char *mode_name(int mode){return mode?"on":"off";}
static int ram_file(const char *path,const void *data,size_t size,int mode){
 char temp[UB_ACTION_PATH];int fd,rc=0;size_t used=0;
 if(snprintf(temp,sizeof temp,"%s.tmp-%ld",path,(long)getpid())>=(int)sizeof temp)return -1;
 unlink(temp);fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,mode);if(fd<0)return -1;
 while(used<size){ssize_t n=write(fd,(const char *)data+used,size-used);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-1;break;}used+=(size_t)n;}
 if(close(fd))rc=-1;
 if(rc||rename(temp,path)){unlink(temp);return -1;}return 0;
}
static int publish_status(void){
 char line[192];int n;
 if(effective<0){unlink(UB_POWER_ENABLED);unlink(UB_POWER_STATUS);return 0;}
 n=snprintf(line,sizeof line,"%u %s %s\n",menu_seq,mode_name(effective),menu_message);
 if(n<0||(size_t)n>=sizeof line||ram_file(UB_POWER_STATUS,line,(size_t)n,0600))return -1;
 return ram_file(UB_POWER_ENABLED,"1\n",2,0600);
}
static void answer(void){
 char line[192];int n;if(!pending.reply[0]||effective<0)return;
 n=snprintf(line,sizeof line,"%u %s %s\n",pending.menu_seq,mode_name(effective),menu_message);
 if(n>0&&(size_t)n<sizeof line&&ub_action_reply(pending.reply,line,(size_t)n))ub_log(ID,UB_ALWAYS,"cannot acknowledge power request");
}
static void unknown(const char *why){
 if(effective<0&&blocked&&pending.unknown&&!strcmp(menu_message,why))return;
 effective=-1;blocked=1;pending.unknown=1;snprintf(menu_message,sizeof menu_message,"%s",why);
 publish_status();ub_log(ID,UB_ALWAYS,"auto power-off result unknown: %s; no automatic resend",why);
}
static void clean_producer(void){
 struct ub_dir d;const char *entry;sigset_t empty;int nul;
 for(int sig=1;sig<65;sig++)signal(sig,SIG_DFL);
 sigemptyset(&empty);sigprocmask(SIG_SETMASK,&empty,0);umask(077);
 if(!ub_dir_open(&d,"/proc/self/fd")){
  while((entry=ub_dir_next(&d)))if(entry[0]>='0'&&entry[0]<='9'){int fd=atoi(entry);if(fd>2&&fd!=d.fd)close(fd);}
  ub_dir_close(&d);
 }else{
  struct rlimit lim;if(getrlimit(RLIMIT_NOFILE,&lim)||lim.rlim_max==RLIM_INFINITY)_exit(126);
  for(rlim_t fd=3;fd<lim.rlim_max;fd++)close((int)fd);
 }
 nul=open("/dev/null",O_RDWR|O_CLOEXEC);
 if(nul<0||dup2(nul,0)<0||dup2(nul,1)<0||dup2(nul,2)<0)_exit(126);
 if(nul>2)close(nul);
 execl(UB_POWER_TOOL,"events",UB_POWER_PACKET,(char *)0);_exit(127);
}
static int begin(int mode,unsigned sequence,const char *reply,uint32_t expires){
 struct ub_power_packet packet;pid_t producer;
 if(pending.active||blocked||!available||mode<0||mode>1)return -1;
 if(!expires)expires=now_seconds()+15u;
 if((int32_t)(now_seconds()-expires)>=0)return -1;
 if(serial==UINT32_MAX){unknown("control sequence exhausted");return -1;}
 memset(&packet,0,sizeof packet);packet.magic=UB_POWER_MAGIC;packet.version=UB_POWER_VERSION;
 memcpy(packet.token,token,sizeof token);packet.seq=++serial;packet.auto_off=(uint32_t)mode;packet.expires=expires;
 if(ram_file(UB_POWER_PACKET,&packet,sizeof packet,0600))return -1;
 memset(&pending,0,sizeof pending);pending.active=1;pending.mode=mode;pending.serial=serial;pending.expires=expires;pending.menu_seq=sequence;
 if(reply)snprintf(pending.reply,sizeof pending.reply,"%s",reply);
 producer=fork();if(!producer)clean_producer();
 if(producer<0){memset(&pending,0,sizeof pending);return -1;}
 pending.producer=producer;
 ub_log(ID,UB_INFO,"queued native policy request %u: %s (owner PID%d)",serial,mode_name(mode),owner_pid);return 0;
}
static void producer_poll(void){
 int status;pid_t rc;if(pending.producer<=0)return;
 do{rc=waitpid(pending.producer,&status,WNOHANG);}while(rc<0&&errno==EINTR);
 if(rc==pending.producer){pending.producer=0;pending.producer_done=1;pending.producer_status=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
  if(pending.producer_status)ub_log(ID,UB_ALWAYS,"EV producer ended with status %d; native acknowledgement remains authoritative",pending.producer_status);
 }else if(rc<0&&errno==ECHILD){pending.producer=0;pending.producer_done=1;pending.producer_status=-1;}
}
static void finish_native(void){
 char text[256],received_token[33],tail;unsigned sequence,mode;int result,eep_seen,eep_rc,fpc_seen,fpc_rc,n;
 if(!pending.active)return;
 n=(int)ub_read_file(UB_POWER_NATIVE,text,sizeof text);if(n<=0)return;
 if(sscanf(text,"UBPWR1 %32s %u %u %d %d %d %d %d %c",received_token,&sequence,&mode,&result,&eep_seen,&eep_rc,&fpc_seen,&fpc_rc,&tail)!=8)return;
 if(strcmp(received_token,token_hex)||sequence!=pending.serial||mode!=(unsigned)pending.mode)return;
 if(result!=UB_POWER_OK){unknown("native policy change was not confirmed");pending.active=0;return;}
 /* A native result can precede the producer process's return. Keep its PID
  * until reaped; never overwrite a packet while an older producer can read it. */
 if(!pending.producer_done)return;
 effective=pending.mode;auto_off=effective;menu_seq=pending.menu_seq;blocked=0;
 if(pending.reply[0]){
  int rc=ub_setting_set(ID,"auto_off",mode_name(effective));
  if(rc)snprintf(menu_message,sizeof menu_message,"applied; not saved to USB (%s)",ub_stick_error(rc));
  else snprintf(menu_message,sizeof menu_message,"saved");
 }else snprintf(menu_message,sizeof menu_message,"ready");
 ub_log(ID,UB_ALWAYS,"auto power-off %s confirmed by manager thread (EEP %d/%d, FPC %d/%d)",mode_name(effective),eep_seen,eep_rc,fpc_seen,fpc_rc);
 answer();pending.active=0;publish_status();
}
static void menu_request(void){
 char buf[80],choice[8],number[12],reply[UB_ACTION_PATH],extra;unsigned sequence=0;uint32_t expires;int mode;
 if(pending.active||blocked||effective<0)return;
 if(!ub_action_receive_deadline(UB_POWER_REQUEST,buf,sizeof buf,reply,sizeof reply,&expires))return;
 if(!reply[0]||!expires)return; /* This new endpoint has no unexpiring legacy clients. */
 if(sscanf(buf,"set %7s %11s %c",choice,number,&extra)!=2||number[0]<'1'||number[0]>'9')return;
 for(const char *c=number;*c;c++){
  unsigned digit=(unsigned char)*c-'0';if(digit>9||sequence>(UINT32_MAX-digit)/10u)return;
  sequence=sequence*10u+digit;
 }
 if(reply[0]){
  const char *suffix=strrchr(reply,'.');char expected[12];
  snprintf(expected,sizeof expected,"%u",sequence);
  if(!suffix||strcmp(suffix+1,expected))return;
 }
 mode=!strcmp(choice,"on")?1:!strcmp(choice,"off")?0:-1;
 if(mode<0)return;
 if(mode==effective){
  memset(&pending,0,sizeof pending);pending.menu_seq=sequence;snprintf(pending.reply,sizeof pending.reply,"%s",reply);
  snprintf(menu_message,sizeof menu_message,"already applied");answer();menu_seq=sequence;publish_status();return;
 }
 if(begin(mode,sequence,reply,expires)){
  memset(&pending,0,sizeof pending);pending.menu_seq=sequence;snprintf(pending.reply,sizeof pending.reply,"%s",reply);
  snprintf(menu_message,sizeof menu_message,"request could not be sent");answer();menu_seq=sequence;publish_status();
 }
}
static void prepare(struct ub_part *part,struct ub_target *target){
 char program_sha[65];const struct ub_payload_variant *variant;int firmware185;
 if(!available){target->failed=1;return;}
 if(target->state[0])return;
 if(ub_hash_file("/usr/dtvrec/bin/apl_mgr",program_sha)||(variant=ub_payload_variant(part->payload,program_sha))==0){target->failed=1;ub_log(ID,UB_ALWAYS,"native power control unavailable: manager build differs");return;}
 firmware185=!strcmp(variant->firmware,"1.85");
 for(unsigned i=0;i<UB_POWER_LIBRARY_GUARD_COUNT;i++){
  const struct ub_power_library_guard *guard=&ub_power_library_guards[i];char suffix[64],mapped[256],sha[65],allowed[256];uint32_t base;
  snprintf(suffix,sizeof suffix,"/%s",guard->name);base=ub_map_base(target->pid,suffix,mapped,sizeof mapped);
  if(base!=guard->base)goto rejected;
  snprintf(allowed,sizeof allowed,"/usr2/dtvrec/lib/%s",guard->name);
  if(strcmp(mapped,allowed)){snprintf(allowed,sizeof allowed,"/usr/dtvrec/lib/%s",guard->name);if(strcmp(mapped,allowed))goto rejected;}
  if(ub_hash_file(mapped,sha)||strcmp(sha,firmware185?guard->sha185:guard->sha182))goto rejected;
  continue;
 rejected:
  target->failed=1;ub_log(ID,UB_ALWAYS,"native power control unavailable: %s mapping or exact library hash differs",guard->name);return;
 }
 target->state[0]=1;
}
static void control_step(struct ub_part *part,struct ub_target *unused){
 struct ub_target *target=&parts[0].target;(void)part;(void)unused;
 producer_poll();
 if(!available)return;
 if(!target->installed||target->failed||target->verified!=1||parts[0].tripped){
  if(!blocked&&(effective>=0||initial_sent))unknown("manager hook is not available");
  return;
 }
 if(owner_pid!=target->pid||owner_born!=target->born){
  if(pending.active){unknown("manager instance changed during request");return;}
  owner_pid=target->pid;owner_born=target->born;effective=-1;initial_sent=0;blocked=0;publish_status();
 }
 if(!initial_sent){initial_sent=1;if(begin(auto_off,0,0,now_seconds()+15u))unknown("initial policy request could not be sent");}
 finish_native();
 if(pending.active&&(int32_t)(now_seconds()-pending.expires)>=0&&!pending.unknown)unknown("native acknowledgement deadline elapsed");
 menu_request();
}
static void start(struct ub_module *module){
 int fd;size_t done=0;(void)module;
 unlink(UB_POWER_ENABLED);unlink(UB_POWER_STATUS);unlink(UB_POWER_NATIVE);
 fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0)goto failed;
 while(done<sizeof token){ssize_t n=read(fd,token+done,sizeof token-done);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);goto failed;}done+=(size_t)n;}
 close(fd);ub_power_token_hex(token_hex,token);
 if(ram_file(UB_POWER_TOKEN,token,sizeof token,0600)||ram_file(UB_POWER_TOOL,ub_tool_events,ub_tool_events_size,0700))goto failed;
 available=1;return;
 failed:available=0;ub_log(ID,UB_ALWAYS,"native power control unavailable: RAM preparation failed");
}
static const char *const names[]={"off","on",0};
static const struct ub_option options[]={{"auto_off",0,1,&auto_off,names},{0,0,0,0,0}};
static struct ub_part parts[]={
 {"policy","apl_mgr",UB_SHA256_APL_MGR,&ub_payload_power_policy,1,prepare,0,{0},0,0},
 {"control",0,0,0,0,0,control_step,{0},0,0},
};
struct ub_module ub_module_power={ID,"Idle auto power-off: stock policy or disabled; manual power unchanged",1,options,start,parts,2,0,0};
