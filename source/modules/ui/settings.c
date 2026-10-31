/* UB820 Player Settings extension (apl_setting). A freestanding PIC DSO
 * installed into apl_setting each time Player Settings opens; the hooks run on
 * its UI thread. Loader bindings, originals and the 1.85 addresses are in
 * settings_hooks.json (1.82 addresses below).
 *
 * System gets one more tile, "UB820 Helper", appended to its right column by
 * the hook on the main-view renderer. OK on it opens our submenu (a CTK
 * window over the right area): Blu-ray and DVD region, Netflix button, SSH,
 * Helper log, Save log to USB, the same actions as OPTIONS > UB820 Helper
 * (helper_actions.h).
 *
 * How apl_setting builds the screen:
 * - apl_setting is a separate CTK program that apl_mgr starts when Player
 *   Settings opens and ends when it is left, so this payload is installed on
 *   every opening.
 * - The top level is built at run time: APL_SET_Initial_menu_View
 *   (FUN_0009ad24) calls FUN_000d7b80 to fill the record at 0x159ab8. Left
 *   column (categories): count in word 4, ids 5..16, flags 17..28, text IDs
 *   29..40. Right column (items of the selected category): count in word
 *   0x29, ids 0x2a.., flags 0x36.., text IDs 0x42.., filled by
 *   FUN_000d78ec(record, id), which rebuilds the System column (0x1389) for
 *   System and for each System item id; an unknown id builds nothing.
 * - Only the category ids are the left column (focus layer 0); any other id
 *   is a right-column row (layer 1), so a new id works there.
 * - The main window (FUN_000bf070) makes one button per right row the record
 *   has when it is built (FUN_000bea0c(window, fixed, row), names s_1st_btn
 *   .. s_11th_btn); the renderer (FUN_00096900) finds them by name, keeps them
 *   in the view (0x1692e4, words 0x20+row) and labels them.
 * - Keys: the word at 0x16910c points at the active key table, records of
 *   five u32 {key, press, repeat, release, 0} ending 0xffffffff; handlers are
 *   int fn(void), 1 = handled. FUN_0009a9dc picks it when an entry is opened;
 *   Up/Down inside a column only move the focus (view word 2 = focused right
 *   row). Key codes: 0x85 up, 0x86 down, 0x82 OK, 0x81 back.
 */
#include <stddef.h>
#include <stdint.h>
#include "helper_actions.h"

typedef void *Widget;
typedef unsigned int GType;
extern Widget CTK_window_new(void),CTK_fixed_new(void),CTK_label_new(const char *);
extern Widget CTK_stock_button_new(int,const char *);
extern GType CTK_window_get_type(void),CTK_fixed_get_type(void),CTK_button_get_type(void);
extern GType gtk_container_get_type(void),gtk_fixed_get_type(void),gtk_label_get_type(void);
extern Widget gtk_type_check_object_cast(Widget,GType);
extern void gtk_widget_set_name(Widget,const char *),gtk_widget_show(Widget),gtk_widget_hide(Widget);
extern void gtk_widget_set_usize(Widget,int,int),gtk_widget_set_uposition(Widget,int,int);
extern void gtk_widget_set_state(Widget,int),gtk_widget_set_sensitive(Widget,int),gtk_widget_destroy(Widget);
extern void gtk_container_add(Widget,Widget),gtk_fixed_put(Widget,Widget,int,int);
extern void gtk_label_set_text(Widget,const char *),gtk_label_set_justify(Widget,int);
extern void CTK_window_set_layer(Widget,int),CTK_fixed_set_shape(Widget,int),CTK_fixed_set_alpha(Widget,int);
extern void CTK_button_set_text(Widget,const char *),CTK_button_set_shape(Widget,int);
extern unsigned int g_timeout_add(unsigned int,int (*)(void *),void *);
extern void *gtk_object_get_data(Widget,const char *);

/* Loader fills these before any branch is installed. */
void (*ub_original_render)(int *,void *,void *,void *);
void (*ub_original_entry_table)(int);
void (*ub_build_column)(uint32_t *,int);
int *ub_view;
void **ub_key_table;
uint32_t *ub_right_count;
uint32_t *ub_system_keys;
void (*ub_make_right_button)(Widget,Widget,int);
const char **ub_right_names_all;
const char **ub_right_names;
const char **ub_right_values;

