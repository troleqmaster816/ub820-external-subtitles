/* Actual UI/controller IPC with own files and native-widget callbacks. */
#define _GNU_SOURCE
#ifndef UB_HOST_TEST
#error "Brightness UI fixtures require UB_HOST_TEST"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../modules/external_subtitles/protocol.h"
#include "helper_menu.h"
static char request_path[256],status_path[256],save_path[256],keys_path[256],abort_path[256];
#undef UB_REQUEST_FILE
#undef UB_STATUS_FILE
#undef UB_BRIGHTNESS_SAVE_ACK
#undef UB_KEYS_FIFO
#undef UB_OVERLAY_ABORT
#define UB_REQUEST_FILE request_path
#define UB_STATUS_FILE status_path
#define UB_BRIGHTNESS_SAVE_ACK save_path
#define UB_KEYS_FIFO keys_path
#define UB_OVERLAY_ABORT abort_path
static uint32_t seconds=100;
static int ui_clock(clockid_t id,struct timespec *t){assert(id==CLOCK_MONOTONIC);t->tv_sec=seconds;t->tv_nsec=0;return 0;}
#define clock_gettime ui_clock
#include "../modules/ui/menu.c"
#undef clock_gettime

struct widget {int x,y,w,h,min,max,indicator,destroyed;char name[96],text[256];};
static struct widget widgets[256];static unsigned used;
static int deferred_read;
static Widget widget_new(void){assert(used<256);return &widgets[used++];}
static struct widget *widget(Widget w){assert(w>=((Widget)widgets)&&w<((Widget)(widgets+256)));return w;}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define TYPE(n) GType n(void){return 1;}
#define VOID(n,args) void n args {}
Widget CTK_window_new(void){return widget_new();}Widget CTK_fixed_new(void){return widget_new();}
Widget CTK_plotbar_new(void){return widget_new();}
Widget CTK_label_new(const char *t){return widget_new();}Widget CTK_stock_button_new(int s,const char *t){return widget_new();}
TYPE(CTK_window_get_type) TYPE(CTK_fixed_get_type) TYPE(CTK_plotbar_get_type) TYPE(CTK_button_get_type) TYPE(CTK_imagebutton_get_type)
TYPE(gtk_container_get_type) TYPE(gtk_fixed_get_type) TYPE(gtk_label_get_type)
Widget gtk_type_check_object_cast(Widget w,GType t){return w;}
void gtk_widget_set_name(Widget w,const char *s){snprintf(widget(w)->name,sizeof widget(w)->name,"%s",s);}
VOID(gtk_widget_show,(Widget w)) VOID(gtk_widget_hide,(Widget w)) VOID(gtk_widget_set_state,(Widget w,int s)) VOID(gtk_widget_set_sensitive,(Widget w,int s))
void gtk_widget_destroy(Widget w){widget(w)->destroyed=1;}
void gtk_widget_set_usize(Widget w,int a,int b){widget(w)->w=a;widget(w)->h=b;}
void gtk_widget_set_uposition(Widget w,int a,int b){widget(w)->x=a;widget(w)->y=b;}
VOID(gtk_container_add,(Widget w,Widget c)) VOID(gtk_fixed_put,(Widget w,Widget c,int x,int y))
void gtk_label_set_text(Widget w,const char *s){ha_copy(widget(w)->text,s,sizeof widget(w)->text);}
void CTK_button_set_text(Widget w,const char *s){gtk_label_set_text(w,s);}
VOID(gtk_label_set_justify,(Widget w,int j)) VOID(CTK_window_set_layer,(Widget w,int l))
VOID(CTK_fixed_set_shape,(Widget w,int s)) VOID(CTK_fixed_set_alpha,(Widget w,int a)) VOID(CTK_button_set_shape,(Widget w,int s))
VOID(CTK_imagebutton_set_bgpixmap,(Widget w,int s,const char *p))
void CTK_plotbar_set_min_value(Widget w,int v){widget(w)->min=v;}void CTK_plotbar_set_max_value(Widget w,int v){widget(w)->max=v;}
void CTK_plotbar_set_indicator(Widget w,int v){assert(v>=0&&v<=100);widget(w)->indicator=v;}
unsigned g_timeout_add(unsigned i,int(*f)(void *),void *p){return 1;}
unsigned gtk_signal_connect(Widget w,const char *n,void(*f)(Widget,void *),void *p){return 1;}
#pragma GCC diagnostic pop

