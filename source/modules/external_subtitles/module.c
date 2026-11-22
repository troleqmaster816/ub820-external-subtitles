/* External subtitles: the SRT engine controller inside apl_meda_bdplay with
 * the disc-like style, the player's own hidden subtitle rows (native_rows.h)
 * and their English text (english_text.h). The OPTIONS file browser is drawn
 * by the ui module, which shows it while this module is enabled. Each is a separate part, so the circuit breaker switches
 * off only the one that coincides with a crash. The owner-proven
 * engage_core.c shares transaction-verified styling through renames. */
#define main ub_engage_unused_main
#define style_patch legacy_style_patch
#define font_size_set legacy_font_size_set
#define playdat_set_on legacy_playdat_set_on
#include "engage_core.c"
#undef main
#undef style_patch
#undef font_size_set
#undef playdat_set_on
#include "ub.h"
#include "payloads.h"
#include "protocol.h"
#include "helper_menu.h"
#include "style.h"
#include "native_rows.h"
#include "english_text.h"
#include "brightness_supervisor.h"
#include "precision_clock.h"
#include "subtitle_memory.h"
#include "disc_id.h"

#define ID "external_subtitles"

static int native_options=1,english_text=1,precision=1,brightness_config=100,disc_memory=1;
/* play_dat byte 2: 0 Normal, 1 Small, 2 Large (esl_tst_conv_fontsizeESL2Val). */
static const char *const onoff_names[]={"off","on",0};
static const char *const size_names[]={"normal","small","large",0};
static const struct ub_option options[]={
 {"font_percent",90,150,&disc_style.font_percent,0},
 /* The player's own range (FUN_0001c424, ESL_Set_Attr): wider settings
  * are common on 2.39:1 screens. */
 {"position",-32,32,&disc_style.position,0},
 {"size",0,2,&disc_style.size,size_names},
 {"bottom_align",0,1,&disc_style.bottom_align,0},
 {"native_options",0,1,&native_options,0},
 {"english_text",0,1,&english_text,0},
 {"precision",0,1,&precision,0},
 {"brightness",0,100,&brightness_config,0},
 /* Per-disc file, sync and character code (sub/ub820_subtitles.cfg). */
 {"memory",0,1,&disc_memory,onoff_names},
 {0,0,0,0,0}
};
static void native_step(struct ub_part *p,struct ub_target *t){if(native_options)native_rows_step(p,t);}
static void english_step(struct ub_part *p,struct ub_target *t){if(native_options&&english_text)english_text_step(p,t);}
static void precision_prepare(struct ub_part *p,struct ub_target *t){
 (void)p;if(!precision)t->failed=1;
}

/* ---- subtitle memory: last folder and the player's own settings ---------- */
static struct sub_memory memory;
static struct save_sched memory_save,prefs_save;
static struct prefs_obs prefs;
static int prefs_pid;static unsigned long long prefs_born,engine_seen_ms;
static uint32_t selection_seq_seen;
/* This playback: its disc (empty: none or memory=off), what the start write
 * puts in play_dat b3/b4, and the file it opened last (selection record). */