#define TILE_ID 0x7531u          /* unused by apl_setting 1.82 */
#define TILE_TEXT 0xd7fu         /* placeholder text ID, replaced by our label */
#define SYSTEM_CATEGORY 0x1389
/* Record words (run-time record of the main view, FUN_000d7b80). */
#define R_COUNT 0x29
#define R_IDS 0x2a
#define R_FLAGS 0x36
#define R_TEXTS 0x42
#define R_MAX 11u               /* s_1st_btn .. s_11th_btn */
/* View words: 2 selected right row, 4 record, 5 window, 0x20+i right-column
 * buttons. */
#define V_RIGHT_SELECTED 2
#define V_RECORD 4
#define V_WINDOW 5
#define V_RIGHT 0x20
#define KEY_BACK 0x81u
#define KEY_OK 0x82u
#define KEY_UP 0x85u
#define KEY_DOWN 0x86u
#define KEY_LEFT 0x87u
#define KEY_RIGHT 0x88u
#define KEY_UP2 0x161u
#define KEY_DOWN2 0x160u
#define LOG_FILE "/tmp/ub820_submenu.log"

static Widget cast(Widget w,GType type){return gtk_type_check_object_cast(w,type);}
/* One line into the helper's log (it appends with one write per line). */
static void plog(const char *text){
 char line[160];int fd,n=snprintf(line,sizeof line,"settings: %s\n",text);
 if(n<=0||n>=(int)sizeof line)return;
 fd=open(LOG_FILE,O_WRONLY|O_APPEND);if(fd<0)return;
 if(write(fd,line,(size_t)n)<0){}
 close(fd);
}

/* ---- The tile ----------------------------------------------------------- */
static int selected_id,logged_room;
/* The right column is System's when it holds one of its items. */
static int system_column(const uint32_t *rec){
 unsigned int i,n=rec[R_COUNT];
 for(i=0;i<n&&i<R_MAX;i++){uint32_t id=rec[R_IDS+i];if(id==0x1391||id==0x1396||id==0x142d||id==0x1448)return 1;}
 return 0;
}
static int tile_index(const uint32_t *rec){
 unsigned int i,n=rec[R_COUNT];
 for(i=0;i<n&&i<R_MAX;i++)if(rec[R_IDS+i]==TILE_ID)return (int)i;
 return -1;
}
/* The window (FUN_000bf070) makes one button per right row the record had
 * when it was built, before this hook adds our row, so ours is made here the
 * same way. Without it the renderer finds no widget for the row: no tile, and
 * Down onto it dereferences NULL (FUN_00098xxx). */
