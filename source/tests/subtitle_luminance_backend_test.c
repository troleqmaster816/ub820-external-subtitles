/* New property through the real controller; native calls are own callbacks. */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
static char play_path[160],lock_path[160];
#define UB_PLAY_DAT play_path
#define UB_PLAY_LOCK lock_path
#include "../modules/external_subtitles/backend.c"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static struct {uint32_t reserved[2],foreground,background;} gc;
static char ti;
static void *ti_value=&ti,*gc_value=&gc;
static uint32_t active_value=1,parser_value=1,display_value=1,clock_value=1027,mapped_value=1,dirty_value;
static uint32_t cue_value[2]={0x00110740,0x00110959},seek_value,rendered_color;
static int32_t attrs[5];static unsigned char loaded_value=1;
static unsigned fg_calls,render_calls,present_calls,open_calls,close_calls,update_calls,start_calls;
static unsigned seq;
static int render_failures,fg_failure,in_close,open_error;
static void locked(void){assert(pthread_mutex_trylock(&mutex)==EBUSY);assert(atomic_read(&gate));}
static int foreground(void *display,void *text_gc,uint32_t color){
 locked();assert(!in_close&&display==ti_value&&text_gc==gc_value);fg_calls++;
 if(fg_failure)return -1;
 gc.foreground=color;return 0;
}
static void dirty(uint32_t value){locked();dirty_value=value;}
static int prepare(void){locked();return 0;}
static int render(void){
 locked();render_calls++;assert(dirty_value);mapped_value=0;
 if(render_failures){render_failures--;return 6;}
 rendered_color=gc.foreground;dirty_value=0;return 0;
}
static int present(void){locked();present_calls++;mapped_value=1;return 0;}
static int unmap(void){locked();mapped_value=0;return 0;}
static void seek(uint32_t value){locked();seek_value=value;}
static int update(uint32_t enabled,uint32_t seconds){
 locked();(void)seconds;update_calls++;
 if(enabled){dirty(1);assert(!prepare()&&!render()&&!present());}return 0;
}
static int set_display(uint32_t off){
 assert(atomic_read(&gate));assert(!pthread_mutex_lock(&mutex));display_value=off?0:1;
 if(off)mapped_value=0;
 assert(!pthread_mutex_unlock(&mutex));return 0;
}
static int set_attr(const int32_t *a){
 assert(atomic_read(&gate));assert(!pthread_mutex_lock(&mutex));
 assert(gc.foreground==ub_text_level_color(0xffffffff,text_level));memcpy(attrs,a,sizeof attrs);
 assert(!pthread_mutex_unlock(&mutex));return 0;
}
static int start(const char *path){locked();(void)path;start_calls++;return 0;}
static int native_open(const char *path){
 (void)path;assert(atomic_read(&gate));open_calls++;if(open_error)return open_error;
 assert(!pthread_mutex_lock(&mutex));active_value=parser_value=display_value=1;mapped_value=0;
 ti_value=&ti;gc_value=&gc;gc.foreground=0xffffffff;gc.background=0xff000000;
 assert(!pthread_mutex_unlock(&mutex));return 0;
}
static int native_close(void){
 assert(atomic_read(&gate));close_calls++;assert(!pthread_mutex_lock(&mutex));in_close=1;
 active_value=parser_value=display_value=mapped_value=0;ti_value=gc_value=0;in_close=0;
 assert(!pthread_mutex_unlock(&mutex));return 0;
}
int UFS_OpenDir(const char *path,uint32_t *h){(void)path;(void)h;return -1;}
int UFS_ReadDir(uint32_t h,void *p,uint32_t *n){(void)h;(void)p;(void)n;return -1;}
int UFS_CloseDir(uint32_t h){(void)h;return 0;}
static struct ub_request request(unsigned op){
 struct ub_request r;memset(&r,0,sizeof r);r.magic=UB_MAGIC;r.version=UB_VERSION;r.seq=++seq;r.epoch=state.epoch;r.op=op;return r;
}
static int send_level(unsigned value,struct ub_status *status){
 struct ub_request r=request(UB_OP_SET_TEXT_LEVEL);r.text_level.value=value;return ub_test_process(&r,status);
}
int main(void){
 struct ub_api a={0};struct ub_status result;unsigned cases=0;
 char directory[]="/tmp/ub820-luminance-XXXXXX";assert(mkdtemp(directory));
 snprintf(play_path,sizeof play_path,"%s/play",directory);snprintf(lock_path,sizeof lock_path,"%s/lock",directory);
 a.start=start;a.dirty=dirty;a.seek=seek;a.update=update;a.set_attr=set_attr;a.set_display=set_display;
 a.active=&active_value;a.parser=&parser_value;a.display=&display_value;a.clock=&clock_value;
 a.attrs=attrs;a.loaded=&loaded_value;a.mutex=&mutex;a.set_foreground=foreground;a.ti_display=&ti_value;a.text_gc=&gc_value;
 ub_test_bind(&a);ub_test_reset();ub_original_open=native_open;ub_original_close=native_close;
 ub_precision_renderer=(struct ub_precision_render_api){.prepare=prepare,.render=render,.present=present,.unmap=unmap,.cue=cue_value,.mapped=&mapped_value};
 assert(!ub_hook_open("/mnt/usb/sub/example.srt"));assert(!fg_calls&&!render_calls&&text_level==255);cases++;
 mapped_value=1;precision_last_ticks=UINT64_C(1027)*45000+80*450;precision_drawn=precision_valid=1;
 uint64_t ticks=precision_last_ticks;uint32_t cue[2];memcpy(cue,cue_value,sizeof cue);
 assert(!send_level(80,&result));assert(gc.foreground==0xff505050&&rendered_color==0xff505050&&text_level==80&&mapped_value==1);
 assert(precision_last_ticks==ticks&&!memcmp(cue,cue_value,sizeof cue)&&!seek_value&&!update_calls&&!start_calls&&open_calls==1&&clock_value==1027);cases++;
 unsigned before=fg_calls;assert(!send_level(80,&result));assert(fg_calls==before&&render_calls==1);cases++;
 struct ub_request bad=request(UB_OP_SET_TEXT_LEVEL);bad.text_level.value=80;bad.text_level.reserved[251]=1;
 assert(ub_test_process(&bad,&result)==UB_BAD_PATH&&fg_calls==before&&text_level==80);cases++;
 assert(send_level(256,&result)==UB_BAD_PATH&&fg_calls==before&&text_level==80);cases++;
 bad=request(UB_OP_SET_TEXT_LEVEL);bad.epoch--;bad.text_level.value=40;
 assert(ub_test_process(&bad,&result)==UB_STALE&&fg_calls==before&&text_level==80);cases++;
 unsigned char record[8]={0};FILE *f=fopen(UB_PLAY_DAT,"wb");assert(f);assert(fwrite(record,1,sizeof record,f)==sizeof record);assert(!fclose(f));
 struct ub_request toggle=request(UB_OP_OFF);assert(!ub_test_process(&toggle,&result)&&!display_value);cases++;
 before=render_calls;assert(!send_level(96,&result));assert(gc.foreground==0xff606060&&render_calls==before&&text_level==96);cases++;
 toggle=request(UB_OP_ON);assert(!ub_test_process(&toggle,&result));assert(display_value&&rendered_color==0xff606060&&text_level==96);cases++;
 unsigned before_close_fg=fg_calls;assert(!ub_hook_close());assert(!active_value&&!parser_value&&fg_calls==before_close_fg&&text_level==96);cases++;
 assert(send_level(40,&result)==UB_NO_ENGINE&&fg_calls==before_close_fg&&text_level==96);cases++;
 assert(!ub_hook_open("/mnt/usb/sub/next.srt"));assert(gc.foreground==0xff606060&&text_level==96&&fg_calls==before_close_fg+1&&!mapped_value);cases++;
 /* Inactive reload calls the original Open directly; it too must restore
  * color BEFORE native Set_Attr/Set_Display can draw the new surface. */
 active_value=parser_value=0;mapped_value=0;ti_value=gc_value=0;
 struct ub_request reload=request(UB_OP_RELOAD);assert(!ub_test_process(&reload,&result));
 assert(gc.foreground==0xff606060&&rendered_color==0xff606060&&text_level==96);cases++;
 reload=request(UB_OP_RELOAD);before=open_calls;assert(!ub_test_process(&reload,&result));assert(open_calls==before&&gc.foreground==0xff606060&&start_calls==1);cases++;
 /* A transient failed render restores the OLD paused image once. A later
  * successful value change must still redraw with no seek/clock operation. */
 render_failures=1;before=render_calls;ticks=precision_last_ticks;
 assert(send_level(40,&result)==UB_PARSE_ERROR);assert(text_level==96&&gc.foreground==0xff606060&&rendered_color==0xff606060&&mapped_value==1&&render_calls==before+2);cases++;
 assert(!send_level(40,&result)&&text_level==40&&gc.foreground==0xff282828&&rendered_color==0xff282828&&precision_last_ticks==ticks);cases++;
 render_failures=2;assert(send_level(20,&result)==UB_PARSE_ERROR);assert(text_level==40&&gc.foreground==0xff282828&&!mapped_value&&dirty_value==1&&strstr(result.message,"cleared"));cases++;
 fg_failure=1;before=fg_calls;assert(send_level(20,&result)==UB_PARSE_ERROR&&fg_calls==before+1&&text_level==40);fg_failure=0;cases++;
 atomic_write(&closing,1);before=fg_calls;assert(send_level(20,&result)==UB_NO_ENGINE&&fg_calls==before);atomic_write(&closing,0);cases++;
 unlink(UB_PLAY_DAT);unlink(UB_PLAY_LOCK);assert(!rmdir(directory));
 printf("subtitle_luminance_backend_test passed: %u cases; typed ABI, paused redraw, Off/On, both Open paths, errors and unchanged clock\n",cases);
 return 0;
}
