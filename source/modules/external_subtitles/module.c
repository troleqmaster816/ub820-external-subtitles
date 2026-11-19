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

#define ID "external_subtitles"

static int native_options=1,english_text=1,precision=1,brightness_config=100;
static const struct ub_option options[]={
 {"font_percent",90,150,&disc_style.font_percent,0},
 {"position",-10,10,&disc_style.position,0},
 {"bottom_align",0,1,&disc_style.bottom_align,0},
 {"native_options",0,1,&native_options,0},
 {"english_text",0,1,&english_text,0},
 {"precision",0,1,&precision,0},
 {"brightness",0,100,&brightness_config,0},
 {0,0,0,0,0}
};
static void native_step(struct ub_part *p,struct ub_target *t){if(native_options)native_rows_step(p,t);}
static void english_step(struct ub_part *p,struct ub_target *t){if(native_options&&english_text)english_text_step(p,t);}
static void precision_prepare(struct ub_part *p,struct ub_target *t){
 (void)p;if(!precision)t->failed=1;
}

/* An obsolete status must not terminate the new process's bootstrap. The UI
 * notices its disappearance/epoch change and obtains a fresh LIST. */
static void engine_prepare(struct ub_part *p,struct ub_target *t){
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
}
/* Preserve the working native font/outline patch. Initial On is established
 * once per playback process; subsequent Off/On belongs to the custom menu.
 * Nothing here waits: a busy lock or a running child defers to a later tick.
 * state: 0 load events sent (15 = done), 1 style failures, 2 style done,
 * 3/4 calibration done/failures, 5 last event result, 6 busy lock logged,
 * 7 settings/gate failures. A busy lock is never a failure. */
static void engine_step(struct ub_part *p,struct ub_target *t){
 int *attempts=&t->state[0];unsigned char loaded=0;struct ub_status status;int sfd,style_rc,have_status=0,on;pid_t child;
 (void)p;
 brightness_save_tick(t);
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
  on=playdat_set_on();
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
static void start(struct ub_module *m){int fd;(void)m;fd=open(UB_EXTSUB_ENABLED,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd>=0)close(fd);}

struct ub_module ub_module_external_subtitles={
 ID,"External .srt subtitles: native SRT engine and rows, disc-like style (file browser in the ui module)",
 1,options,start,parts,sizeof parts/sizeof *parts,0,0
};
