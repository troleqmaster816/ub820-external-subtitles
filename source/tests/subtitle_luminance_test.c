/* Own callback fixtures: no firmware/device renderer is executed. */
#include "../modules/external_subtitles/luminance.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct {uint32_t reserved[2],foreground,background;} gc;
static uint32_t mapped,dirty,cue_start,cue_end,clock_ticks;
static int locked,fg_calls,prepare_calls,render_calls,present_calls,unmap_calls;
static int fg_fail_call,prepare_error,render_error,present_error,unmap_error;
static int prepare_fail_once,render_fail_once,present_fail_once;
static uint32_t outline[4],main_color;
static char display;
static void must_hold_lock(void){assert(locked);}
static int foreground(void *d,void *g,uint32_t color){
 must_hold_lock();assert(d==&display&&g==&gc);fg_calls++;
 if(fg_calls==fg_fail_call)return -1;
 gc.foreground=color;return 0;
}
static void mark_dirty(uint32_t value){must_hold_lock();dirty=value;}
static int prepare(void){must_hold_lock();prepare_calls++;return prepare_error&&(!prepare_fail_once||prepare_calls==1)?prepare_error:0;}
static int render(void){
 must_hold_lock();render_calls++;assert(dirty==1);mapped=0;
 if(render_error&&(!render_fail_once||render_calls==1))return render_error;
 /* Native outline uses the same GC's background for its offset passes,
  * then restores foreground before drawing the main glyph. */
 uint32_t saved=gc.foreground;gc.foreground=gc.background;
 for(unsigned i=0;i<4;i++)outline[i]=gc.foreground;
 gc.foreground=saved;main_color=gc.foreground;dirty=0;return 0;
}
static int present(void){must_hold_lock();present_calls++;if(present_error&&(!present_fail_once||present_calls==1))return present_error;mapped=1;return 0;}
static int unmap(void){must_hold_lock();unmap_calls++;assert(mapped==1);if(unmap_error)return unmap_error;mapped=0;return 0;}
static const struct ub_text_level_api api={foreground,mark_dirty,prepare,render,present,unmap,&mapped};
static void reset(void){
 memset(&gc,0,sizeof gc);gc.foreground=0xffffffff;gc.background=0xff000000;
 mapped=1;dirty=0;locked=1;fg_calls=prepare_calls=render_calls=present_calls=unmap_calls=0;
 fg_fail_call=prepare_error=render_error=present_error=unmap_error=0;
 prepare_fail_once=render_fail_once=present_fail_once=0;
 memset(outline,0,sizeof outline);main_color=0;
 cue_start=0x00110740;cue_end=0x00110959;clock_ticks=0xabcdef;
}
static int apply(unsigned value,unsigned visible,struct ub_text_level_outcome *out){
 uint32_t before_clock=clock_ticks,before_start=cue_start,before_end=cue_end,bg=gc.background;
 int rc=ub_text_level_apply(&api,&display,&gc,gc.foreground,value,visible,out);
 assert(clock_ticks==before_clock&&cue_start==before_start&&cue_end==before_end&&gc.background==bg);
 return rc;
}
int main(void){
 struct ub_text_level_outcome out;unsigned cases=0;
 for(unsigned value=0;value<=255;value++){
  reset();assert(!apply(value,1,&out));uint32_t expected=0xff000000u|value*0x010101u;
  assert(gc.foreground==expected&&mapped==1);
  if(value==255)assert(!fg_calls&&!render_calls&&!out.changed);
  else {assert(fg_calls==1&&prepare_calls==1&&render_calls==1&&present_calls==1&&main_color==expected);
   for(unsigned i=0;i<4;i++)assert(outline[i]==0xff000000);}
  cases++;
 }
 reset();gc.foreground=0x7fffffff;assert(!apply(96,1,&out));assert(gc.foreground==0x7f606060&&main_color==0x7f606060);cases++;
 reset();mapped=0;assert(!apply(80,0,&out));assert(gc.foreground==0xff505050&&dirty==1&&!prepare_calls&&!render_calls&&!present_calls&&mapped==0);cases++;
 reset();fg_fail_call=1;assert(apply(80,1,&out)==UB_TEXT_LEVEL_SET);assert(gc.foreground==0xffffffff&&!dirty&&!render_calls);cases++;
 reset();render_error=6;render_fail_once=1;assert(apply(80,1,&out)==UB_TEXT_LEVEL_RENDER);
 assert(gc.foreground==0xffffffff&&fg_calls==2&&unmap_calls==1&&mapped==1&&!dirty&&out.native_rc==6&&!out.restore_rc&&!out.repaint_rc&&main_color==0xffffffff);cases++;
 reset();present_error=7;present_fail_once=1;assert(apply(80,1,&out)==UB_TEXT_LEVEL_RENDER);assert(mapped==1&&gc.foreground==0xffffffff&&fg_calls==2&&present_calls==2);cases++;
 reset();prepare_error=8;prepare_fail_once=1;assert(apply(80,1,&out)==UB_TEXT_LEVEL_RENDER);assert(render_calls==1&&present_calls==1&&unmap_calls==1&&gc.foreground==0xffffffff&&mapped==1);cases++;
 reset();render_error=6;assert(apply(80,1,&out)==UB_TEXT_LEVEL_RESTORE);assert(out.repaint_rc==6&&unmap_calls==2&&!mapped&&dirty==1&&gc.foreground==0xffffffff);
 render_error=0;assert(!apply(40,0,&out)&&dirty==1);assert(!prepare()&&!render()&&!present());assert(mapped==1&&!dirty&&main_color==0xff282828);cases++;
 reset();render_error=6;fg_fail_call=2;assert(apply(80,1,&out)==UB_TEXT_LEVEL_RESTORE);assert(out.restore_rc==-1&&gc.foreground==0xff505050&&!mapped);cases++;
 reset();render_error=6;unmap_error=9;assert(apply(80,1,&out)==UB_TEXT_LEVEL_RESTORE);assert(out.clear_rc==9&&gc.foreground==0xffffffff);cases++;
 reset();assert(apply(256,1,&out)==UB_TEXT_LEVEL_ARGUMENT);assert(!fg_calls&&!render_calls);cases++;
 reset();assert(ub_text_level_apply(&api,0,&gc,gc.foreground,80,1,&out)==UB_TEXT_LEVEL_ARGUMENT&&!fg_calls);cases++;
 reset();assert(ub_text_level_apply(&api,&display,&gc,gc.foreground,80,1,0)==UB_TEXT_LEVEL_ARGUMENT&&!fg_calls);cases++;
 printf("subtitle_luminance_test passed: %u cases; RGB/alpha/outline, paused cue, gap, native errors and rollback\n",cases);
 return 0;
}
