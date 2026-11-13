/* Persistence protocol only: own RAM records, mocked config/USB writer. */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
#include "ub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char seed_path[160],ack_path[160],claim_path[160],notice_path[160],status_path[160];
#define UB_BRIGHTNESS_SEED seed_path
#define UB_BRIGHTNESS_SAVE_ACK ack_path
#define UB_BRIGHTNESS_SAVE_CLAIM claim_path
#define UB_BRIGHTNESS_COMMIT_NOTICE notice_path
#include "../modules/external_subtitles/protocol.h"
#undef UB_STATUS_FILE
#define UB_STATUS_FILE status_path
#include "../modules/external_subtitles/brightness_io.h"
static int claim_read_error,claim_write_error,ack_write_error,ack_writes,status_reads;
static int fixture_read(const char *path,void *out,size_t size){
 if(!strcmp(path,claim_path)&&claim_read_error){errno=EIO;return -1;}
 if(!strcmp(path,status_path))status_reads++;
 return ub_br_private_read(path,out,size);
}
static int fixture_write(const char *path,const void *data,size_t size){
 if(!strcmp(path,ack_path))ack_writes++;
 if((!strcmp(path,claim_path)&&claim_write_error)||(!strcmp(path,ack_path)&&ack_write_error)){errno=EIO;return -1;}
 return ub_br_private_write(path,data,size);
}
#define ub_br_private_read fixture_read
#define ub_br_private_write fixture_write
#include "../modules/external_subtitles/brightness_supervisor.h"
#undef ub_br_private_read
#undef ub_br_private_write