static char disc_cur[33];
static int start_sync,start_code,learn_pending;
static unsigned last_kind;static char last_path[UB_SUB_PATH_BYTES];
static int memory_text_read(const char *path,char *out,size_t cap){
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);size_t used=0;ssize_t n;
 if(fd<0)return -1;
 while(used<cap-1){n=read(fd,out+used,cap-1-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
 {char extra;do{n=read(fd,&extra,1);}while(n<0&&errno==EINTR);if(n)used=cap;} /* too long or unreadable: refuse */
 close(fd);if(used>=cap-1)return -1;out[used]=0;return 0;
}
/* Copies the stick's file to RAM and parses it. 0 read, 1 absent, -1 unknown. */
static int memory_fetch(struct sub_memory *into){
 static char text[96*1024];
 int rc=ub_stick_load(UB_STICK_DIR "/" UB_SUB_MEMORY_NAME,UB_SUB_CONFIG_DIR "/" UB_SUB_MEMORY_NAME,1);
 memset(into,0,sizeof *into);
 if(rc==UB_STICK_ABSENT)return 1;
 if(rc||memory_text_read(UB_SUB_CONFIG_DIR "/" UB_SUB_MEMORY_NAME,text,sizeof text))return -1;
 sub_memory_parse(into,text);
 if(into->rejected)ub_log(ID,UB_ALWAYS,"%s/%s: %u malformed line(s) ignored",UB_STICK_DIR,UB_SUB_MEMORY_NAME,into->rejected);
 return 0;
}
static void memory_load(void){
 int rc=memory_fetch(&memory);
 memory.known=rc>=0;
 if(rc<0)ub_log(ID,UB_ALWAYS,"%s/%s could not be read; the last folder is kept in RAM and the file is not replaced until it reads",UB_STICK_DIR,UB_SUB_MEMORY_NAME);
}
/* After a failed read (a stick still starting at boot): the file's entries
 * join RAM, where everything learnt since wins. 0 known now, -1 still not. */
static int memory_retry(void){
 struct sub_memory file;int f;
 if(memory.known)return 0;
 f=memory_fetch(&file);
 if(f<0)return -1;
 if(!f)sub_memory_merge(&memory,&file);
 memory.known=1;
 return 0;
}
static void memory_save_now(unsigned long long now){
 static char text[96*1024];static const char path[]=UB_SUB_CONFIG_DIR "/" UB_SUB_MEMORY_NAME ".out";
 size_t n;int fd,rc=0;size_t done=0;
 if(memory_retry()){sched_result(&memory_save,now,0);ub_log(ID,UB_ALWAYS,"%s/%s still unreadable; subtitle memory not saved",UB_STICK_DIR,UB_SUB_MEMORY_NAME);return;}
 n=sub_memory_format(&memory,text,sizeof text);
 mkdir(UB_SUB_CONFIG_DIR,0700);
 fd=n?open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600):-1;
 if(fd<0)rc=-1;
 while(!rc&&done<n){ssize_t w=write(fd,text+done,n-done);if(w<0&&errno==EINTR)continue;if(w<=0)rc=-1;else done+=(size_t)w;}
 if(fd>=0&&close(fd))rc=-1;
 if(!rc)rc=ub_stick_write(path,UB_STICK_DIR "/" UB_SUB_MEMORY_NAME);
 unlink(path);
 sched_result(&memory_save,now,!rc);
 if(rc)ub_log(ID,UB_ALWAYS,"subtitle memory not saved to USB (%s)",rc>0?ub_stick_error(rc):"RAM copy failed");
}
static void prefs_save_now(unsigned long long now){
 int rc=ub_setting_flush();
 sched_result(&prefs_save,now,!rc);
 if(rc)ub_log(ID,UB_ALWAYS,"subtitle position/size applied; not saved to USB (%s)",ub_stick_error(rc));
}
/* Every tick (the "load event" part). forced: playback ended since. */
static void memory_tick(void){
 unsigned long long now=ub_now_ms();int forced=engine_seen_ms&&now-engine_seen_ms>2000;
 if(forced)engine_seen_ms=0;
 if(sched_due(&prefs_save,now,forced))prefs_save_now(now);
 if(sched_due(&memory_save,now,forced))memory_save_now(now);
}
static int playdat_stable(unsigned char out[8]){
 unsigned char a[8],b[8];
 for(int i=0;i<2;i++){
  int fd=open(PLAY_DAT,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);ssize_t n;unsigned char *dst=i?b:a;
  if(fd<0)return -1;
  do{n=read(fd,dst,8);}while(n<0&&errno==EINTR);
  close(fd);if(n!=8)return -1;
 }
 if(memcmp(a,b,8))return -1;
 memcpy(out,a,8);return 0;
}
static void prefs_apply(unsigned mask){
 unsigned long long now=ub_now_ms();char value[16];int staged=0;
 if(mask&(1u<<PREF_POSITION)){
  snprintf(value,sizeof value,"%d",(int)(signed char)prefs.base[PREF_POSITION]);
  if(!ub_setting_stage(ID,"position",value)){staged=1;ub_log(ID,UB_ALWAYS,"subtitle position %s chosen in the player's menu; saving",value);}
 }
 if(mask&(1u<<PREF_SIZE)){
  const char *name=size_names[prefs.base[PREF_SIZE]];
  if(!ub_setting_stage(ID,"size",name)){staged=1;ub_log(ID,UB_ALWAYS,"subtitle size %s chosen in the player's menu; saving",name);}
 }
 if(staged)sched_changed(&prefs_save,now);
}
/* The file opened in this playback (payload record) and the player's own
 * settings. Called first in engine_step, every tick while it runs. */
