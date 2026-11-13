/* Native automatic-standby policy. Control runs only at the manager's post-EV
 * message classifier; all other calls retain native behavior. No periodic thread, fake
 * key, EEPROM write, or FPC call from the helper/producer. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include "power_protocol.h"
#include "payload_time.h"

int (*ub_original_power_dispatch)(void *);
int (*ub_original_power_eep_read)(void *,unsigned,unsigned);
int (*ub_original_power_fpc_auto)(unsigned,unsigned);
void (*ub_native_power_configure)(void *);
uintptr_t ub_power_owner_return;

#define CTX_MESSAGE 0x34b4u
#define CTX_ACTIVE_MESSAGE 0x3cc4u
#define CTX_QUEUE 0x5534u
#define CTX_ARMED 0x750u
#define EEP_AUTO_OFF 0x70c3u
struct power_observation {int eep_seen,eep_rc,fpc_seen,fpc_rc,fpc_bad;unsigned enable,code;};
static uint32_t desired_off,apply_tid;
static struct power_observation observation;
static unsigned char cached_token[16];
static uint32_t cached_seq,cached_choice;
static int cached_result,have_cache;
static struct power_observation cached_observation;

#ifdef UB_POWER_POLICY_TEST
extern int ub_power_test_token(unsigned char [16]);
extern int ub_power_test_now(uint32_t *);
extern uint32_t ub_power_test_tid(void);
extern uintptr_t ub_power_test_caller(void);
extern int ub_power_test_result(const struct ub_power_packet *,int,const struct power_observation *);
#define read_token ub_power_test_token
#define power_now ub_power_test_now
#define power_tid ub_power_test_tid
#define publish_result ub_power_test_result
#else
static uint32_t power_tid(void){return (uint32_t)syscall(SYS_gettid);}
static int power_now(uint32_t *now){return ub_payload_now(now);}
static int read_token(unsigned char token[16]){
 int fd=open(UB_POWER_TOKEN,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);size_t done=0;ssize_t n;unsigned char extra;
 if(fd<0)return -1;
 while(done<16){do{n=read(fd,token+done,16-done);}while(n<0&&errno==EINTR);if(n<=0){close(fd);return -1;}done+=(size_t)n;}
 do{n=read(fd,&extra,1);}while(n<0&&errno==EINTR);
 {int rc=n!=0;if(close(fd))rc=1;return rc?-1:0;}
}
static int publish_result(const struct ub_power_packet *p,int result,const struct power_observation *o){
 char text[240],token[33];int fd,n,rc=0;size_t done=0;
 ub_power_token_hex(token,p->token);
 n=snprintf(text,sizeof text,"UBPWR1 %s %u %u %d %d %d %d %d\n",token,p->seq,p->auto_off,result,o->eep_seen,o->eep_rc,o->fpc_seen,o->fpc_rc);
 if(n<0||(size_t)n>=sizeof text)return -1;
 fd=open(UB_POWER_NATIVE ".tmp",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
 if(fd<0)return -1;
 while(done<(size_t)n){ssize_t w=write(fd,text+done,(size_t)n-done);if(w<0&&errno==EINTR)continue;if(w<=0){rc=-1;break;}done+=(size_t)w;}
 if(close(fd))rc=-1;
 if(!rc&&rename(UB_POWER_NATIVE ".tmp",UB_POWER_NATIVE))rc=-1;
 if(rc)unlink(UB_POWER_NATIVE ".tmp");
 return rc;
}
#endif

static int observing(void){uint32_t tid=__atomic_load_n(&apply_tid,__ATOMIC_ACQUIRE);return tid&&tid==power_tid();}
int ub_hook_power_eep_read(void *out,unsigned length,unsigned key){
 int rc=ub_original_power_eep_read?ub_original_power_eep_read(out,length,key):-1;
 if(key==EEP_AUTO_OFF&&length==1){
  if(observing()){observation.eep_seen++;observation.eep_rc=rc;}
  if(!rc&&out&&__atomic_load_n(&desired_off,__ATOMIC_ACQUIRE))*(unsigned char *)out=2;
 }
 return rc;
}
int ub_hook_power_fpc_auto(unsigned enabled,unsigned code){
 int rc=ub_original_power_fpc_auto?ub_original_power_fpc_auto(enabled,code):-1;
 if(observing()){
  observation.fpc_seen++;if(!observation.fpc_bad)observation.fpc_rc=rc;observation.fpc_bad|=rc!=0;
  observation.enable=enabled;observation.code=code;
 }
 return rc;
}

static int apply_control(const struct ub_power_packet *p,void *ctx){
 uint32_t previous=__atomic_load_n(&desired_off,__ATOMIC_ACQUIRE),tid=power_tid();int result=UB_POWER_NATIVE_OK;
 memset(&observation,0,sizeof observation);
 if(!tid||!ub_native_power_configure||!ub_original_power_eep_read||!ub_original_power_fpc_auto)return UB_POWER_NATIVE_CONTEXT;
 __atomic_store_n(&desired_off,p->auto_off?0u:1u,__ATOMIC_RELEASE);
 __atomic_store_n(&apply_tid,tid,__ATOMIC_RELEASE);
 ub_native_power_configure(ctx);
 __atomic_store_n(&apply_tid,0u,__ATOMIC_RELEASE);
 if(observation.eep_seen&&observation.eep_rc)result=UB_POWER_NATIVE_EEP;
 else if(observation.fpc_bad)result=UB_POWER_NATIVE_FPC;
 else if(!p->auto_off&&(!observation.fpc_seen||observation.enable||observation.code||*(uint32_t *)((unsigned char *)ctx+CTX_ARMED)))result=UB_POWER_NATIVE_FPC;
 else if(p->auto_off&&!observation.eep_seen&&!observation.fpc_seen)result=UB_POWER_NATIVE_CONTEXT;
 /* Failed native outcome is not a reason to invent a second FPC operation.
  * Restore our software policy only; physical/native state remains unknown. */
 if(result)__atomic_store_n(&desired_off,previous,__ATOMIC_RELEASE);
 return result;
}