static struct ub_target target;
static struct ub_status native;
static int save_calls,save_result,saved_percent,close_during_save;
static struct ub_brightness_save_ack receipt(int result);
void ub_log(const char *module,int level,const char *format,...){(void)module;(void)level;(void)format;}
int ub_setting_set(const char *module,const char *key,const char *value){
 struct ub_brightness_save_ack claim;
 assert(!strcmp(module,"external_subtitles")&&!strcmp(key,"brightness"));
 assert(!ub_br_private_read(claim_path,&claim,sizeof claim)&&brightness_claim_valid(&claim)&&claim.result==-1);
 assert(claim.seq==native.brightness_commit_seq&&claim.epoch==native.brightness_commit_epoch);
 save_calls++;saved_percent=atoi(value);
 if(close_during_save){native.epoch++;assert(!ub_br_private_write(status_path,&native,sizeof native));}
 return save_result;
}
static void publish(unsigned begin,unsigned percent){
 memset(&native,0,sizeof native);native.magic=UB_MAGIC;native.version=UB_VERSION;native.epoch=10;
 native.brightness_pid=brightness_supervisor.seed.pid;native.brightness_birth_lo=brightness_supervisor.seed.birth_lo;
 native.brightness_birth_hi=brightness_supervisor.seed.birth_hi;memcpy(native.brightness_token,brightness_supervisor.seed.token,16);
 native.brightness_phase=UB_BRIGHTNESS_COMMIT;native.brightness_edit=begin;native.brightness_edit_epoch=10;
 native.brightness_commit_seq=begin+1;native.brightness_commit_epoch=10;native.brightness_commit_percent=percent;
 native.brightness_commit_level=ub_brightness_level(percent);native.text_level=native.brightness_commit_level;
 native.brightness_owner_pid=(uint32_t)getpid();native.brightness_owner_start_lo=(uint32_t)target.born;native.brightness_owner_start_hi=(uint32_t)(target.born>>32);
 assert(!ub_br_private_write(status_path,&native,sizeof native));
 struct ub_brightness_save_ack notice=receipt(-1);assert(!ub_br_private_write(notice_path,&notice,sizeof notice));
}
static void fresh(void){
 unlink(seed_path);unlink(ack_path);unlink(claim_path);unlink(notice_path);unlink(status_path);
 save_calls=save_result=saved_percent=close_during_save=0;claim_read_error=claim_write_error=ack_write_error=ack_writes=status_reads=0;
 brightness_seed_prepare(&target,100);assert(brightness_supervisor.valid);publish(100,37);
}
static struct ub_brightness_save_ack receipt(int result){
 struct ub_brightness_save_ack r={0};r.magic=UB_BRIGHTNESS_SAVE_MAGIC;r.version=UB_BRIGHTNESS_PROTOCOL;
 memcpy(r.token,native.brightness_token,16);r.pid=native.brightness_pid;r.birth_lo=native.brightness_birth_lo;r.birth_hi=native.brightness_birth_hi;
 r.edit=native.brightness_edit;r.epoch=native.brightness_commit_epoch;r.seq=native.brightness_commit_seq;
 r.percent=native.brightness_commit_percent;r.level=native.brightness_commit_level;r.result=result;return r;
}
static struct ub_brightness_save_ack read_ack(void){
 struct ub_brightness_save_ack ack;assert(!ub_br_private_read(ack_path,&ack,sizeof ack));return ack;
}
int main(void){
 char directory[]="/tmp/ub820-brightness-save-XXXXXX";assert(mkdtemp(directory));uint64_t born;assert(!ub_br_birth((unsigned)getpid(),&born));
 target=(struct ub_target){.pid=(int)getpid(),.born=born,.verified=1};
 snprintf(seed_path,sizeof seed_path,"%s/seed",directory);snprintf(ack_path,sizeof ack_path,"%s/ack",directory);
 snprintf(claim_path,sizeof claim_path,"%s/claim",directory);snprintf(status_path,sizeof status_path,"%s/status",directory);
 snprintf(notice_path,sizeof notice_path,"%s/notice",directory);
 unsigned cases=0;struct ub_brightness_save_ack claim,ack;
 fresh();assert(ub_br_private_read(claim_path,&claim,sizeof claim)==1);
 brightness_save_tick(&target);ack=read_ack();assert(save_calls==1&&saved_percent==37&&ack.result==UB_BRIGHTNESS_SAVED);cases++;
 brightness_save_tick(&target);assert(save_calls==1&&ack_writes==1&&!status_reads);cases++;
 brightness_seed_prepare(&target,100);brightness_save_tick(&target);assert(save_calls==1&&read_ack().result==UB_BRIGHTNESS_SAVED);cases++;
 fresh();unlink(notice_path);native.brightness_phase=UB_BRIGHTNESS_PREVIEW;assert(!ub_br_private_write(status_path,&native,sizeof native));
 brightness_save_tick(&target);assert(!save_calls&&access(ack_path,F_OK));cases++;
 fresh();native.brightness_token[0]^=1;claim=receipt(-1);assert(!ub_br_private_write(notice_path,&claim,sizeof claim));brightness_save_tick(&target);assert(!save_calls);cases++;
 fresh();native.brightness_birth_lo++;claim=receipt(-1);assert(!ub_br_private_write(notice_path,&claim,sizeof claim));brightness_save_tick(&target);assert(!save_calls);cases++;
 fresh();native.epoch++;assert(!ub_br_private_write(status_path,&native,sizeof native));brightness_save_tick(&target);
 assert(save_calls==1&&read_ack().epoch==10);cases++;
 fresh();close_during_save=1;brightness_save_tick(&target);assert(save_calls==1&&native.epoch==11&&read_ack().epoch==10);cases++;
 fresh();save_result=5;brightness_save_tick(&target);assert(save_calls==1&&read_ack().result==UB_BRIGHTNESS_SAVE_ERROR);brightness_save_tick(&target);assert(save_calls==1);cases++;
 fresh();claim=receipt(-1);assert(!ub_br_private_write(claim_path,&claim,sizeof claim));brightness_save_tick(&target);
 assert(!save_calls&&read_ack().result==UB_BRIGHTNESS_SAVE_UNKNOWN);cases++;
 fresh();assert(!ub_br_private_write(claim_path,"bad",3));brightness_save_tick(&target);
 assert(!save_calls&&read_ack().result==UB_BRIGHTNESS_SAVE_UNKNOWN);brightness_save_tick(&target);assert(!save_calls);cases++;
 /* Repaired uncertainty is terminal for the OLD receipt, not an artificial
  * permanent restriction: a fresh higher COMMIT is a new authorization. */
 publish(200,43);brightness_save_tick(&target);assert(save_calls==1&&saved_percent==43&&read_ack().result==UB_BRIGHTNESS_SAVED);cases++;
 fresh();claim=receipt(-1);claim.magic=0;assert(!ub_br_private_write(claim_path,&claim,sizeof claim));brightness_save_tick(&target);assert(!save_calls&&read_ack().result==UB_BRIGHTNESS_SAVE_UNKNOWN);cases++;
 fresh();claim=receipt(-1);claim.seq=0;assert(!ub_br_private_write(claim_path,&claim,sizeof claim));brightness_save_tick(&target);assert(!save_calls&&read_ack().result==UB_BRIGHTNESS_SAVE_UNKNOWN);cases++;
 fresh();claim=receipt(-1);claim.level++;assert(!ub_br_private_write(claim_path,&claim,sizeof claim));brightness_save_tick(&target);assert(!save_calls&&read_ack().result==UB_BRIGHTNESS_SAVE_UNKNOWN);cases++;
 fresh();claim=receipt(-1);claim.token[0]^=1;assert(!ub_br_private_write(claim_path,&claim,sizeof claim));brightness_save_tick(&target);assert(!save_calls&&read_ack().result==UB_BRIGHTNESS_SAVE_UNKNOWN);cases++;
 fresh();claim_read_error=1;brightness_save_tick(&target);assert(!save_calls&&read_ack().result==UB_BRIGHTNESS_SAVE_UNKNOWN);cases++;
 fresh();claim_read_error=claim_write_error=1;brightness_save_tick(&target);assert(!save_calls&&read_ack().result==UB_BRIGHTNESS_SAVE_UNKNOWN);
 claim_read_error=claim_write_error=0;brightness_save_tick(&target);assert(!save_calls);publish(200,54);brightness_save_tick(&target);assert(save_calls==1);cases++;
 fresh();ack_write_error=1;brightness_save_tick(&target);assert(save_calls==1&&access(ack_path,F_OK));ack_write_error=0;
 brightness_save_tick(&target);assert(save_calls==1&&read_ack().result==UB_BRIGHTNESS_SAVED&&ack_writes==2);
 brightness_save_tick(&target);assert(ack_writes==2);cases++;
 fresh();brightness_save_tick(&target);assert(save_calls==1);unlink(claim_path);unlink(ack_path);publish(50,12);
 brightness_save_tick(&target);assert(save_calls==1&&read_ack().result==UB_BRIGHTNESS_SAVE_STALE&&brightness_supervisor.highest_seq==101);cases++;
 fresh();claim=receipt(-1);claim.seq=401;claim.edit=400;assert(!ub_br_private_write(claim_path,&claim,sizeof claim));
 brightness_save_tick(&target);assert(!save_calls&&read_ack().result==UB_BRIGHTNESS_SAVE_STALE);cases++;
 fresh();assert(!chmod(notice_path,0644));brightness_save_tick(&target);assert(!save_calls);cases++;
 fresh();brightness_save_tick(&target);publish(200,62);brightness_save_tick(&target);
 assert(save_calls==2&&saved_percent==62&&ack_writes==2&&!status_reads);brightness_save_tick(&target);assert(save_calls==2&&ack_writes==2);cases++;
 fresh();unsigned char token[16];memcpy(token,brightness_supervisor.seed.token,16);brightness_seed_prepare(&target,10);
 assert(!memcmp(token,brightness_supervisor.seed.token,16)&&brightness_supervisor.seed.percent==100);cases++;
 unlink(seed_path);unlink(ack_path);unlink(claim_path);unlink(notice_path);unlink(status_path);assert(!rmdir(directory));
 printf("subtitle_brightness_supervisor_test passed: %u cases; nativeACK-only save, once claims/restart/uncertainty, epoch intent and explicit recovery\n",cases);return 0;
}
