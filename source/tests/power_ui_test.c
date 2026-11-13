/* Actual Helper power row and IPC, with own temp files and CTK stubs. */
#define _GNU_SOURCE
#ifndef UB_HOST_TEST
#error "Power UI fixtures require UB_HOST_TEST, including when run under QEMU"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "helper_menu.h"
#undef UB_POWER_ENABLED
#undef UB_POWER_REQUEST
#undef UB_POWER_STATUS
static char enabled_path[256],request_path[256],status_path[256];
#define UB_POWER_ENABLED enabled_path
#define UB_POWER_REQUEST request_path
#define UB_POWER_STATUS status_path
static uint32_t seconds=100;
static void test_server_now(struct timespec *out){out->tv_sec=seconds;out->tv_nsec=0;}
#define ub_now test_server_now
#include "action_ipc.h"
#undef ub_now
static int test_clock(clockid_t id,struct timespec *out){assert(id==CLOCK_MONOTONIC);out->tv_sec=seconds;out->tv_nsec=0;return 0;}
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
NEW(CTK_plotbar_new) TYPE(CTK_plotbar_get_type)
VOID(CTK_plotbar_set_min_value,(Widget w,int value)) VOID(CTK_plotbar_set_max_value,(Widget w,int value)) VOID(CTK_plotbar_set_indicator,(Widget w,int value))
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