static const char *make_tile_button(const int *view,unsigned int at){
 Widget window=(Widget)(uintptr_t)view[V_WINDOW],fixed;const char *name;
 if(!window)return "no window";
 if(!ub_make_right_button||!ub_right_names_all||!ub_right_names||at>=R_MAX)return "no binding";
 name=ub_right_names_all[at];if(!name)return "no name";
 ub_right_names[at]=name;if(ub_right_values)ub_right_values[at]=0;
 if(gtk_object_get_data(window,name))return "found";
 fixed=(Widget)gtk_object_get_data(window,"Std_first_fixed_second");if(!fixed)return "no fixed";
 ub_make_right_button(window,fixed,(int)at);
 return gtk_object_get_data(window,name)?"made":"not made";
}
/* One line per change of window, button or outcome (the log stays short). */
static void log_tile(const int *view,int at,Widget button,const char *how){
 static int last_window,last_at=-2;static Widget last_button;static const char *last_how;char t[128];
 if(view[V_WINDOW]==last_window&&at==last_at&&button==last_button&&how==last_how)return;
 last_window=view[V_WINDOW];last_at=at;last_button=button;last_how=how;
 snprintf(t,sizeof t,"tile row %d: window %08x button %08x (%s)",at+1,(unsigned int)view[V_WINDOW],(unsigned int)(uintptr_t)button,how);plog(t);
}
void ub_hook_render(int *view,void *a,void *b,void *c){
 uint32_t *rec=0;int at=-1;const char *how="";
 if(view&&view==ub_view&&view[V_RECORD])rec=(uint32_t *)(uintptr_t)view[V_RECORD];
 if(rec){
  /* our tile is selected: the stock code built no column for its id */
  if(selected_id==(int)TILE_ID&&!rec[R_COUNT]&&ub_build_column)ub_build_column(rec,SYSTEM_CATEGORY);
  if(system_column(rec)){
   unsigned int n=rec[R_COUNT];
   at=tile_index(rec);
   if(at<0&&n<R_MAX){
    rec[R_IDS+n]=TILE_ID;rec[R_FLAGS+n]=1;rec[R_TEXTS+n]=TILE_TEXT;rec[R_COUNT]=n+1;
    if(ub_right_count)*ub_right_count=n+1;
    at=(int)n;
    if(!logged_room){char t[64];logged_room=1;snprintf(t,sizeof t,"UB820 Helper tile added to System (row %d)",at+1);plog(t);}
   }else if(at<0&&!logged_room){logged_room=1;plog("System has no room for the UB820 Helper tile");}
   if(at>=0)how=make_tile_button(view,(unsigned int)at);
  }
 }
 ub_original_render(view,a,b,c);
 if(rec&&at>=0){
  Widget button=(Widget)(uintptr_t)view[V_RIGHT+at];
  /* the renderer found no widget: make it now and give it to the view */
  if(!button&&view[V_WINDOW]&&ub_right_names_all&&ub_right_names_all[at]){
   how=make_tile_button(view,(unsigned int)at);
   button=(Widget)gtk_object_get_data((Widget)(uintptr_t)view[V_WINDOW],ub_right_names_all[at]);
   view[V_RIGHT+at]=(int)(uintptr_t)button;
  }
  if(button){CTK_button_set_text(cast(button,CTK_button_get_type()),"UB820 Helper");gtk_widget_show(button);}
  log_tile(view,at,button,how);
 }
}

/* ---- Key tables ------------------------------------------------------------ */
struct key_entry { uint32_t key; int (*press)(void); int (*repeat)(void); int (*release)(void); uint32_t zero; };
static struct key_entry tile_keys[10],sub_keys[10];
static int (*stock_ok)(void);
static int tile_ok(void);
static void set_key(struct key_entry *e,uint32_t key,int (*fn)(void)){e->key=key;e->press=fn;e->repeat=fn;e->release=0;e->zero=0;}
/* Whether the focused right row is our tile. */
static int tile_focused(void){
 const uint32_t *rec;unsigned int at;
 if(!ub_view||!ub_view[V_RECORD])return 0;
 rec=(const uint32_t *)(uintptr_t)ub_view[V_RECORD];at=(unsigned int)ub_view[V_RIGHT_SELECTED];
 return at<rec[R_COUNT]&&at<R_MAX&&rec[R_IDS+at]==TILE_ID;
}
/* OK in the System column: our tile opens the submenu, the others keep the
 * stock handler. */
static int system_ok(void){
 if(tile_focused())return tile_ok();
 return stock_ok?stock_ok():0;
}
/* The System items' table with OK replaced by system_ok. Up/Down only move
 * the focus inside the column (FUN_00098a08), so the table set when the
 * column was entered stays active on our row too. */
static void build_tile_keys(void){
 unsigned int n=0;const uint32_t *s=ub_system_keys;
 if(s)for(;n<8&&s[0]!=0xffffffffu;s+=5){
  if(s[0]==KEY_OK){stock_ok=(int (*)(void))(uintptr_t)s[1];set_key(&tile_keys[n++],KEY_OK,system_ok);}
  else{tile_keys[n].key=s[0];tile_keys[n].press=(int (*)(void))(uintptr_t)s[1];tile_keys[n].repeat=(int (*)(void))(uintptr_t)s[2];tile_keys[n].release=(int (*)(void))(uintptr_t)s[3];tile_keys[n].zero=0;n++;}
 }
 tile_keys[n].key=0xffffffffu;
}
void ub_hook_entry_table(int id){
 selected_id=id;
 ub_original_entry_table(id);
 if(ub_key_table&&(id==(int)TILE_ID||(ub_system_keys&&*ub_key_table==(void *)ub_system_keys))){build_tile_keys();*ub_key_table=tile_keys;}
}

