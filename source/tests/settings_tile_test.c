/* Real Settings hooks; stock UI factories and timer are controlled fixtures.
 * Build without PIE so the firmware's 32-bit view slots can hold fixture data. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../modules/ui/settings.c"

static int view[64],timer_result=1,timer_calls,render_calls,make_calls,fail_make,disabled;
static uint32_t rec[0x50],panel[0x30],system_keys[25],main_keys[25],child_keys[25];
static void *active_keys;
static Widget buttons[R_MAX];
static const char *names[R_MAX]={"b0","b1","b2","b3","b4","b5","b6","b7","b8","b9","b10"};
static const char *render_names[R_MAX],*values[R_MAX];
static char last_label[64];
static int builder_id,stock_ok_calls,tick_replace_record,tick_shutdown;
static uint32_t replacement[0x50];

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define VOID(name,args) void name args {}
#define TYPE(name) GType name(void){return 1;}
#define NEW(name) Widget name(void){return 0;}
NEW(CTK_window_new) NEW(CTK_fixed_new)
Widget CTK_label_new(const char *text){return 0;}
Widget CTK_stock_button_new(int style,const char *text){return 0;}
TYPE(CTK_window_get_type) TYPE(CTK_fixed_get_type) TYPE(CTK_button_get_type)
TYPE(gtk_container_get_type) TYPE(gtk_fixed_get_type) TYPE(gtk_label_get_type)
Widget gtk_type_check_object_cast(Widget w,GType type){return w;}
VOID(gtk_widget_set_name,(Widget w,const char *name))
void gtk_widget_show(Widget w){assert(w!=(Widget)0x80);}
VOID(gtk_widget_hide,(Widget w))
VOID(gtk_widget_set_usize,(Widget w,int width,int height)) VOID(gtk_widget_set_uposition,(Widget w,int x,int y))
VOID(gtk_widget_set_state,(Widget w,int state)) VOID(gtk_widget_set_sensitive,(Widget w,int enabled)) VOID(gtk_widget_destroy,(Widget w))
VOID(gtk_container_add,(Widget w,Widget child)) VOID(gtk_fixed_put,(Widget w,Widget child,int x,int y))
VOID(gtk_label_set_text,(Widget w,const char *text)) VOID(gtk_label_set_justify,(Widget w,int alignment))
VOID(CTK_window_set_layer,(Widget w,int layer)) VOID(CTK_fixed_set_shape,(Widget w,int shape)) VOID(CTK_fixed_set_alpha,(Widget w,int alpha))
void CTK_button_set_text(Widget w,const char *text){snprintf(last_label,sizeof last_label,"%s",text);}
VOID(CTK_button_set_shape,(Widget w,int shape))
void CTK_button_set_state_disable(Widget w,int state){disabled=state;}
unsigned int g_timeout_add(unsigned int interval,int (*fn)(void *),void *arg){return 1;}
void *gtk_object_get_data(Widget window,const char *name){
 unsigned i;assert(window==(Widget)0x80);
 if(!strcmp(name,"Std_first_fixed_second"))return (Widget)0x90;
 for(i=0;i<R_MAX;i++)if(!strcmp(name,names[i]))return buttons[i];
 return 0;
}
#pragma GCC diagnostic pop

static int stock_ok_stub(void){stock_ok_calls++;return 23;}
static void build_record(uint32_t *r,uint32_t *p,int id){
 builder_id=id;if(!r||!p)return;
 memset(r,0,sizeof rec);r[0]=0x3e9;r[R_COUNT]=2;
 r[R_IDS]=id==SYSTEM_CATEGORY?0x1391:0x13bc;r[R_IDS+1]=id==SYSTEM_CATEGORY?0x1396:0x13ca;
 r[R_FLAGS]=r[R_FLAGS+1]=1;p[0x1f]=r[R_COUNT];
}
static void make_button(Widget window,Widget fixed,int row){
 assert(window==(Widget)0x80&&fixed==(Widget)0x90);assert(row>=0&&(unsigned)row<R_MAX);
 make_calls++;if(!fail_make)buttons[row]=(Widget)(uintptr_t)(0x1000+row);
}
static void render_original(int *v,void *a,void *b,void *c){
 unsigned i;uint32_t *r=(uint32_t *)(uintptr_t)v[V_RECORD];(void)a;(void)b;(void)c;render_calls++;
 for(i=0;i<r[R_COUNT]&&i<R_MAX;i++)v[V_RIGHT+i]=(int)(uintptr_t)buttons[i];
}
static int tick_original(void *app){
 (void)app;timer_calls++;
 if(tick_shutdown){((int *)app)[3]=1;ub_view=(int *)1;}
 if(tick_replace_record){view[V_RECORD]=(int)(uintptr_t)replacement;tick_replace_record=0;}
 return timer_result;
}
static void entry_original(int id){(void)id;active_keys=system_keys;}
static void reset(void){
 memset(view,0,sizeof view);memset(panel,0,sizeof panel);memset(buttons,0,sizeof buttons);
 memset(render_names,0,sizeof render_names);memset(values,0,sizeof values);memset(&s,0,sizeof s);
 memset(system_keys,0,sizeof system_keys);system_keys[0]=KEY_OK;system_keys[1]=(uint32_t)(uintptr_t)stock_ok_stub;system_keys[5]=0xffffffffu;
 system_keys[6]=0;main_keys[0]=child_keys[0]=0xffffffffu;
 ub_view=view;ub_key_table=&active_keys;ub_system_keys=system_keys;ub_main_keys=main_keys;
 ub_right_count=panel+0x1f;ub_right_names_all=names;ub_right_names=render_names;ub_right_values=values;
 ub_original_build_record=build_record;ub_original_render=render_original;ub_original_ui_tick=tick_original;
 ub_original_entry_table=entry_original;ub_make_right_button=make_button;ub_build_column=0;
 view[V_RECORD]=(int)(uintptr_t)rec;view[V_WINDOW]=0x80;view[0]=0x3e9;view[1]=3;view[2]=1;view[3]=1;
 active_keys=system_keys;selected_id=0;timer_result=1;timer_calls=render_calls=make_calls=fail_make=stock_ok_calls=tick_replace_record=tick_shutdown=0;
 disabled=-1;last_label[0]=0;build_record(rec,panel,SYSTEM_CATEGORY);
 buttons[0]=(Widget)0x1000;buttons[1]=(Widget)0x1001;
}
int main(void){
 unsigned i;int saved_focus[3];uint32_t before[0x50];
 reset();ub_hook_build_record(rec,panel,SYSTEM_CATEGORY);
 assert(builder_id==SYSTEM_CATEGORY&&rec[R_COUNT]==3&&panel[0x1f]==3&&tile_index(rec)==2&&make_calls==0);
 /* The native window factory sees the appended row and creates it itself. */
 buttons[2]=(Widget)0x1002;ub_hook_render(view,0,0,0);
 assert(make_calls==0&&!strcmp(last_label,"UB820 Helper"));
 for(i=0;i<20;i++)assert(ub_hook_ui_tick(0)==1);
 assert(rec[R_COUNT]==3&&make_calls==0&&render_calls==1&&active_keys==tile_keys);
 ub_hook_build_record(rec,panel,TILE_ID);assert(builder_id==SYSTEM_CATEGORY&&rec[R_COUNT]==3);
 ub_hook_build_record(rec,panel,0x13bc);assert(rec[R_COUNT]==2&&tile_index(rec)<0);

 reset();memcpy(saved_focus,view+1,sizeof saved_focus);
 assert(ub_hook_ui_tick(0)==1&&make_calls==1&&rec[R_COUNT]==3&&panel[0x1f]==3);
 assert(!memcmp(saved_focus,view+1,sizeof saved_focus)&&active_keys==tile_keys&&!strcmp(last_label,"UB820 Helper")&&disabled==0);
 assert(system_ok()==23&&stock_ok_calls==1); /* stock focused item remains stock */
 for(i=0;i<20;i++)ub_hook_ui_tick(0);
 assert(make_calls==1&&render_calls==1&&rec[R_COUNT]==3);

 reset();fail_make=1;ub_hook_ui_tick(0);assert(rec[R_COUNT]==2&&panel[0x1f]==2&&make_calls==1);
 fail_make=0;ub_hook_ui_tick(0);assert(rec[R_COUNT]==3&&make_calls==2);
 reset();ub_hook_build_record(rec,panel,SYSTEM_CATEGORY);fail_make=1;
 ub_hook_ui_tick(0);assert(rec[R_COUNT]==2&&panel[0x1f]==2);
 fail_make=0;ub_hook_ui_tick(0);assert(rec[R_COUNT]==3&&buttons[2]);
 reset();ub_hook_build_record(rec,panel,SYSTEM_CATEGORY);view[V_RIGHT_SELECTED]=2;
 view[V_RIGHT+2]=0x1234;fail_make=1;ub_hook_ui_tick(0);
 assert(rec[R_COUNT]==2&&view[V_RIGHT_SELECTED]==1&&view[V_RIGHT+2]==0);
 fail_make=0;ub_hook_ui_tick(0);assert(rec[R_COUNT]==3&&view[V_RIGHT_SELECTED]==1);

 reset();view[3]=0;active_keys=main_keys;ub_hook_ui_tick(0);
 assert(disabled==1&&active_keys==main_keys&&view[3]==0);
 reset();active_keys=child_keys;memcpy(before,rec,sizeof rec);ub_hook_ui_tick(0);
 assert(!memcmp(before,rec,sizeof rec)&&make_calls==0&&active_keys==child_keys);
 reset();s.open=1;ub_hook_ui_tick(0);assert(rec[R_COUNT]==2&&make_calls==0&&active_keys==system_keys);
 reset();view[V_WINDOW]=0;ub_hook_ui_tick(0);assert(rec[R_COUNT]==2&&make_calls==0);
 reset();ub_view=0;ub_hook_ui_tick(0);assert(make_calls==0);
 reset();timer_result=0;ub_view=(int *)1;assert(!ub_hook_ui_tick(0)&&make_calls==0); /* no dangling access after stock shutdown */
 reset();{int app[4]={0};tick_shutdown=1;assert(ub_hook_ui_tick(app)==1&&app[3]==1&&make_calls==0);}
 reset();memcpy(replacement,rec,sizeof rec);view[V_RECORD]=1;tick_replace_record=1;
 ub_hook_ui_tick(0);assert(replacement[R_COUNT]==3&&make_calls==1); /* inspect post-stock view */
 reset();build_record(rec,panel,0x13bc);ub_hook_ui_tick(0);assert(rec[R_COUNT]==2&&make_calls==0);
 reset();rec[R_COUNT]=R_MAX;panel[0x1f]=R_MAX;memcpy(before,rec,sizeof rec);
 ub_hook_ui_tick(0);assert(!memcmp(before,rec,sizeof rec)&&make_calls==0&&panel[0x1f]==R_MAX);
 reset();ub_hook_ui_tick(0);memset(buttons,0,sizeof buttons);ub_hook_ui_tick(0);
 assert(rec[R_COUNT]==3&&buttons[2]&&make_calls==2); /* same-address replacement window */
 puts("settings_tile_test passed");return 0;
}