static void put(const char *path,const char *text){FILE *f=fopen(path,"w");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static void reply_to(struct helper_state *s,unsigned seq,const char *text){char path[UB_ACTION_PATH];assert(!ub_action_path(path,sizeof path,UB_POWER_REQUEST,'r',s->action_session,seq));assert(!ub_action_reply(path,text,strlen(text)));}
static void cancel(struct helper_state *s){ha_action_cancel(s,UB_POWER_REQUEST,s->power_seq);}
int main(void){
 char directory[]="/tmp/ub820-power-ui-XXXXXX",text[192],reply[UB_ACTION_PATH],label[128];
 unsigned ids[HELPER_ROWS],seq,choice;const char *message;unsigned checks=0;
 struct helper_state a={0},b={0};
 assert(mkdtemp(directory));
 snprintf(enabled_path,sizeof enabled_path,"%s/enabled",directory);
 snprintf(request_path,sizeof request_path,"%s/request",directory);
 snprintf(status_path,sizeof status_path,"%s/status",directory);
 assert(H_REGION==0&&H_DVD==1&&H_NETFLIX==2&&H_SSH==3&&H_LOG==4&&H_SAVE==5);
 hs.dvd_known=hs.netflix_known=hs.ssh_enabled=1;
 assert(helper_rows(ids)==6&&ids[0]==H_REGION&&ids[1]==H_DVD&&ids[2]==H_NETFLIX&&ids[3]==H_SSH&&ids[4]==H_LOG&&ids[5]==H_SAVE);checks++;
 assert(!ha_power_poll(&a)&&!a.power_known&&!a.power_enabled);
 ha_power_set(&a,0);assert(!a.power_wait&&!strcmp(ha_power_value(&a),"not available"));checks++;
 put(enabled_path,"");assert(ha_power_poll(&a)&&a.power_enabled&&!a.power_known);
 hs.power_enabled=1;assert(helper_rows(ids)==7&&ids[3]==H_POWER&&ids[4]==H_SSH&&ids[5]==H_LOG&&ids[6]==H_SAVE);checks++;
 put(status_path,"0 on current\n");assert(ha_power_poll(&a)&&a.power_known&&a.power_choice==1);checks++;
 hs=a;assert(!strcmp(helper_text(H_POWER,label,sizeof label),"Auto power-off: On"));
 m.drop_kind=DROP_POWER;assert(!strcmp(drop_text(0),"On")&&!strcmp(drop_text(1),"Off"));m.drop_kind=DROP_NONE;
 ha_power_set(&a,0);assert(a.power_wait&&a.power_choice==1&&a.power_known);checks++;
 assert(ub_action_receive(UB_POWER_REQUEST,text,sizeof text,reply,sizeof reply));assert(strstr(text,"set off 1\n"));
 put(status_path,"1 off saved\n");ha_power_poll(&a);assert(a.power_wait&&a.power_choice==1);checks++;
 reply_to(&a,a.power_seq,"2 off saved\n");ha_power_poll(&a);assert(a.power_wait&&a.power_choice==1);checks++;
 ha_power_poll(&b);assert(b.power_known&&b.power_choice==0);ha_power_set(&b,1);assert(b.power_wait);
 reply_to(&b,b.power_seq,"1 on saved\n");ha_power_poll(&a);assert(a.power_wait);assert(ha_power_poll(&b)&&!b.power_wait&&b.power_choice==1);checks++;
 reply_to(&a,a.power_seq,"1 off saved\n");assert(ha_power_poll(&a)&&!a.power_wait&&a.power_choice==0&&strstr(a.notice,"Manual POWER"));checks++;
 ha_power_set(&a,1);assert(a.power_wait&&a.power_choice==0);
 snprintf(text,sizeof text,"%u off native policy unavailable\n",a.power_seq);reply_to(&a,a.power_seq,text);
 assert(ha_power_poll(&a)&&!a.power_wait&&a.power_choice==0&&strstr(a.notice,"native policy unavailable"));checks++;
 ha_power_set(&a,1);assert(a.power_wait);seconds+=30;
 assert(ha_power_poll(&a)&&!a.power_wait&&!a.power_known&&a.power_uncertain&&strstr(a.notice,"reopen Helper"));checks++;
 snprintf(text,sizeof text,"%u on saved\n",a.power_seq);reply_to(&a,a.power_seq,text);
 put(status_path,text);ha_power_poll(&a);assert(!a.power_known&&a.power_uncertain);
 ha_power_set(&a,0);assert(!a.power_wait);checks++;
 ha_load(&a);assert(a.power_known&&!a.power_uncertain&&a.power_choice==1&&!a.power_wait);checks++;
 ha_power_set(&a,0);assert(a.power_wait);unlink(enabled_path);
 assert(ha_power_poll(&a)&&!a.power_enabled&&!a.power_known&&!a.power_wait&&strstr(a.notice,"result unknown"));checks++;
 /* Rendering after loss must close the old row's popup before rows compact. */
 hs=a;m.kind=PAGE_HELPER;m.active=1;m.window=(Widget)(uintptr_t)1;m.list=(Widget)(uintptr_t)2;m.list_fixed=(Widget)(uintptr_t)3;m.drop_kind=DROP_POWER;
 render();assert(m.drop_kind==DROP_NONE&&!m.list&&!m.list_fixed);checks++;
 hs.power_enabled=1;hs.power_known=0;m.list=(Widget)(uintptr_t)2;m.list_fixed=(Widget)(uintptr_t)3;m.drop_kind=DROP_POWER;
 render();assert(m.drop_kind==DROP_NONE&&!m.list&&!m.list_fixed);checks++;
 m.active=0;m.window=0;
 assert(!ha_power_status("4294967295 off saved",&seq,&choice,&message)&&seq==UINT32_MAX&&!choice);
 assert(ha_power_status("4294967296 off saved",&seq,&choice,&message));
 assert(ha_power_status("-1 off saved",&seq,&choice,&message));
 assert(ha_power_status("1 offline saved",&seq,&choice,&message));
 assert(ha_power_status("1 on",&seq,&choice,&message));checks++;
 cancel(&a);cancel(&b);seconds+=100;
 while(ub_action_receive(UB_POWER_REQUEST,text,sizeof text,reply,sizeof reply)){}
 unlink(status_path);assert(!rmdir(directory));
 printf("power_ui_test passed (%u scenarios)\n",checks);return 0;
}