/* ---- Submenu ---------------------------------------------------------------- */
#define ROWS 9u
#define LOG_BYTES 8192u
#define LOG_LINES 120u
enum { S_REGION, S_DVD, S_NETFLIX, S_SSH, S_LOG, S_SAVE };
enum { LIST_NONE, LIST_REGION, LIST_NETFLIX, LIST_DVD };
static struct helper_state hs;
#define DROP_ROWS 11u
/* The submenu window (std_main geometry) and the value list beside it. */
#define SUB_X 0x132
#define SUB_Y 0x28
#define SUB_W 0x249
#define SUB_H 0x1a4
#define LIST_PAD 10
#define LIST_ROW_W 250
#define LIST_W (LIST_ROW_W+2*LIST_PAD)
/* listing: which value list (LIST_*) is open in its own framed window beside
 * its row (OK takes the highlighted value, RETURN keeps the old one) */
static struct { Widget window,fixed,title,footer,rows[ROWS],values[ROWS],list,list_fixed,drop[DROP_ROWS]; int open,log_view,timer,listing; unsigned int selected,log_first,log_count,drop_sel; char *log_lines[LOG_LINES]; } s;
static char log_text[LOG_BYTES+1];
static unsigned int sub_rows(unsigned int *ids){unsigned int n=0;ids[n++]=S_REGION;if(hs.dvd_known)ids[n++]=S_DVD;if(hs.netflix_known)ids[n++]=S_NETFLIX;if(hs.ssh_enabled)ids[n++]=S_SSH;ids[n++]=S_LOG;ids[n++]=S_SAVE;return n;}
#define SUB_ROWS_MAX 6u
static unsigned int list_count(void){return s.listing==LIST_NETFLIX?HA_NETFLIX_CHOICES:s.listing==LIST_DVD?HA_DVD_CHOICES:ha_region_count(&hs);}
static const char *list_label(unsigned int k){return s.listing==LIST_NETFLIX?ha_netflix_labels[k]:s.listing==LIST_DVD?ha_dvd_labels[k]:ha_region_labels[k];}
static Widget label(const char *name,int x,int y,int w,int h){
 Widget l=CTK_label_new("");if(!l)return 0;
 gtk_widget_set_name(l,name);gtk_fixed_put(cast(s.fixed,gtk_fixed_get_type()),l,x,y);gtk_widget_set_uposition(l,x,y);gtk_widget_set_usize(l,w,h);gtk_label_set_justify(cast(l,gtk_label_get_type()),0);gtk_widget_show(l);
 return l;
}
static void load_log(void){
 int fd=open(LOG_FILE,O_RDONLY);off_t size,from;ssize_t n=0,got;char *p,*end;
 s.log_count=0;log_text[0]=0;
 if(fd>=0){
  size=lseek(fd,0,SEEK_END);from=size>(off_t)LOG_BYTES?size-(off_t)LOG_BYTES:0;
  if(size>=0&&lseek(fd,from,SEEK_SET)==from)while((size_t)n<LOG_BYTES&&(got=read(fd,log_text+n,LOG_BYTES-(size_t)n))>0)n+=got;
  close(fd);
  log_text[n]=0;p=log_text;end=log_text+n;
  if(from>0)while(p<end&&*p!='\n')p++;
  while(p<end){
   char *line;
   if(*p=='\n'){p++;continue;}
   line=p;while(p<end&&*p!='\n'){if((unsigned char)*p<32||*p==127)*p=' ';p++;}
   if(p<end)*p++=0;
   if(s.log_count==LOG_LINES){memmove(s.log_lines,s.log_lines+1,sizeof s.log_lines-sizeof *s.log_lines);s.log_count--;}
   s.log_lines[s.log_count++]=line;
  }
 }
 s.log_first=s.log_count>ROWS?s.log_count-ROWS:0;
}
static void close_list(void);
static int create_list(void);
static void render_sub(void){
 unsigned int ids[SUB_ROWS_MAX],n,i;char text[200];
 if(!s.open)return;
 if(s.log_view){
  gtk_label_set_text(cast(s.title,gtk_label_get_type()),"Helper log");
  for(i=0;i<ROWS;i++){
   unsigned int k=s.log_first+i;
   if(k<s.log_count){CTK_button_set_text(cast(s.rows[i],CTK_button_get_type()),s.log_lines[k]);gtk_widget_set_usize(s.rows[i],560,30);gtk_widget_set_state(s.rows[i],0);gtk_widget_show(s.rows[i]);}
   else gtk_widget_hide(s.rows[i]);
   gtk_widget_hide(s.values[i]);
  }
  snprintf(text,sizeof text,"%s%u-%u of %u   Up/Down: scroll   RETURN: back",s.log_count?"Lines ":"",s.log_count?s.log_first+1:0,s.log_first+ROWS<s.log_count?s.log_first+ROWS:s.log_count,s.log_count);
  gtk_label_set_text(cast(s.footer,gtk_label_get_type()),s.log_count?text:"The log is empty or cannot be read");
  close_list();
  return;
 }
 gtk_label_set_text(cast(s.title,gtk_label_get_type()),"UB820 Helper");
 n=sub_rows(ids);if(s.selected>=n)s.selected=n-1;
 for(i=0;i<ROWS;i++){
  const char *name=0,*value=0;
  if(i<n){
   switch(ids[i]){
   case S_REGION:name="Blu-ray region";
    value=s.listing==LIST_REGION?"":ha_region_value(&hs,text,sizeof text);  /* the list shows the values */
    break;
   case S_DVD:name="DVD region";
    value=s.listing==LIST_DVD?"":ha_dvd_labels[hs.dvd_choice<HA_DVD_CHOICES?hs.dvd_choice:0];
    break;
   case S_NETFLIX:name="Netflix button";
    value=s.listing==LIST_NETFLIX?"":ha_netflix_labels[hs.netflix_choice<HA_NETFLIX_CHOICES?hs.netflix_choice:0];
    break;
   case S_SSH:name="SSH";value=hs.ssh_running?"On":"Off";break;
   case S_LOG:name="Helper log";value="";break;
   default:name="Save log to USB";value=hs.log_always?"Automatic":"";break;
   }
  }
  if(name){
   CTK_button_set_text(cast(s.rows[i],CTK_button_get_type()),name);gtk_widget_set_usize(s.rows[i],300,30);
   gtk_widget_set_state(s.rows[i],i==s.selected?2:0);gtk_widget_show(s.rows[i]);
   gtk_label_set_text(cast(s.values[i],gtk_label_get_type()),value);gtk_widget_show(s.values[i]);
  }else{gtk_widget_hide(s.rows[i]);gtk_widget_hide(s.values[i]);}
 }
 if(s.listing&&!s.list&&create_list()!=0){close_list();s.listing=LIST_NONE;}
 if(s.listing){
  /* right-aligned to the submenu, the first value level with the row */
  int h=LIST_PAD+(int)list_count()*34+LIST_PAD-4,y=SUB_Y+46+(int)s.selected*34-LIST_PAD;
  if(y+h>SUB_Y+SUB_H)y=SUB_Y+SUB_H-h;
  gtk_widget_set_uposition(s.list,SUB_X+SUB_W-LIST_W,y);gtk_widget_set_usize(s.list,LIST_W,h);gtk_widget_set_usize(s.list_fixed,LIST_W,h);
  for(i=0;i<DROP_ROWS;i++){
   if(i<list_count()){CTK_button_set_text(cast(s.drop[i],CTK_button_get_type()),list_label(i));gtk_widget_set_state(s.drop[i],i==s.drop_sel?2:0);gtk_widget_show(s.drop[i]);}
   else gtk_widget_hide(s.drop[i]);
  }
  gtk_widget_show(s.list);
 }else close_list();
 gtk_label_set_text(cast(s.footer,gtk_label_get_type()),s.listing?"OK: take   RETURN: keep as it was":hs.notice[0]?hs.notice:"OK: change   RETURN: back");
}
static int sub_tick(void *unused){
 int changed;(void)unused;
 if(!s.open){s.timer=0;return 0;}
 changed=ha_region_poll(&hs);changed|=ha_dvd_poll(&hs);changed|=ha_netflix_poll(&hs);changed|=ha_ssh_poll(&hs);changed|=ha_log_poll(&hs);
 if(changed&&!s.log_view)render_sub();
 return 1;
}
static void close_sub(void){
 Widget w=s.window;
 close_list();
 s.open=0;s.log_view=0;s.listing=LIST_NONE;s.window=0;
 if(w)gtk_widget_destroy(w);
 if(ub_key_table)*ub_key_table=tile_keys;
}
static int sub_up(void){if(s.listing){s.drop_sel=s.drop_sel?s.drop_sel-1:list_count()-1;}else if(s.log_view){if(s.log_first)s.log_first--;}else{unsigned int ids[SUB_ROWS_MAX],n=sub_rows(ids);s.selected=s.selected?s.selected-1:n-1;}render_sub();return 1;}
static int sub_down(void){if(s.listing){s.drop_sel=s.drop_sel+1<list_count()?s.drop_sel+1:0;}else if(s.log_view){if(s.log_first+ROWS<s.log_count)s.log_first++;}else{unsigned int ids[SUB_ROWS_MAX],n=sub_rows(ids);s.selected=s.selected+1<n?s.selected+1:0;}render_sub();return 1;}
static int sub_back(void){if(s.listing){s.listing=LIST_NONE;render_sub();}else if(s.log_view){s.log_view=0;render_sub();}else close_sub();return 1;}
static int sub_ok(void){
 unsigned int ids[SUB_ROWS_MAX],n=sub_rows(ids);
 if(s.listing){ /* only a different value is saved */
  if(s.listing==LIST_NETFLIX)ha_netflix_set(&hs,s.drop_sel);
  else if(s.listing==LIST_DVD)ha_dvd_set(&hs,s.drop_sel);
  else ha_region_set(&hs,s.drop_sel);
  s.listing=LIST_NONE;render_sub();return 1;
 }
 if(s.log_view){s.log_view=0;render_sub();return 1;}
 if(s.selected>=n)return 1;
 switch(ids[s.selected]){
 case S_REGION:
  if(!hs.region_known){ha_copy(hs.notice,"Needs the region module ([region] in modules.cfg)",sizeof hs.notice);break;}
  s.listing=LIST_REGION;s.drop_sel=ha_region_index(hs.region_choice);if(s.drop_sel>=ha_region_count(&hs))s.drop_sel=0;break;
 case S_DVD:
  s.listing=LIST_DVD;s.drop_sel=hs.dvd_choice<HA_DVD_CHOICES?hs.dvd_choice:0;break;
 case S_NETFLIX:
  s.listing=LIST_NETFLIX;s.drop_sel=hs.netflix_choice<HA_NETFLIX_CHOICES?hs.netflix_choice:0;break;
 case S_SSH:ha_ssh_toggle(&hs);break;
 case S_LOG:load_log();s.log_view=1;break;
 default:ha_log_save(&hs);break;
 }
 render_sub();return 1;
}
static int sub_left(void){if(s.listing){s.listing=LIST_NONE;render_sub();}return 1;}
static void build_sub_keys(void){
 unsigned int n=0;
 set_key(&sub_keys[n++],KEY_UP,sub_up);set_key(&sub_keys[n++],KEY_UP2,sub_up);
 set_key(&sub_keys[n++],KEY_DOWN,sub_down);set_key(&sub_keys[n++],KEY_DOWN2,sub_down);
 set_key(&sub_keys[n++],KEY_OK,sub_ok);set_key(&sub_keys[n++],KEY_RIGHT,sub_ok);
 set_key(&sub_keys[n++],KEY_BACK,sub_back);set_key(&sub_keys[n++],KEY_LEFT,sub_left);
 sub_keys[n].key=0xffffffffu;
}
/* The value list: a framed window made after the submenu, so it is drawn
 * over it. It is made each time it opens and destroyed when it closes, and
 * its rows keep the default (centred) text: in OPTIONS, a text draw attribute
 * made set text show only on the first opening. */