static void subtitle_memory_poll(struct ub_target *t){
 struct ub_sub_selection sel;unsigned char bytes[8],loaded=0;
 engine_seen_ms=ub_now_ms();
 if(t->pid!=prefs_pid||t->born!=prefs_born)return; /* engine_prepare has not run for it */
 /* The choice first: an unstable play_dat reading must not delay it. */
 if(!ub_br_private_read(UB_SUB_SELECTION_FILE,&sel,sizeof sel)&&
    ub_sub_selection_valid(&sel,(uint32_t)t->pid,t->born)&&sel.seq!=selection_seq_seen){
  char dir[UB_SUB_PATH_BYTES];
  selection_seq_seen=sel.seq;last_kind=sel.kind;learn_pending=1;
  memcpy(last_path,sel.path,sizeof last_path);last_path[sizeof last_path-1]=0;
  if(sel.kind==UB_SUB_KIND_CHOSEN&&!sub_dirname(sel.path,dir,sizeof dir)&&strcmp(dir,memory.last_dir)){
   snprintf(memory.last_dir,sizeof memory.last_dir,"%s",dir);sched_changed(&memory_save,ub_now_ms());
   ub_log(ID,UB_INFO,"subtitle browser will start in %s",dir);
  }
 }
 if(playdat_stable(bytes)||mem_read(t->pid,LOADED_ADDR,&loaded,1))return;
 if(learn_pending){
  /* The file opened; its sync/charcode as they are now (a failed autoload
   * has reset them) are the reference, and kind 2/3 is what the disc keeps. */
  learn_pending=0;prefs_rebase_file(&prefs,bytes);
  if(disc_learn_selection(&memory,disc_cur,last_kind,last_path,bytes)){
   sched_changed(&memory_save,ub_now_ms());
   if(last_kind==UB_SUB_KIND_CHOSEN)ub_log(ID,UB_INFO,"disc %.8s: remembering %s",disc_cur,last_path);
  }
 }
 if(!prefs.armed){
  /* The engine runs without this playback's start write (it was already
   * loaded, or startup gave up): the current settings are the reference. */
  if(t->state[0]>=15&&loaded)prefs_arm(&prefs,bytes);
  return;
 }
 {unsigned mask=prefs_sample(&prefs,bytes,loaded==1);
  prefs_apply(mask);
  if(disc_learn_prefs(&memory,disc_cur,last_kind,mask,prefs.base)){
   sched_changed(&memory_save,ub_now_ms());
   ub_log(ID,UB_INFO,"disc %.8s: sync %d s, character code %s remembered",disc_cur,
    disc_sync(prefs.base[PREF_SYNC]),disc_code(prefs.base[PREF_CODE])?"GB2312":"UTF-8");
  }
 }
}

/* An obsolete status must not terminate the new process's bootstrap. The UI
 * notices its disappearance/epoch change and obtains a fresh LIST. */