static void put(const char *path,const void *data,size_t size){FILE *f=fopen(path,"wb");assert(f&&!fchmod(fileno(f),0600)&&fwrite(data,1,size,f)==size&&!fclose(f));}
static struct ub_request queued(void){struct ub_request r;FILE *f=fopen(request_path,"rb");assert(f&&fread(&r,1,sizeof r,f)==sizeof r&&fgetc(f)==EOF&&!fclose(f));return r;}
static void reset(unsigned pct,unsigned raw){
 memset(&m,0,sizeof m);memset(&brightness,0,sizeof brightness);memset(widgets,0,sizeof widgets);used=0;seconds=100;deferred_read=0;
 unlink(request_path);unlink(status_path);unlink(save_path);
 m.kind=PAGE_SUBS;m.epoch=10;m.status.magic=UB_MAGIC;m.status.version=UB_VERSION;m.status.epoch=10;
 strcpy(m.status.dir,"/mnt/usb");m.status.brightness_available=m.status.brightness_known=1;
 m.status.brightness_percent=pct;m.status.text_level=raw;m.status.brightness_pid=6001;m.status.brightness_birth_lo=8;m.status.brightness_birth_hi=1;
 memset(m.status.brightness_token,0xa5,16);assert(!create_child());m.selected=2;
}
static void response(struct ub_request r,int result,unsigned pct,unsigned raw){
 struct ub_status s=m.status;s.seq=r.seq;s.epoch=r.epoch;s.result=result;s.text_level=raw;s.brightness_percent=pct;
 s.brightness_edit=r.op==UB_OP_BRIGHTNESS_BEGIN?r.seq:r.brightness.edit;s.brightness_edit_epoch=r.epoch;s.brightness_phase=UB_BRIGHTNESS_PREVIEW;
 s.brightness_owner_pid=r.brightness.owner_pid;s.brightness_owner_start_lo=r.brightness.owner_start_lo;s.brightness_owner_start_hi=r.brightness.owner_start_hi;
 if(r.op==UB_OP_BRIGHTNESS_CANCEL&&result==UB_OK){s.brightness_edit=0;s.brightness_phase=UB_BRIGHTNESS_NONE;}
 if(r.op==UB_OP_BRIGHTNESS_COMMIT&&result==UB_OK){s.brightness_phase=UB_BRIGHTNESS_COMMIT;s.brightness_commit_seq=r.seq;s.brightness_commit_epoch=r.epoch;s.brightness_commit_percent=pct;s.brightness_commit_level=raw;}
 strcpy(s.message,result==UB_OK?"ready":"Native brightness request failed");put(status_path,&s,sizeof s);if(!deferred_read)read_status();
}
static struct ub_request begin(unsigned pct,unsigned raw){brightness_begin(0);struct ub_request r=queued();assert(r.op==UB_OP_BRIGHTNESS_BEGIN&&r.brightness.edit==0&&r.brightness.owner_pid==(uint32_t)getpid());assert(r.brightness.owner_start_lo|r.brightness.owner_start_hi);response(r,UB_OK,pct,raw);return r;}
static void save_ack(int result){struct ub_brightness_save_ack a=brightness.save;a.result=result;put(save_path,&a,sizeof a);brightness_pump();render();}
int main(void){
 char dir[]="/tmp/ub820-brightness-ui-XXXXXX",label_text[160];unsigned cases=0;struct ub_request r,p;
 assert(mkdtemp(dir));snprintf(request_path,sizeof request_path,"%s/request",dir);snprintf(status_path,sizeof status_path,"%s/status",dir);snprintf(save_path,sizeof save_path,"%s/save",dir);snprintf(keys_path,sizeof keys_path,"%s/keys",dir);snprintf(abort_path,sizeof abort_path,"%s/abort",dir);
 reset(100,255);assert(total_rows()==5&&!strcmp(row_text(2,label_text,sizeof label_text),"Text brightness: Original (100%)")&&!strcmp(row_text(3,label_text,sizeof label_text),"Reset text brightness"));cases++;
 assert(!mkfifo(status_path,0600));read_status();assert(m.status.brightness_percent==100&&!m.pending);assert(!unlink(status_path));cases++;
 reset(80,204);choose_row();r=queued();assert(brightness.active&&brightness.op==UB_OP_BRIGHTNESS_BEGIN);
 assert(widget(m.list)->w==340&&widget(m.list)->h==124&&widget(m.list)->y+124<=PAGE_Y+PAGE_H&&widget(m.drop[2])->min==0&&widget(m.drop[2])->max==100&&widget(m.drop[2])->indicator==80);cases++;
 brightness_key(KEY_LEFT);brightness_key(KEY_UP);brightness_key(KEY_UP);assert(brightness.percent==99&&queued().seq==r.seq);brightness_key(KEY_RIGHT);assert(brightness.percent==100&&queued().seq==r.seq);cases++;
 response(r,UB_OK,80,204);p=queued();assert(p.op==UB_OP_BRIGHTNESS_PREVIEW&&p.brightness.value==100&&m.status.brightness_percent==80);cases++;
 brightness_key(KEY_LEFT);brightness_key(KEY_DOWN);assert(brightness.percent==89&&queued().seq==p.seq);response(p,UB_OK,100,255);r=queued();assert(r.op==UB_OP_BRIGHTNESS_PREVIEW&&r.brightness.value==89&&r.seq!=p.seq);cases++;
 response(r,UB_OK,89,ub_brightness_level(89));assert(!m.pending&&!brightness.op&&m.status.brightness_percent==89);
 brightness_key(KEY_OK);r=queued();assert(r.op==UB_OP_BRIGHTNESS_COMMIT&&r.brightness.value==89);response(r,UB_OK,89,ub_brightness_level(89));assert(brightness.saving&&brightness.active);cases++;
 struct ub_brightness_save_ack wrong=brightness.save;wrong.seq++;put(save_path,&wrong,sizeof wrong);brightness_pump();assert(brightness.saving);wrong=brightness.save;wrong.token[0]^=1;put(save_path,&wrong,sizeof wrong);brightness_pump();assert(brightness.saving);cases++;
 assert(!unlink(save_path)&&!mkfifo(save_path,0600));brightness_pump();assert(brightness.saving);assert(!unlink(save_path));put(save_path,&brightness.save,sizeof brightness.save);
 assert(!chmod(save_path,0644));brightness_pump();assert(brightness.saving);assert(!chmod(save_path,0600));cases++;
 save_ack(UB_BRIGHTNESS_SAVED);assert(!brightness.active&&m.drop_kind==DROP_NONE&&strstr(m.notice,"saved"));cases++;
 r=queued();brightness_key(KEY_RIGHT);brightness_key(KEY_OK);assert(!brightness.active&&queued().seq==r.seq);cases++;
 /* The backend can consume SaveACK before the first UI COMMIT observation. */
 reset(80,204);begin(80,204);brightness_key(KEY_OK);r=queued();deferred_read=1;response(r,UB_OK,80,204);
 struct ub_status early;FILE *f=fopen(status_path,"rb");assert(f&&fread(&early,1,sizeof early,f)==sizeof early&&!fclose(f));early.brightness_edit=0;early.brightness_phase=UB_BRIGHTNESS_NONE;put(status_path,&early,sizeof early);
 struct ub_brightness_save_ack done=brightness.save;done.result=UB_BRIGHTNESS_SAVED;put(save_path,&done,sizeof done);read_status();assert(!brightness.active&&!m.pending&&strstr(m.notice,"saved"));cases++;
 /* An accepted commit survives a media epoch change of the SAME publisher. */
 reset(80,204);begin(80,204);brightness_key(KEY_OK);r=queued();deferred_read=1;response(r,UB_OK,80,204);
 f=fopen(status_path,"rb");assert(f&&fread(&early,1,sizeof early,f)==sizeof early&&!fclose(f));early.epoch++;early.brightness_edit=0;early.brightness_phase=UB_BRIGHTNESS_NONE;put(status_path,&early,sizeof early);read_status();
 assert(brightness.active&&brightness.saving&&brightness.epoch==r.epoch&&m.epoch!=r.epoch&&m.needs_list);save_ack(UB_BRIGHTNESS_SAVED);assert(!brightness.active&&m.needs_list&&strstr(m.notice,"saved")&&queued().seq==r.seq);cases++;
 /* The same old receipt/ACK does not authorize a DIFFERENT payload instance. */
 reset(80,204);begin(80,204);brightness_key(KEY_OK);r=queued();deferred_read=1;response(r,UB_OK,80,204);done=brightness.save;done.result=UB_BRIGHTNESS_SAVED;put(save_path,&done,sizeof done);
 f=fopen(status_path,"rb");assert(f&&fread(&early,1,sizeof early,f)==sizeof early&&!fclose(f));early.epoch++;early.brightness_token[0]^=1;put(status_path,&early,sizeof early);read_status();assert(!brightness.active&&m.needs_list&&!strstr(m.notice,"saved")&&queued().seq==r.seq);cases++;
 /* Read-only opening does not normalize a legacy nonrepresentable raw80. */
 reset(31,80);begin(31,80);r=queued();assert(r.op==UB_OP_BRIGHTNESS_BEGIN&&!m.pending);brightness_key(KEY_BACK);r=queued();assert(r.op==UB_OP_BRIGHTNESS_CANCEL&&r.brightness.value==0);response(r,UB_OK,31,80);assert(!brightness.active&&m.status.text_level==80);cases++;
 /* Explicit OK does normalize the chosen31% before its persisted commit. */
 reset(31,80);begin(31,80);brightness_key(KEY_OK);r=queued();assert(r.op==UB_OP_BRIGHTNESS_PREVIEW&&r.brightness.value==31);response(r,UB_OK,31,79);r=queued();assert(r.op==UB_OP_BRIGHTNESS_COMMIT);response(r,UB_OK,31,79);assert(brightness.save.level==79);save_ack(UB_BRIGHTNESS_SAVED);cases++;
 reset(0,0);begin(0,0);brightness_key(KEY_LEFT);brightness_key(KEY_DOWN);assert(!brightness.percent&&!m.pending);brightness_key(KEY_UP);r=queued();assert(r.brightness.value==10);for(unsigned i=0;i<20;i++)brightness_key(KEY_UP);assert(brightness.percent==100&&queued().seq==r.seq);brightness_key(KEY_RIGHT);assert(brightness.percent==100);cases++;
 brightness_key(KEY_BACK);response(r,UB_OK,10,ub_brightness_level(10));r=queued();assert(r.op==UB_OP_BRIGHTNESS_CANCEL);response(r,UB_OK,0,0);assert(!brightness.active);cases++;
 /* A destroyed page retains BEGIN and cancels it through the existing timer. */
 reset(50,128);brightness_begin(0);r=queued();unsigned allocated=used;close_child(0);assert(!m.active&&brightness.active&&brightness.cancel&&m.pending==r.seq);
 deferred_read=1;response(r,UB_OK,50,128);m.ticks=POLL_TICKS-1;assert(timer_tick(0)==1);r=queued();assert(r.op==UB_OP_BRIGHTNESS_CANCEL&&!m.active&&used==allocated);
 response(r,UB_OK,50,128);m.ticks=POLL_TICKS-1;assert(timer_tick(0)==1);assert(!brightness.active&&!m.pending&&!m.active);cases++;
 reset(50,128);begin(50,128);brightness_key(KEY_RIGHT);r=queued();struct ub_status stale=m.status;stale.seq=r.seq-1;put(status_path,&stale,sizeof stale);read_status();assert(m.pending==r.seq&&brightness.op==UB_OP_BRIGHTNESS_PREVIEW);cases++;
 /* A matching sequence with the wrong applied value is not an ACK. */
 response(r,UB_OK,52,ub_brightness_level(52));assert(m.pending==r.seq&&brightness.op==UB_OP_BRIGHTNESS_PREVIEW&&queued().seq==r.seq);cases++;
 /* Epoch changes close the transaction; an old acknowledgement cannot save. */
 stale=m.status;stale.epoch++;put(status_path,&stale,sizeof stale);read_status();assert(!brightness.active&&!m.pending&&m.needs_list&&m.drop_kind==DROP_NONE&&queued().seq==r.seq);cases++;
 /* A transient preview failure must not swallow an already requested Cancel. */
 reset(50,128);begin(50,128);brightness_key(KEY_RIGHT);r=queued();brightness_key(KEY_BACK);response(r,UB_PARSE_ERROR,50,128);r=queued();assert(r.op==UB_OP_BRIGHTNESS_CANCEL);response(r,UB_OK,50,128);assert(!brightness.active);cases++;
 reset(20,51);brightness_begin(1);r=queued();response(r,UB_OK,20,51);r=queued();assert(r.op==UB_OP_BRIGHTNESS_PREVIEW&&r.brightness.value==100);response(r,UB_OK,100,255);r=queued();assert(r.op==UB_OP_BRIGHTNESS_COMMIT);response(r,UB_OK,100,255);save_ack(UB_BRIGHTNESS_SAVE_ERROR);assert(!brightness.active&&strstr(m.notice,"not saved")&&m.status.brightness_percent==100);cases++;
 /* Timeout publishes a compensating Cancel, never a preview/commit resend. */
 reset(50,128);brightness_begin(0);r=queued();seconds+=12;assert(subtitle_pending_expire());brightness_pump();p=queued();assert(p.op==UB_OP_BRIGHTNESS_CANCEL&&p.seq!=r.seq);
 response(r,UB_OK,50,128);assert(brightness.active&&m.pending==p.seq);response(p,UB_OK,50,128);assert(!brightness.active);cases++;
 reset(50,128);begin(50,128);brightness_key(KEY_BACK);r=queued();seconds+=12;assert(subtitle_pending_expire()&&brightness.active&&brightness.op==UB_OP_BRIGHTNESS_CANCEL);brightness_pump();assert(queued().seq==r.seq);response(r,UB_OK,50,128);assert(!brightness.active);cases++;
 reset(50,128);begin(50,128);brightness_key(KEY_BACK);r=queued();response(r,UB_PARSE_ERROR,50,128);assert(brightness.active&&!brightness.op&&!brightness.cancel);brightness_pump();assert(queued().seq==r.seq);brightness_key(KEY_BACK);p=queued();assert(p.op==UB_OP_BRIGHTNESS_CANCEL&&p.seq!=r.seq);response(p,UB_OK,50,128);assert(!brightness.active);cases++;
 /* Missing persistence transport cannot hold all subtitle controls forever. */
 reset(80,204);begin(80,204);brightness_key(KEY_OK);r=queued();response(r,UB_OK,80,204);done=brightness.save;brightness_key(KEY_BACK);assert(brightness.saving&&m.drop_kind==DROP_NONE);
 seconds+=BRIGHTNESS_SAVE_WAIT-1;brightness_pump();assert(brightness.saving);seconds++;brightness_pump();assert(!brightness.active&&!m.pending&&strstr(m.notice,"unconfirmed"));
 assert(!send_request(UB_OP_LIST,"/mnt/usb"));p=queued();assert(p.op==UB_OP_LIST&&p.seq!=r.seq);done.result=UB_BRIGHTNESS_SAVED;put(save_path,&done,sizeof done);brightness_pump();assert(queued().seq==p.seq&&m.pending==p.seq&&!brightness.active);cases++;
 close_child(0);unlink(request_path);unlink(status_path);unlink(save_path);unlink(keys_path);unlink(abort_path);assert(!rmdir(dir));
 printf("subtitle_brightness_ui_test passed (%u scenarios)\n",cases);return 0;
}