/* Returns1 for our private sender namespace, including malformed controls.
 * Such bytes must never be handed to the stock message dispatcher. */
static int consume_control(void *ctx,void *message,uintptr_t caller){
 uint32_t *header=message,now;unsigned char token[16];struct ub_power_packet packet;int result;
 if(!message||header[1]!=UB_POWER_SENDER)return 0;
 if(header[0]!=UB_POWER_QUEUE)return 1;
 if(header[2]!=sizeof packet)return 1;
 if(!ctx||!ub_power_owner_return||(caller&~(uintptr_t)1)!=(ub_power_owner_return&~(uintptr_t)1))return 1;
 memcpy(&packet,(unsigned char *)message+16,sizeof packet);
 if(read_token(token)||power_now(&now))return 1;
 if(packet.magic!=UB_POWER_MAGIC||packet.version!=UB_POWER_VERSION||!packet.seq||packet.auto_off>1||packet.reserved||memcmp(packet.token,token,16))return 1;
 if(!packet.expires||(int32_t)(now-packet.expires)>=0){
  struct power_observation none={0};publish_result(&packet,UB_POWER_NATIVE_EXPIRED,&none);return 1;
 }
 /* Validate every delivery, including duplicates: a cached acknowledgement
  * must not conceal a current native context mismatch. */
 if(message!=(unsigned char *)ctx+CTX_MESSAGE||*(uint32_t *)((unsigned char *)ctx+CTX_QUEUE)!=UB_POWER_QUEUE){
  struct power_observation none={0};publish_result(&packet,UB_POWER_NATIVE_CONTEXT,&none);return 1;
 }
 if(have_cache&&memcmp(cached_token,token,16))have_cache=0;
 if(have_cache&&packet.seq<=cached_seq){
  if(packet.seq==cached_seq&&packet.auto_off==cached_choice)publish_result(&packet,cached_result,&cached_observation);
  return 1;
 }
 result=apply_control(&packet,ctx);
 memcpy(cached_token,token,16);cached_seq=packet.seq;cached_choice=packet.auto_off;
 cached_result=result;cached_observation=observation;have_cache=1;
 publish_result(&packet,result,&observation);
 return 1;
}

int ub_hook_power_dispatch(void *ctx){
 uintptr_t caller;void *message;
#ifdef UB_POWER_POLICY_TEST
 caller=ub_power_test_caller();
#else
 caller=(uintptr_t)__builtin_return_address(0);
#endif
 if(!ub_original_power_dispatch)return -1;
 if(!ctx)return ub_original_power_dispatch(ctx);
 message=*(void **)((unsigned char *)ctx+CTX_ACTIVE_MESSAGE);
 /* Unlike an EV_get_message entry hook, this boundary also sees a packet
  * returned by a receive call already in progress when hooks were installed.
  * Native return0 means handled: the unchanged owner loop proceeds normally. */
 if(consume_control(ctx,message,caller))return 0;
 return ub_original_power_dispatch(ctx);
}
