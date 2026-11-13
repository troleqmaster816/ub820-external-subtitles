#ifndef UB_SUBTITLE_PRECISION_RENDER_H
#define UB_SUBTITLE_PRECISION_RENDER_H

#include <stdint.h>

/* Firmware 1.82 esl_tst_Update (0x20134), with its two zero fractional
 * arguments replaced by centiseconds. Search, cue advancement, glyph layout,
 * mapping, presentation and clearing still use the original engine.
 *
 * CALLER HOLDS the native ESL mutex (0x45710), as for api.update. This does
 * not replace the seconds ABI of ESL_Draw_Subtitle or maintain its last-time
 * field: the caller owns backward-seek detection and lifecycle invalidation.
 */
struct ub_precision_render_api {
 uint32_t (*get_state)(void);
 int32_t (*sync_seconds)(void);
 uint32_t (*get_seek)(void);
 void (*set_seek)(uint32_t);
 int (*search)(void *,uint32_t,uint32_t,uint32_t,uint32_t);
 int (*next)(void *);
 int (*prepare)(void);
 int (*render)(void);
 int (*present)(void);
 int (*unmap)(void);
 void (*store_display)(uint32_t);
 volatile uint32_t *cue;
 volatile uint32_t *mapped;
 int (*needs_update)(void);
};

#ifndef UB_HOST_TEST
static const struct ub_precision_render_api ub_precision_renderer = {
 .get_state=(uint32_t (*)(void))0x1d6a9,
 .sync_seconds=(int32_t (*)(void))0x1da29,
 .get_seek=(uint32_t (*)(void))0x1d719,
 .set_seek=(void (*)(uint32_t))0x1d731,
 .search=(int (*)(void *,uint32_t,uint32_t,uint32_t,uint32_t))0x22271,
 .next=(int (*)(void *))0x22349,
 .prepare=(int (*)(void))0x1ff7d,
 .render=(int (*)(void))0x1f311,
 .present=(int (*)(void))0x1ffb5,
 .unmap=(int (*)(void))0x2000d,
 .store_display=(void (*)(uint32_t))0x1d6f9,
 .cue=(volatile uint32_t *)0x45758,
 .mapped=(volatile uint32_t *)0x45970,
 .needs_update=(int (*)(void))0x206c5
};
#else
static struct ub_precision_render_api ub_precision_renderer;
#endif

static int ub_precision_render_update_impl(uint32_t display,uint32_t seconds,
                                           uint32_t centiseconds,int skip_unchanged) {
 const struct ub_precision_render_api *p=&ub_precision_renderer;
 uint32_t hour,minute,second,now,hours,remainder;int rc=0;int64_t adjusted;
 if(!p->get_state())return 0;
 if(!display){p->unmap();rc=1;goto done;}
 if(centiseconds>99)return 7;
 /* The stock input guard reserves 30 seconds below INT32_MAX. Compute the
  * signed synchronization offset without C signed overflow, retaining its
  * seconds unit and the supplied fractional part. */
 if(seconds>0x7fffffe1u){p->unmap();goto done;}
 adjusted=(int64_t)seconds+p->sync_seconds();
 if(adjusted<0 || adjusted>0x7fffffff){p->unmap();goto done;}
 seconds=(uint32_t)adjusted;
 /* Same multiply/shift constants as stock 0x2020a..0x20266. Explicitly use
  * them so -Os cannot introduce unbound __aeabi_idiv[mod] imports into the
  * injected object. ARMv7 implements these products with UMULL. */
 hours=(uint32_t)(((uint64_t)seconds*0x91a2b3c5u)>>43);
 remainder=seconds-hours*3600u;
 minute=(uint32_t)(((uint64_t)remainder*0x88888889u)>>37);
 second=remainder-minute*60u;hour=(uint8_t)hours;
 if(p->get_seek()==1){
  p->set_seek(0);
  rc=p->search((void *)p->cue,hour,minute,second,centiseconds);
  if(rc!=0 && rc!=4)return 7;
  if(rc==4){rc=0;p->unmap();goto done;}
 }
 now=(hour<<24)|(minute<<16)|(second<<8)|centiseconds;
 if(p->cue[0]<=now && now<p->cue[1])goto render;
 if(p->cue[0]>now){p->unmap();rc=0;goto done;}
 do {
  rc=p->next((void *)p->cue);
 } while(rc==0 && p->cue[1]<=now);
 if(rc!=0 || p->cue[0]>now){p->unmap();rc=0;goto done;}
render:
 /* Check only AFTER cue selection/expiry. Native 0x206c4 compares the full
  * current/previous cue (0x10c bytes), mapped state and dirty flag. Its false
  * result means Render itself would do nothing, but stock Present would
  * still blit/flush. Avoid that repeated work for the fractional scheduler.
  * The same engine mutex covers selection, this check and presentation. */
 if(skip_unchanged && *p->mapped==1 && p->needs_update && !p->needs_update()){
  rc=0;goto done;
 }
 p->prepare();
 rc=p->render();
 if(rc){
  /* Stock render-failure path forces Unmap to clear a partial surface. */
  *p->mapped=1;p->unmap();return rc;
 }
 p->present();
done:
 p->store_display(display);return rc;
}

/* The unoptimized entry preserves native mutation/presentation call order;
 * the fast entry additionally suppresses presentation of an unchanged cue. */
static inline int ub_precision_render_update(uint32_t display,uint32_t seconds,
                                             uint32_t centiseconds) {
 return ub_precision_render_update_impl(display,seconds,centiseconds,0);
}
static inline int ub_precision_render_update_fast(uint32_t display,uint32_t seconds,
                                                  uint32_t centiseconds) {
 return ub_precision_render_update_impl(display,seconds,centiseconds,1);
}

#endif
