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

#define ID "external_subtitles"

static int native_options=1,english_text=1;
static const struct ub_option options[]={
 {"font_percent",90,150,&disc_style.font_percent,0},
 {"position",-10,10,&disc_style.position,0},
 {"bottom_align",0,1,&disc_style.bottom_align,0},
 {"native_options",0,1,&native_options,0},
 {"english_text",0,1,&english_text,0},
 {0,0,0,0,0}
};
static void native_step(struct ub_part *p,struct ub_target *t){if(native_options)native_rows_step(p,t);}
static void english_step(struct ub_part *p,struct ub_target *t){if(native_options&&english_text)english_text_step(p,t);}

/* An obsolete status must not terminate the new process's bootstrap. The UI
 * notices its disappearance/epoch change and obtains a fresh LIST. */
static void engine_prepare(struct ub_part *p,struct ub_target *t){
 (void)p;(void)t;unlink(UB_STATUS_FILE);unlink(UB_REQUEST_FILE);
}
/* Preserve the working native font/outline patch. Initial On is established
 * once per playback process; subsequent Off/On belongs to the custom menu. */
static void engine_step(struct ub_part *p,struct ub_target *t){
 int *attempts=&t->state[0];unsigned char loaded=0;struct ub_status status;int sfd,style_rc,have_status=0;
 (void)p;
 if(*attempts>=15)return;
 /* A natural Draw can start the backend worker before this first step. Its
  * status only acknowledges the controller, not supervisor style setup. */
 if(!t->state[2]){
  style_rc=style_patch(t->pid);
  if(style_rc){
   if(style_rc==-2){t->failed=1;*attempts=15;ub_log(ID,UB_ALWAYS,"Style rollback failed; PID%d remains quarantined",t->pid);return;}
   if(++t->state[1]>=15){*attempts=15;ub_log(ID,UB_ALWAYS,"Style initialization unavailable; automatic subtitle startup skipped");}
   else ub_log(ID,UB_INFO,"Style initialization deferred (%d); retrying before subtitle startup",style_rc);
   return;
  }
  font_size_set();t->state[2]=1;
 }
 sfd=open(UB_STATUS_FILE,O_RDONLY);
 if(sfd>=0){ssize_t got=read(sfd,&status,sizeof status);close(sfd);have_status=got==(ssize_t)sizeof status&&status.magic==UB_MAGIC&&status.version==UB_VERSION;}
 if(mem_read(t->pid,LOADED_ADDR,&loaded,1))return;
 /* Applying a safe style transaction is independent of activating subtitles.
  * A live parser/display (including user Off) must never force Initial On. */
 if(loaded || (have_status && status.active)){*attempts=15;return;}
 if(*attempts && have_status){*attempts=15;return;}
 if(*attempts==0){
  if(playdat_set_on() || mem_write_u32(t->pid,GATE_ADDR,1)){
   if(++t->state[1]>=15)*attempts=15;
   ub_log(ID,UB_ALWAYS,"Initial subtitle settings/gate unavailable; load event deferred");return;
  }
 }
 if(evt_load())ub_log(ID,UB_ALWAYS,"Initial subtitle load event failed");
 (*attempts)++;
}

static struct ub_part parts[]={
 {"engine","apl_meda_bdplay",UB_SHA256_APL_MEDA_BDPLAY,&ub_payload_external_subtitles_engine,0,engine_prepare,engine_step,{0},0,0},
 {"native rows","apl_dispsrv",UB_SHA256_APL_DISPSRV,0,0,0,native_step,{0},0,0},
 {"english text","apl_dispsrv",UB_SHA256_APL_DISPSRV,0,1,0,english_step,{0},0,0},
};
/* Marker for the ui module's menu (the core calls start() only when enabled). */
static void start(struct ub_module *m){int fd;(void)m;fd=open(UB_EXTSUB_ENABLED,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd>=0)close(fd);}

struct ub_module ub_module_external_subtitles={
 ID,"External .srt subtitles: native SRT engine and rows, disc-like style (file browser in the ui module)",
 1,options,start,parts,sizeof parts/sizeof *parts,0
};