static void engine_prepare(struct ub_part *p,struct ub_target *t){
 struct ub_sub_hint h;int entry=-1;
 prefs_disarm(&prefs);prefs_pid=t->pid;prefs_born=t->born;selection_seq_seen=0;
 disc_cur[0]=0;start_sync=start_code=0;learn_pending=0;last_kind=0;last_path[0]=0;
 unlink(UB_SUB_SELECTION_FILE);
 memset(&h,0,sizeof h);
 h.magic=UB_SUB_HINT_MAGIC;h.version=UB_SUB_RECORD_VERSION;h.pid=(uint32_t)t->pid;
 h.birth_lo=(uint32_t)t->born;h.birth_hi=(uint32_t)(t->born>>32);
 /* The disc lookup needs the stick's entries: a failed boot read is retried
  * once per playback, not only when something is saved. */
 if(!memory.known&&!memory_retry())ub_log(ID,UB_ALWAYS,"%s/%s read on retry",UB_STICK_DIR,UB_SUB_MEMORY_NAME);
 if(memory.last_dir[0]){h.flags|=UB_SUB_HINT_START_DIR;snprintf(h.start_dir,sizeof h.start_dir,"%s",memory.last_dir);}
 if(disc_memory){
  char root[200];unsigned long long began=ub_now_ms();
  if(ub_disc_root(root,sizeof root)&&!ub_disc_id(root,disc_cur)){
   entry=sub_memory_find(&memory,disc_cur);
   ub_log(ID,UB_INFO,"disc %.8s (%s, %llu ms)%s",disc_cur,root,ub_now_ms()-began,entry>=0?": remembered subtitle file":"");
  }else{disc_cur[0]=0;ub_log(ID,UB_INFO,"no Blu-ray identifier for this playback; disc memory inactive");}
 }
 if(entry>=0){
  const struct sub_disc *d=&memory.disc[entry];
  h.flags|=UB_SUB_HINT_AUTOLOAD;memcpy(h.autoload,d->path,sizeof h.autoload);h.autoload[sizeof h.autoload-1]=0;
  h.sync=(int8_t)d->sync;h.charcode=(int8_t)d->charcode;start_sync=d->sync;start_code=d->charcode;
  sub_memory_front(&memory,(unsigned)entry); /* RAM order only; saved with the next change */
 }
 if(h.flags){
  if(ub_br_private_write(UB_SUB_HINT_FILE,&h,sizeof h)){
   unlink(UB_SUB_HINT_FILE);start_sync=start_code=0;
   ub_log(ID,UB_ALWAYS,"subtitle start folder/remembered file could not be handed over");
  }
 }else unlink(UB_SUB_HINT_FILE);
 (void)p;brightness_seed_prepare(t,(unsigned)brightness_config);unlink(UB_STATUS_FILE);unlink(UB_REQUEST_FILE);
 /* precision=0 reaches the engine as a RAM marker, read once by its clock
  * worker: no clock client loop, the stock whole-second Draw. */
 if(precision)unlink(UB_PC_DISABLED_FILE);
 else{int fd=open(UB_PC_DISABLED_FILE,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
  if(fd>=0)close(fd);else ub_log(ID,UB_ALWAYS,"precision=0 marker unavailable; the engine keeps its clock client");}
}
/* The evtSend child posts to the vendor EV queue, not to one PID, so a single
 * slot serves every bdplay instance. It is never waited for: later ticks reap
 * it with WNOHANG, and no second load event starts while it runs. The engine
 * step polls it while its playback runs; the "load event" part (no process,
 * so it runs on every tick) polls it otherwise, e.g. after STOP ended that
 * playback. An unknown result belongs to the instance that sent the event. */
#define EVT_DEADLINE 5
enum { EVT_OK, EVT_UNKNOWN, EVT_FAILED };
static pid_t evt_child;static int evt_ticks,evt_polled,evt_owner,evt_result;
static unsigned long long evt_owner_born;
static void evt_poll(void){
 int code=0,rc;
 evt_polled=1;
 if(!evt_child)return;
 rc=evt_reap(evt_child,&code);
 if(!rc){
  if(++evt_ticks==EVT_DEADLINE){kill(evt_child,SIGKILL);ub_log(ID,UB_ALWAYS,"Initial subtitle load event did not finish; stopped");}
  return;
 }
 if(rc<0||code<0){evt_result=EVT_UNKNOWN;ub_log(ID,UB_ALWAYS,"Initial subtitle load event result unknown; checking the engine before any retry");}
 else if(code){evt_result=EVT_FAILED;ub_log(ID,UB_ALWAYS,"Initial subtitle load event failed");}
 else evt_result=EVT_OK;
 evt_child=0;evt_ticks=0;
}
/* Results belong to the playback that sent the event, including when the
 * every-tick part reaped the child after STOP. */
static int evt_busy(struct ub_target *t){
 evt_poll();
 if(!evt_child && evt_owner==t->pid && evt_owner_born==t->born){
  t->state[5]=evt_result;evt_owner=0;
 }
 return evt_child!=0;
}
static void evt_step(struct ub_part *p,struct ub_target *t){
 (void)p;(void)t;
 if(!evt_polled)evt_poll();
 evt_polled=0;
 memory_tick();
}
/* Preserve the working native font/outline patch. Initial On is established
 * once per playback process; subsequent Off/On belongs to the custom menu.
 * Nothing here waits: a busy lock or a running child defers to a later tick.
 * state: 0 load events sent (15 = done), 1 style failures, 2 style done,
 * 3/4 calibration done/failures, 5 last event result, 6 busy lock logged,
 * 7 settings/gate failures. A busy lock is never a failure. */
static void engine_step(struct ub_part *p,struct ub_target *t){
 int *attempts=&t->state[0];unsigned char loaded=0,written[8];struct ub_status status;int sfd,style_rc,have_status=0,on;pid_t child;
 (void)p;
 brightness_save_tick(t);
 subtitle_memory_poll(t);
 if(evt_busy(t))return;
 if(*attempts>=15)return;
 /* A natural Draw can start the backend worker before this first step. Its
  * status only acknowledges the controller, not supervisor style setup. */
 if(!t->state[2]){
  style_rc=style_patch(t->pid);
  if(style_rc){
   if(style_rc==-2){t->failed=1;*attempts=15;ub_log(ID,UB_ALWAYS,"Style rollback failed; PID%d remains quarantined",t->pid);return;}
   if(++t->state[1]>=15){*attempts=15;ub_log(ID,UB_ALWAYS,"Style initialization unavailable; automatic subtitle startup skipped");}
   else ub_log(ID,UB_INFO,"Style initialization deferred (%d: %s); retrying before subtitle startup",style_rc,
    style_rc==-3?"library preflight":ub_patch_stage());
   return;
  }
  t->state[2]=1;
 }
 /* Native Open reads the calibration once. Retry this write alone (never the
  * code transaction) before the load event; after 15 failures subtitles start
  * at the native size rather than not at all. */
 if(!t->state[3]){
  if(!font_size_set())t->state[3]=1;
  else if(++t->state[4]>=15){t->state[3]=1;ub_log(ID,UB_ALWAYS,"Font calibration write failed; subtitles start at the native size");}
  else{ub_log(ID,UB_INFO,"Font calibration write failed; retrying before subtitle startup");return;}
 }
 sfd=open(UB_STATUS_FILE,O_RDONLY);
 if(sfd>=0){ssize_t got=read(sfd,&status,sizeof status);close(sfd);have_status=got==(ssize_t)sizeof status&&status.magic==UB_MAGIC&&status.version==UB_VERSION;}
 if(mem_read(t->pid,LOADED_ADDR,&loaded,1))return;
 /* Applying a safe style transaction is independent of activating subtitles.
  * A live parser/display (including user Off) must never force Initial On. */
 if(loaded || (have_status && status.active)){*attempts=15;return;}
 if(*attempts && have_status && t->state[5]!=EVT_FAILED){*attempts=15;return;}
 /* An event of unknown result may still take effect: one more tick of
  * evidence (loaded flag, engine status) before another is sent. */
 if(t->state[5]==EVT_UNKNOWN){t->state[5]=EVT_OK;return;}
 if(*attempts==0){
  on=playdat_set_on_disc(start_sync,start_code,written);
  /* The settings this playback starts from: only later changes are choices. */
  if(!on&&t->pid==prefs_pid&&t->born==prefs_born)prefs_arm(&prefs,written);
  if(on>0){
   if(!t->state[6]){t->state[6]=1;ub_log(ID,UB_INFO,"Subtitle settings are locked by the player; load event deferred until they are free");}
   return;
  }
  t->state[6]=0;
  if(on || mem_write_u32(t->pid,GATE_ADDR,1)){
   if(++t->state[7]>=15){*attempts=15;ub_log(ID,UB_ALWAYS,"Initial subtitle settings/gate unavailable; automatic subtitle startup skipped");}
   else ub_log(ID,UB_ALWAYS,"Initial subtitle settings/gate unavailable; load event deferred");
   return;
  }
 }
 child=evt_start(evt_load_argv);
 if(child<0){t->state[5]=EVT_FAILED;ub_log(ID,UB_ALWAYS,"Initial subtitle load event failed");}
 else{evt_child=child;evt_ticks=0;evt_owner=t->pid;evt_owner_born=t->born;}
 (*attempts)++;
}

/* apl_bdv normally exits between discs. The core breaker cannot distinguish
 * that from a crash, so keep this per-disc source unguarded like the engine.
 * Each new process still requires its exact firmware and validated install. */
static struct ub_part parts[]={
 {"precise clock","apl_bdv",UB_SHA256_APL_BDV,&ub_payload_external_subtitles_precision_source,0,precision_prepare,0,{0},0,0},
 {"engine","apl_meda_bdplay",UB_SHA256_APL_MEDA_BDPLAY,&ub_payload_external_subtitles_engine,0,engine_prepare,engine_step,{0},0,0},
 {"native rows","apl_dispsrv",UB_SHA256_APL_DISPSRV,0,0,0,native_step,{0},0,0},
 {"english text","apl_dispsrv",UB_SHA256_APL_DISPSRV,0,1,0,english_step,{0},0,0},
 {"load event",0,0,0,0,0,evt_step,{0},0,0}, /* after the engine: see evt_poll */
};
/* Marker for the ui module's menu (the core calls start() only when enabled). */
static void start(struct ub_module *m){int fd;(void)m;fd=open(UB_EXTSUB_ENABLED,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd>=0)close(fd);memory_load();}

struct ub_module ub_module_external_subtitles={
 ID,"External .srt subtitles: native SRT engine and rows, disc-like style (file browser in the ui module)",
 1,options,start,parts,sizeof parts/sizeof *parts,0,0
};