static void close_list(void){
 Widget w=s.list;s.list=s.list_fixed=0;memset(s.drop,0,sizeof s.drop);
 if(w)gtk_widget_destroy(w);
}
static int create_list(void){
 unsigned int i;char name[64];
 s.list=CTK_window_new();if(!s.list)return -1;
 gtk_widget_set_name(s.list,"UB820_Helper_list_CmnE_18");
 CTK_window_set_layer(cast(s.list,CTK_window_get_type()),3);
 s.list_fixed=CTK_fixed_new();if(!s.list_fixed)return -1;
 gtk_widget_set_name(s.list_fixed,"UB820_Helper_list_fixed_CmnB_18");
 CTK_fixed_set_shape(cast(s.list_fixed,CTK_fixed_get_type()),3);CTK_fixed_set_alpha(cast(s.list_fixed,CTK_fixed_get_type()),255);
 gtk_container_add(cast(s.list,gtk_container_get_type()),s.list_fixed);gtk_widget_show(s.list_fixed);
 for(i=0;i<DROP_ROWS;i++){
  Widget b=CTK_stock_button_new(1,"");if(!b)return -1;
  snprintf(name,sizeof name,"UB820_Helper_drop%u_btn_B_BtnB_18",i);gtk_widget_set_name(b,name);
  gtk_fixed_put(cast(s.list_fixed,gtk_fixed_get_type()),b,LIST_PAD,LIST_PAD+(int)i*34);gtk_widget_set_uposition(b,LIST_PAD,LIST_PAD+(int)i*34);gtk_widget_set_usize(b,LIST_ROW_W,30);
  s.drop[i]=b;
 }
 return 0;
}
/* Over the right area of Player Settings (std_main geometry 0x132,0x28). */
static int tile_ok(void){
 unsigned int i;char name[64];
 if(s.open)return 1;
 ha_load(&hs);s.selected=0;s.log_view=0;s.listing=LIST_NONE;
 s.window=CTK_window_new();if(!s.window)return 1;
 gtk_widget_set_name(s.window,"UB820_Helper_std_CmnE_18");gtk_widget_set_uposition(s.window,SUB_X,SUB_Y);gtk_widget_set_usize(s.window,SUB_W,SUB_H);
 CTK_window_set_layer(cast(s.window,CTK_window_get_type()),3);
 s.fixed=CTK_fixed_new();if(!s.fixed){gtk_widget_destroy(s.window);s.window=0;return 1;}
 gtk_widget_set_name(s.fixed,"UB820_Helper_fixed_CmnB_18");gtk_widget_set_usize(s.fixed,SUB_W,SUB_H);
 CTK_fixed_set_shape(cast(s.fixed,CTK_fixed_get_type()),3);CTK_fixed_set_alpha(cast(s.fixed,CTK_fixed_get_type()),255);
 gtk_container_add(cast(s.window,gtk_container_get_type()),s.fixed);gtk_widget_show(s.fixed);
 s.open=1; /* label() places into s.fixed */
 s.title=label("UB820_Helper_head_lbl_CmnB_18",12,8,560,32);
 for(i=0;i<ROWS;i++){
  Widget b=CTK_stock_button_new(1,"");
  s.rows[i]=b;
  if(b){snprintf(name,sizeof name,"UB820_Helper%u_btn_B_BtnB_18",i);gtk_widget_set_name(b,name);gtk_fixed_put(cast(s.fixed,gtk_fixed_get_type()),b,12,46+(int)i*34);gtk_widget_set_uposition(b,12,46+(int)i*34);gtk_widget_set_usize(b,300,30);gtk_widget_show(b);}
  snprintf(name,sizeof name,"UB820_Helper%u_val_lbl_CmnB_18",i);s.values[i]=label(name,322,46+(int)i*34,250,30);
 }
 s.footer=label("UB820_Helper_msg_lbl_CmnB_18",12,46+(int)ROWS*34+4,560,56);
 for(i=0;i<ROWS;i++)if(!s.rows[i]||!s.values[i]){close_sub();return 1;}
 if(!s.title||!s.footer){close_sub();return 1;}
 gtk_widget_show(s.window);
 build_sub_keys();if(ub_key_table)*ub_key_table=sub_keys;
 render_sub();
 if(!s.timer)s.timer=(int)g_timeout_add(250,sub_tick,0);
 plog("UB820 Helper submenu opened");
 return 1;
}
