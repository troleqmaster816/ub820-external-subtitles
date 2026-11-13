#ifndef UB_SUBTITLE_LUMINANCE_H
#define UB_SUBTITLE_LUMINANCE_H
#include <stdint.h>

/* RGB code intensity, not opacity or calibrated HDR nits. The caller owns
 * the native ESL mutex and a live native text GC for the entire operation.
 * TiSetForeground owns its own graphics lock. Outline/background, cue cursor,
 * clock, parser and glyph layout remain with their native implementations. */
#define UB_TEXT_LEVEL_MAX 255u
#define UB_TEXT_LEVEL_DEFAULT 255u
enum ub_text_level_result {
 UB_TEXT_LEVEL_OK=0,UB_TEXT_LEVEL_ARGUMENT=-1,UB_TEXT_LEVEL_SET=-2,
 UB_TEXT_LEVEL_RENDER=-3,UB_TEXT_LEVEL_RESTORE=-4
};
struct ub_text_level_api {
 int (*foreground)(void *,void *,uint32_t);
 void (*dirty)(uint32_t);
 int (*prepare)(void),(*render)(void),(*present)(void),(*unmap)(void);
 volatile uint32_t *mapped;
};
struct ub_text_level_outcome {int native_rc,restore_rc,clear_rc,repaint_rc;unsigned changed;};

static int ub_text_level_redraw(const struct ub_text_level_api *api){
 int rc=api->prepare();
 if(!rc)rc=api->render();
 if(!rc)rc=api->present();
 return rc;
}

static inline uint32_t ub_text_level_color(uint32_t original,unsigned level){
 return (original&UINT32_C(0xff000000))|((uint32_t)level*UINT32_C(0x010101));
}
static int ub_text_color_apply(const struct ub_text_level_api *api,void *display,void *gc,
                               uint32_t previous,uint32_t color,unsigned visible,
                               struct ub_text_level_outcome *out){
 int rc;
 if(!out)return UB_TEXT_LEVEL_ARGUMENT;
 *out=(struct ub_text_level_outcome){0};
 if(!api||!api->foreground||!api->dirty||!display||!gc||
    (visible&&(!api->prepare||!api->render||!api->present||!api->unmap||!api->mapped)))
  return UB_TEXT_LEVEL_ARGUMENT;
 if(color==previous)return UB_TEXT_LEVEL_OK;
 rc=api->foreground(display,gc,color);out->native_rc=rc;
 if(rc)return UB_TEXT_LEVEL_SET;
 api->dirty(1);out->changed=1;
 if(!visible)return UB_TEXT_LEVEL_OK;
 /* Redraw the already selected cue. Calling seconds-based Update here would
  * select a different subsecond cue on pause; do not query/seek time at all. */
 rc=ub_text_level_redraw(api);
 if(!rc)return UB_TEXT_LEVEL_OK;
 out->native_rc=rc;
 /* Match native Update's failed-render cleanup: Render may have reset mapped
  * before failing, but a partial surface must still be cleared/unmapped. */
 *api->mapped=1;out->clear_rc=api->unmap();
 out->restore_rc=api->foreground(display,gc,previous);
 api->dirty(1);
 if(out->restore_rc||out->clear_rc)return UB_TEXT_LEVEL_RESTORE;
 /* One explicit restoration of the old paused image, as native Set_Attr does
  * after a failed change. No time selection, loop or scheduled retry. */
 out->repaint_rc=ub_text_level_redraw(api);
 if(out->repaint_rc){*api->mapped=1;out->clear_rc=api->unmap();api->dirty(1);return UB_TEXT_LEVEL_RESTORE;}
 return UB_TEXT_LEVEL_RENDER;
}
static int ub_text_level_apply(const struct ub_text_level_api *api,void *display,void *gc,
                               uint32_t previous,unsigned level,unsigned visible,
                               struct ub_text_level_outcome *out){
 if(level>UB_TEXT_LEVEL_MAX){if(out)*out=(struct ub_text_level_outcome){0};return UB_TEXT_LEVEL_ARGUMENT;}
 return ub_text_color_apply(api,display,gc,previous,ub_text_level_color(previous,level),visible,out);
}
#endif
