/* Actual menu request/status/deadline functions, with rendering APIs stubbed.
 * Time advances without timer callbacks to model a delayed native UI loop. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../modules/external_subtitles/protocol.h"
#ifndef UB_SUBTITLE_UI_DIR
#define UB_SUBTITLE_UI_DIR "/tmp/ub820-subtitle-ui-test"
#endif
#undef UB_REQUEST_FILE
#undef UB_STATUS_FILE
#define UB_REQUEST_FILE UB_SUBTITLE_UI_DIR "/request"
#define UB_STATUS_FILE UB_SUBTITLE_UI_DIR "/status"
static uint32_t seconds=100;
static int failed_clock;
static int test_clock(clockid_t id,struct timespec *out){assert(id==CLOCK_MONOTONIC);if(failed_clock)return -1;out->tv_sec=seconds;out->tv_nsec=0;return 0;}
#define clock_gettime test_clock
#include "../modules/ui/menu.c"
#undef clock_gettime

/* These APIs are linked but not called: tests leave m.active=0, so render()
 * returns before entering CTK. Keep native signatures compiler-checked. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define VOID(name,args) void name args {}
#define TYPE(name) GType name(void){return 1;}
#define NEW(name) Widget name(void){return 0;}
NEW(CTK_window_new) NEW(CTK_fixed_new)
Widget CTK_label_new(const char *text){return 0;}
Widget CTK_stock_button_new(int style,const char *text){return 0;}
TYPE(CTK_window_get_type) TYPE(CTK_fixed_get_type) TYPE(CTK_button_get_type) TYPE(CTK_imagebutton_get_type)
TYPE(gtk_container_get_type) TYPE(gtk_fixed_get_type) TYPE(gtk_label_get_type)
Widget gtk_type_check_object_cast(Widget w,GType type){return w;}
VOID(gtk_widget_set_name,(Widget w,const char *name))
VOID(gtk_widget_show,(Widget w)) VOID(gtk_widget_hide,(Widget w))
VOID(gtk_widget_set_usize,(Widget w,int width,int height)) VOID(gtk_widget_set_uposition,(Widget w,int x,int y))
VOID(gtk_widget_set_state,(Widget w,int state)) VOID(gtk_widget_set_sensitive,(Widget w,int enabled)) VOID(gtk_widget_destroy,(Widget w))
VOID(gtk_container_add,(Widget w,Widget child)) VOID(gtk_fixed_put,(Widget w,Widget child,int x,int y))
VOID(gtk_label_set_text,(Widget w,const char *text)) VOID(gtk_label_set_justify,(Widget w,int alignment))
VOID(CTK_window_set_layer,(Widget w,int layer)) VOID(CTK_fixed_set_shape,(Widget w,int shape)) VOID(CTK_fixed_set_alpha,(Widget w,int alpha))
VOID(CTK_button_set_text,(Widget w,const char *text)) VOID(CTK_button_set_shape,(Widget w,int shape))
VOID(CTK_imagebutton_set_bgpixmap,(Widget w,int state,const char *path))
unsigned int g_timeout_add(unsigned int interval,int (*fn)(void *),void *arg){return 1;}
unsigned int gtk_signal_connect(Widget w,const char *name,void (*fn)(Widget,void *),void *arg){return 1;}
#pragma GCC diagnostic pop

static void publish_ready(unsigned seq){
 struct ub_status status;FILE *f;
 memset(&status,0,sizeof status);status.magic=UB_MAGIC;status.version=UB_VERSION;status.seq=seq;status.epoch=1;status.result=UB_OK;
 strcpy(status.dir,"/mnt/usb");f=fopen(UB_STATUS_FILE,"wb");assert(f);assert(fwrite(&status,1,sizeof status,f)==sizeof status);assert(!fclose(f));
}
int main(void){
 struct ub_request request;FILE *f;
 mkdir(UB_SUBTITLE_UI_DIR,0700);unlink(UB_REQUEST_FILE);unlink(UB_STATUS_FILE);
 assert(!send_request(UB_OP_LIST,"/mnt/usb"));assert(m.pending==1&&m.pending_deadline==103);
 seconds=102;assert(send_request(UB_OP_LIST,"/mnt/usb")==-1&&m.pending==1);assert(strstr(m.notice,"Waiting for subtitle helper"));
 /* No timer dispatch is needed to expire the initial absent-backend wait. */
 seconds=103;assert(subtitle_pending_expire()&&!m.pending&&m.needs_list);assert(strstr(m.notice,"not ready")&&strstr(m.notice,"Helper log"));
 assert(!send_request(UB_OP_LIST,"/mnt/usb")&&m.pending==2);
 publish_ready(m.pending);read_status();assert(!m.pending&&m.epoch==1&&!m.needs_list);
 seconds=200;assert(!send_request(UB_OP_SELECT,"/mnt/usb/movie.srt")&&m.pending_deadline==212);
 seconds=211;assert(!subtitle_pending_expire()&&m.pending);
 seconds=212;assert(subtitle_pending_expire()&&!m.pending&&m.needs_list);assert(strstr(m.notice,"did not answer"));
 assert(send_request(UB_OP_SELECT,"/mnt/usb/movie.srt")==-1&&strstr(m.notice,"Refresh files"));
 assert(!send_request(UB_OP_LIST,"/mnt/usb"));
 /* A click after a delayed timer can retry LIST directly, never generic busy. */
 seconds=225;assert(!send_request(UB_OP_LIST,"/mnt/usb"));
 f=fopen(UB_REQUEST_FILE,"rb");assert(f);assert(fread(&request,1,sizeof request,f)==sizeof request);fclose(f);assert(request.seq==m.pending&&request.op==UB_OP_LIST);
 failed_clock=1;assert(subtitle_pending_expire()&&!m.pending);assert(send_request(UB_OP_LIST,"/mnt/usb")==-1&&!m.pending);
 unlink(UB_REQUEST_FILE);unlink(UB_STATUS_FILE);rmdir(UB_SUBTITLE_UI_DIR);
 puts("subtitle_ui_wait_test passed");return 0;
}
