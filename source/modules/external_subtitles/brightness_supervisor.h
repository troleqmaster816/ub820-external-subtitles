#ifndef UB_SUBTITLE_BRIGHTNESS_SUPERVISOR_H
#define UB_SUBTITLE_BRIGHTNESS_SUPERVISOR_H
#include "brightness_io.h"

static struct {
 int valid;
 struct ub_brightness_seed seed;
 struct ub_brightness_save_ack last;
 int have_last,ack_published;
 uint32_t highest_seq;
} brightness_supervisor;
static int brightness_receipt_same(const struct ub_brightness_save_ack *a,const struct ub_brightness_save_ack *b){
 return a->magic==b->magic&&a->version==b->version&&!memcmp(a->token,b->token,16)&&
  a->pid==b->pid&&a->birth_lo==b->birth_lo&&a->birth_hi==b->birth_hi&&
  a->edit==b->edit&&a->epoch==b->epoch&&a->seq==b->seq&&a->percent==b->percent&&a->level==b->level;
}
static int brightness_claim_valid(const struct ub_brightness_save_ack *claim){
 return claim->magic==UB_BRIGHTNESS_SAVE_MAGIC&&claim->version==UB_BRIGHTNESS_PROTOCOL&&
  claim->pid&&claim->pid<=INT32_MAX&&(claim->birth_lo||claim->birth_hi)&&
  ub_br_token_nonzero(claim->token)&&claim->edit&&claim->epoch&&claim->seq&&
  claim->percent<=100&&claim->level==ub_brightness_level(claim->percent)&&
  claim->result>=-1&&claim->result<=UB_BRIGHTNESS_SAVE_UNKNOWN;
}
static void brightness_seed_prepare(struct ub_target *target,unsigned percent){
 struct ub_brightness_seed seed;int fd;size_t used=0;ssize_t n;
 memset(&brightness_supervisor,0,sizeof brightness_supervisor);
 if(percent>100||!target->pid||!target->born)return;
 /* Preserve a live instance's token and claim history across a helper restart.
  * A new native process gets a new random token before payload publication. */
 if(!ub_br_private_read(UB_BRIGHTNESS_SEED,&seed,sizeof seed)&&
    ub_br_seed_valid(&seed,(unsigned)target->pid,target->born)){
  brightness_supervisor.seed=seed;brightness_supervisor.valid=1;return;
 }
 memset(&seed,0,sizeof seed);seed.magic=UB_BRIGHTNESS_SEED_MAGIC;seed.version=UB_BRIGHTNESS_PROTOCOL;
 seed.pid=(uint32_t)target->pid;seed.birth_lo=(uint32_t)target->born;seed.birth_hi=(uint32_t)(target->born>>32);seed.percent=percent;
 fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
 if(fd<0)return;
 while(used<16){n=read(fd,seed.token+used,16-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
 close(fd);
 if(used!=16||!ub_br_token_nonzero(seed.token)||ub_br_private_write(UB_BRIGHTNESS_SEED,&seed,sizeof seed)){
  ub_log("external_subtitles",UB_ALWAYS,"brightness seed unavailable; native subtitles retain their defaults");return;
 }
 brightness_supervisor.seed=seed;brightness_supervisor.valid=1;
}
static int brightness_notice_valid(const struct ub_brightness_save_ack *notice,struct ub_target *target){
 const struct ub_brightness_seed *seed=&brightness_supervisor.seed;
 return brightness_supervisor.valid&&ub_br_seed_valid(seed,(unsigned)target->pid,target->born)&&
  brightness_claim_valid(notice)&&notice->result==-1&&notice->pid==seed->pid&&
  notice->birth_lo==seed->birth_lo&&notice->birth_hi==seed->birth_hi&&!memcmp(notice->token,seed->token,16);
}
static void brightness_publish_save(struct ub_brightness_save_ack *ack){
 brightness_supervisor.last=*ack;brightness_supervisor.have_last=1;brightness_supervisor.ack_published=0;
 if(!brightness_supervisor.highest_seq||(int32_t)(ack->seq-brightness_supervisor.highest_seq)>0)brightness_supervisor.highest_seq=ack->seq;
 if(ub_br_private_write(UB_BRIGHTNESS_SAVE_ACK,ack,sizeof *ack))
  ub_log("external_subtitles",UB_ALWAYS,"brightness save result could not be published; no USB save will be repeated");
 else brightness_supervisor.ack_published=1;
}
static void brightness_unknown_claim(struct ub_brightness_save_ack *ack){
 ack->result=UB_BRIGHTNESS_SAVE_UNKNOWN;
 /* Repair only the journal/ACK, NEVER replay USB. A later, higher explicit
  * COMMIT is a new authorization; this uncertain receipt remains terminal. */
 if(ub_br_private_write(UB_BRIGHTNESS_SAVE_CLAIM,ack,sizeof *ack))
  ub_log("external_subtitles",UB_ALWAYS,"brightness uncertainty journal could not be repaired; USB save refused");
 brightness_publish_save(ack);
}
static void brightness_save_tick(struct ub_target *target){
 struct ub_brightness_save_ack ack,claim;uint64_t actual_born=0;char value[16];int rc;
 /* A small backend-owned last-COMMIT notice replaces permanent full-status
  * polling. It is never unlinked here: publication/consumption cannot race a
  * subsequent higher COMMIT. Successful ACK publication is not repeated idle. */
 if(!brightness_supervisor.valid||ub_br_private_read(UB_BRIGHTNESS_COMMIT_NOTICE,&ack,sizeof ack)||!brightness_notice_valid(&ack,target))return;
 if(brightness_supervisor.have_last&&brightness_receipt_same(&ack,&brightness_supervisor.last)){
  if(!brightness_supervisor.ack_published){
   ack=brightness_supervisor.last;
   if(!ub_br_private_write(UB_BRIGHTNESS_SAVE_ACK,&ack,sizeof ack))brightness_supervisor.ack_published=1;
  }
  return;
 }
 if(brightness_supervisor.highest_seq&&(int32_t)(ack.seq-brightness_supervisor.highest_seq)<=0){
  ack.result=UB_BRIGHTNESS_SAVE_STALE;brightness_publish_save(&ack);return;
 }
 int claim_rc=ub_br_private_read(UB_BRIGHTNESS_SAVE_CLAIM,&claim,sizeof claim);
 if(claim_rc<0||(claim_rc==0&&!brightness_claim_valid(&claim))){brightness_unknown_claim(&ack);return;}
 if(!claim_rc&&claim.pid==ack.pid&&claim.birth_lo==ack.birth_lo&&claim.birth_hi==ack.birth_hi){
  if(memcmp(claim.token,ack.token,16)){brightness_unknown_claim(&ack);return;}
  if(!brightness_supervisor.highest_seq||(int32_t)(claim.seq-brightness_supervisor.highest_seq)>0)brightness_supervisor.highest_seq=claim.seq;
  if(brightness_receipt_same(&claim,&ack)){
   if(claim.result<0){brightness_unknown_claim(&ack);return;}
   ack.result=claim.result;
   brightness_publish_save(&ack);return;
  }
  if((int32_t)(ack.seq-claim.seq)<=0){ack.result=UB_BRIGHTNESS_SAVE_STALE;brightness_publish_save(&ack);return;}
 }
 if(ub_br_birth((unsigned)target->pid,&actual_born)||actual_born!=target->born){
  ack.result=UB_BRIGHTNESS_SAVE_STALE;brightness_publish_save(&ack);return;
 }
 /* Claim BEFORE touching USB. An unknown interrupted save is never replayed
  * after helper restart. Accepted native COMMIT remains authorized across a
  * Close/Open in this same process; its own immutable epoch is used above. */
 claim=ack;claim.result=-1;
 if(ub_br_private_write(UB_BRIGHTNESS_SAVE_CLAIM,&claim,sizeof claim)){
  ack.result=UB_BRIGHTNESS_SAVE_ERROR;brightness_publish_save(&ack);return;
 }
 snprintf(value,sizeof value,"%u",ack.percent);
 rc=ub_setting_set("external_subtitles","brightness",value);
 ack.result=rc?UB_BRIGHTNESS_SAVE_ERROR:UB_BRIGHTNESS_SAVED;
 claim=ack;(void)ub_br_private_write(UB_BRIGHTNESS_SAVE_CLAIM,&claim,sizeof claim);
 brightness_supervisor.seed.percent=ack.percent;
 (void)ub_br_private_write(UB_BRIGHTNESS_SEED,&brightness_supervisor.seed,sizeof brightness_supervisor.seed);
 brightness_publish_save(&ack);
 ub_log("external_subtitles",UB_ALWAYS,"brightness %u%% applied; %s (save result %d)",ack.percent,rc?"not saved to USB":"saved",rc);
}
#endif
