/* UB820 1.82 native OPTIONS extension. Installed hooks run ONLY on the normal
 * display UI thread. This file is a freestanding PIC DSO, never a standalone
 * player executable. Loader bindings and originals are described in ui_hooks.json.
 *
 * Two categories are added to the OPTIONS root: "External Subtitles" (the
 * .srt browser, while that module is enabled) and "UB820 Helper" (Blu-ray
 * region, Netflix button, SSH, helper log). Both share one child window; m.kind says which
 * page it shows. A choice with several values (Subtitles On/Off, Blu-ray
 * region) opens a list in its own framed window beside its row, like the
 * player's own OPTIONS lists (FUN_001a1454): OK takes the highlighted value,
 * Return keeps the old one. Everything else is reached through files in /tmp.
 *
 * Like the stock categories, the page is shown as soon as the focus is on its
 * tile (m.preview: no row highlighted, keys still go to the root column); OK
 * or Right moves the focus into it, Return back to the tile.
 *
 * Overlay: the UB820 Helper page can also open outside OPTIONS (home screen,
 * menus, playback) when the Netflix button is set to open it (remote module).
 * apl_mgr's key filter then sends key presses through UB_KEYS_FIFO and drops
 * them for everything else while UB_OVERLAY_OPEN exists (helper_menu.h). The
 * payload's timer reads the FIFO; it is started from the hook on apl_dispsrv's
 * main message callback (FUN_00017c58), so it runs without OPTIONS ever being
 * opened.
 */
#include "../external_subtitles/protocol.h"
#include <stddef.h>
#include <stdint.h>
#include "helper_actions.h"
#include "../external_subtitles/brightness_io.h"
#include "tiles_png.h"

typedef void *Widget;
typedef unsigned int GType;
extern Widget CTK_window_new(void),CTK_fixed_new(void),CTK_label_new(const char *);
extern Widget CTK_stock_button_new(int,const char *);
extern Widget CTK_plotbar_new(void);
extern GType CTK_plotbar_get_type(void);
extern void CTK_plotbar_set_min_value(Widget,int),CTK_plotbar_set_max_value(Widget,int);
extern void CTK_plotbar_set_indicator(Widget,int);
extern GType CTK_window_get_type(void),CTK_fixed_get_type(void),CTK_button_get_type(void),CTK_imagebutton_get_type(void);
extern GType gtk_container_get_type(void),gtk_fixed_get_type(void),gtk_label_get_type(void);
extern Widget gtk_type_check_object_cast(Widget,GType);
extern void gtk_widget_set_name(Widget,const char *),gtk_widget_show(Widget),gtk_widget_hide(Widget);
extern void gtk_widget_set_usize(Widget,int,int),gtk_widget_set_uposition(Widget,int,int);
extern void gtk_widget_set_state(Widget,int),gtk_widget_set_sensitive(Widget,int),gtk_widget_destroy(Widget);
extern void gtk_container_add(Widget,Widget),gtk_fixed_put(Widget,Widget,int,int);
extern void gtk_label_set_text(Widget,const char *),gtk_label_set_justify(Widget,int);
extern void CTK_window_set_layer(Widget,int),CTK_fixed_set_shape(Widget,int),CTK_fixed_set_alpha(Widget,int);
extern void CTK_button_set_text(Widget,const char *),CTK_button_set_shape(Widget,int);
extern void CTK_imagebutton_set_bgpixmap(Widget,int,const char *);
extern unsigned int g_timeout_add(unsigned int,int (*)(void *),void *);
extern unsigned int gtk_signal_connect(Widget,const char *,void (*)(Widget,void *),void *);

/* Loader fills these before any branch is installed. ARM function addresses
 * include Thumb bit where appropriate; no fixed-address function calls here. */
unsigned int (*ub_original_root_builder)(int);
void (*ub_original_refresh)(void);
int (*ub_original_key_router)(void);
void *(*ub_get_ctx)(int);
Widget (*ub_lookup)(Widget,const char *);
unsigned int (*ub_get_key)(void *);
int (*ub_register_window)(Widget);
uintptr_t *ub_window_registry;
void *ub_event;
void (*ub_original_input)(void *,int,int);

#define PAGE_OFF 0x490u
#define SELECT_OFF 0x494u
#define COUNT_OFF 0x6d4u
#define ENABLED_OFF 0x644u
#define ROOT_WINDOW_OFF 0x400u
#define ROW_OFF 0x764u
/* Use a valid root-category identity for unhooked native keyguide tables.
 * Distinguish our rows by (id0,child0), owner and compacted index; the native
 * Operating Menu category is (id0,child1). Never leak a private numeric ID or
 * negative child into stock initializer/preview helpers. */
#define CUSTOM_ID 0u
#define ROOT_SLOTS 6u   /* smngbr_ibtn_01..06 */
#define MAX_CUSTOM 2u
#define VIEW_ROWS 9u
#define SUB_ACTIONS 5u
#define HELPER_ROWS 8u
#define DROP_ROWS 11u
/* Our root slots show our own tiles, the caption drawn in white and, for the
 * selected state, in the stock focus colour (tiles_png.h). CTK takes an image
 * path and adds _HD for the HD skin, so both names are written. */
#define TILE_DIR "/tmp/ub820_tile_"
/* The open list: the page window (x 153, y 37, 650x480) and the stock list's
 * geometry, with rows 35 apart like ours. */
#define PAGE_X 153
#define PAGE_Y 37
#define PAGE_W 650
#define PAGE_H 480
#define LIST_PAD 10
#define LIST_ROW_W 250
#define LIST_W (LIST_ROW_W+2*LIST_PAD)
/* Helper log view: the tail of the RAM log, read when the row is opened. */
#define LOG_FILE "/tmp/ub820_submenu.log"
#define LOG_BYTES 12288u
#define LOG_LINES 160u
#define KEY_EXIT 0x80u
#define KEY_BACK 0x81u
#define KEY_OK 0x82u
#define KEY_UP 0x85u
#define KEY_DOWN 0x86u
#define KEY_LEFT 0x87u
#define KEY_RIGHT 0x88u
#define KEY_NETFLIX 0x168u
#define TIMER_MS 100u
#define POLL_TICKS 3u     /* status files every 300 ms */
#define FIFO_RETRY 20u    /* reopen the key FIFO every 2 s while it is missing */
/* Storage's existing tool budget is20s. This bounds only the UI transport
 * wait, allowing callback margin; it never cancels or replays a USB write. */
#define BRIGHTNESS_SAVE_WAIT 30u
enum { PAGE_SUBS, PAGE_HELPER };
enum { H_REGION, H_DVD, H_NETFLIX, H_SSH, H_LOG, H_SAVE, H_POWER, H_CPU };
enum { DROP_NONE, DROP_SUBS, DROP_REGION, DROP_NETFLIX, DROP_DVD, DROP_POWER, DROP_CPU, DROP_BRIGHTNESS };
struct descriptor { uint32_t id,type,label,state; int32_t child; };
static int custom_descriptor(const struct descriptor *d){return d->id==CUSTOM_ID&&d->child==0;}
struct menu_state {
 void *ctx; Widget owner,window,fixed,title,info,footer,rows[VIEW_ROWS],list,list_fixed,drop[DROP_ROWS];
 int tiled[MAX_CUSTOM];
 unsigned int base,count,kinds[MAX_CUSTOM],kind;  /* our root categories: base..base+count-1 */
 unsigned int selected,first,timer,seq,pending,epoch,needs_list,refresh_guard;
 uint32_t pending_deadline;
 uint32_t media_kind,media_number,deck_mode;
 int active; struct ub_status status; char notice[128];
 int log_view; unsigned int log_first,log_count; char *log_lines[LOG_LINES];
 unsigned int drop_kind,drop_sel,drop_count;
 int overlay;               /* the open page is the overlay, not OPTIONS */
 int preview;               /* shown for the focused tile, not entered yet */
 int keys_fd,keys_open;     /* UB_KEYS_FIFO, read end */
 unsigned int ticks,keys_retry;
 uint32_t overlay_deadline;
};
static struct menu_state m;
/* Editing outlives the popup: closing OPTIONS still lets the existing UI
 * timer finish an acknowledged Cancel. One request is outstanding at a time. */
static struct {
 unsigned active,cancel,commit,reset,edited,saving,op,percent,level,sent_percent;
 uint32_t epoch,edit,seq,pid,birth_lo,birth_hi,backend_pid,backend_birth_lo,backend_birth_hi,save_deadline;
 unsigned char token[16];struct ub_brightness_save_ack save;
} brightness;
static struct helper_state hs;
static char log_text[LOG_BYTES+1];
static struct ub_status incoming;
static const char *const root_names[ROOT_SLOTS]={"smngbr_ibtn_01","smngbr_ibtn_02","smngbr_ibtn_03","smngbr_ibtn_04","smngbr_ibtn_05","smngbr_ibtn_06"};
static uint32_t *field(void *p,unsigned int off){return (uint32_t *)((char *)p+off);}
static Widget root_window(void *p){return (Widget)(uintptr_t)*field(p,ROOT_WINDOW_OFF);}
static Widget cast(Widget w,GType type){return gtk_type_check_object_cast(w,type);}
static int bounded_path(const char *p){return p&&p[0]=='/'&&strnlen(p,UB_PATH_BYTES)<=UB_PATH_LIMIT;}
static void unregister_window(Widget w){unsigned int i;if(ub_window_registry)for(i=0;i<100;i++)if(ub_window_registry[i]==(uintptr_t)w)ub_window_registry[i]=0;}
/* The value list is made each time it opens and destroyed when it closes:
 * shown again after a hide, its rows lost their text on the player. */
static void close_list(void){
 Widget w=m.list;m.list=m.list_fixed=0;memset(m.drop,0,sizeof m.drop);
 if(w){unregister_window(w);gtk_widget_destroy(w);}
}
static int create_list(void);
static void render_brightness(void);
static void brightness_abort(void),brightness_pump(void),brightness_forget(void);
static int brightness_ack(const struct ub_status *);
static void close_child(int restore){
 Widget w=m.window;
 if(m.kind==PAGE_SUBS&&m.drop_kind==DROP_BRIGHTNESS)brightness_abort();
 close_list();
 if(m.overlay){m.overlay=0;unlink(UB_OVERLAY_OPEN);unlink(UB_OVERLAY_ABORT);m.overlay_deadline=0;restore=0;}
 m.window=0;m.active=0;m.preview=0;m.log_view=0;m.drop_kind=DROP_NONE;
 if(w){unregister_window(w);gtk_widget_destroy(w);}
 m.fixed=m.title=m.info=m.footer=0;memset(m.rows,0,sizeof m.rows);
 if(restore&&ub_original_refresh&&m.ctx&&root_window(m.ctx)==m.owner)ub_original_refresh();
}
/* The notice under the rows: the page's own, or the helper actions'. */
static const char *notice(void){return m.kind==PAGE_HELPER?hs.notice:m.notice;}
static void set_notice(const char *text){ha_copy(m.kind==PAGE_HELPER?hs.notice:m.notice,text,sizeof m.notice);}

/* ---- Rows of the open page ------------------------------------------------- */
static unsigned int helper_rows(unsigned int *ids){
 unsigned int n=0;ids[n++]=H_REGION;if(hs.dvd_known)ids[n++]=H_DVD;if(hs.netflix_known)ids[n++]=H_NETFLIX;if(hs.power_enabled)ids[n++]=H_POWER;if(hs.cpu_enabled)ids[n++]=H_CPU;if(hs.ssh_enabled)ids[n++]=H_SSH;ids[n++]=H_LOG;ids[n++]=H_SAVE;return n;
}
static const char *helper_text(unsigned int id,char *buf,size_t cap){
 if(id==H_REGION){
  char value[64];
  if(m.drop_kind==DROP_REGION)return "Blu-ray region";  /* the list shows the values */
  snprintf(buf,cap,"Blu-ray region: %s",ha_region_value(&hs,value,sizeof value));
  return buf;
 }
 if(id==H_DVD){
  if(m.drop_kind==DROP_DVD)return "DVD region";
  snprintf(buf,cap,"DVD region: %s",ha_dvd_labels[hs.dvd_choice<HA_DVD_CHOICES?hs.dvd_choice:0]);
  return buf;
 }
 if(id==H_NETFLIX){
  if(m.drop_kind==DROP_NETFLIX)return "Netflix button";
  snprintf(buf,cap,"Netflix button: %s",ha_netflix_labels[hs.netflix_choice<HA_NETFLIX_CHOICES?hs.netflix_choice:0]);
  return buf;
 }
 if(id==H_SSH)return hs.ssh_running?"Stop SSH":"Start SSH";
 if(id==H_POWER){
  if(m.drop_kind==DROP_POWER)return "Auto power-off";
  snprintf(buf,cap,"Auto power-off: %s",ha_power_value(&hs));return buf;
 }
 if(id==H_CPU){
  if(m.drop_kind==DROP_CPU)return "CPU optimization";
  snprintf(buf,cap,"CPU optimization: %s",ha_cpu_value(&hs));return buf;
 }
 if(id==H_LOG)return "Helper log";
 return hs.log_always?"Log is saved to USB automatically":"Save log to USB";
}
static unsigned int total_rows(void){unsigned int ids[HELPER_ROWS];return m.kind==PAGE_HELPER?helper_rows(ids):SUB_ACTIONS+m.status.count;}
static const char *row_text(unsigned int n,char *buf,size_t cap){
 size_t i;
 if(m.kind==PAGE_HELPER){unsigned int ids[HELPER_ROWS],c=helper_rows(ids);return n<c?helper_text(ids[n],buf,cap):"";}
 if(n==0)return "[..] Up one folder";
 if(n==1)return m.drop_kind==DROP_SUBS?"Subtitles":m.status.enabled?"Subtitles: On":"Subtitles: Off";
 if(n==2){
  if(!m.status.brightness_available||!m.status.brightness_known)return "Text brightness: unavailable";
  if(m.status.brightness_percent==100)return "Text brightness: Original (100%)";
  snprintf(buf,cap,"Text brightness: %u%%",m.status.brightness_percent);return buf;
 }
 if(n==3)return "Reset text brightness";
 if(n==4)return "Refresh files";
 n-=SUB_ACTIONS;if(n>=m.status.count)return "";
 snprintf(buf,cap,"%s%s",m.status.items[n].kind==UB_KIND_DIR?"[Folder] ":"",m.status.items[n].name);
 for(i=0;buf[i];i++)if((unsigned char)buf[i]<32||buf[i]==127)buf[i]=' ';
 return buf;
}
/* Reads the last LOG_BYTES of the log into lines, newest at the bottom. */
static void load_log(void){
 int fd=open(LOG_FILE,O_RDONLY);off_t size,from;ssize_t n=0,got;char *p,*end;
 m.log_count=0;log_text[0]=0;
 if(fd>=0){
  size=lseek(fd,0,SEEK_END);from=size>(off_t)LOG_BYTES?size-(off_t)LOG_BYTES:0;
  if(size>=0&&lseek(fd,from,SEEK_SET)==from)while((size_t)n<LOG_BYTES&&(got=read(fd,log_text+n,LOG_BYTES-(size_t)n))>0)n+=got;
  close(fd);
  log_text[n]=0;p=log_text;end=log_text+n;
  if(from>0)while(p<end&&*p!='\n')p++; /* drop the cut first line */
  while(p<end){
   char *line;
   if(*p=='\n'){p++;continue;}
   line=p;while(p<end&&*p!='\n'){if((unsigned char)*p<32||*p==127)*p=' ';p++;}
   if(p<end)*p++=0;
   if(m.log_count==LOG_LINES){memmove(m.log_lines,m.log_lines+1,sizeof m.log_lines-sizeof *m.log_lines);m.log_count--;}
   m.log_lines[m.log_count++]=line;
  }
 }
 m.log_first=m.log_count>VIEW_ROWS?m.log_count-VIEW_ROWS:0;
}
static void render_log(void){
 unsigned int i;char text[160];
 gtk_label_set_text(cast(m.title,gtk_label_get_type()),"Helper log\n" LOG_FILE);
 if(m.log_count)snprintf(text,sizeof text,"Lines %u-%u of %u",m.log_first+1,m.log_first+VIEW_ROWS<m.log_count?m.log_first+VIEW_ROWS:m.log_count,m.log_count);
 else ha_copy(text,"The log is empty or cannot be read",sizeof text);
 gtk_label_set_text(cast(m.info,gtk_label_get_type()),text);
 for(i=0;i<VIEW_ROWS;i++){
  unsigned int n=m.log_first+i;
  if(n<m.log_count){CTK_button_set_text(cast(m.rows[i],CTK_button_get_type()),m.log_lines[n]);gtk_widget_show(m.rows[i]);gtk_widget_set_sensitive(m.rows[i],1);gtk_widget_set_state(m.rows[i],0);}
  else gtk_widget_hide(m.rows[i]);
 }
 close_list();
 gtk_label_set_text(cast(m.footer,gtk_label_get_type()),"Up/Down: scroll   OK/Return: back   OPTIONS: close");
}
/* The open list beside the selected row. */
static const char *drop_text(unsigned int k){
 if(m.drop_kind==DROP_SUBS)return k?"Off":"On";
 if(m.drop_kind==DROP_POWER)return k?"Off":"On";
 if(m.drop_kind==DROP_CPU)return k?"Off":"On";
 if(m.drop_kind==DROP_NETFLIX)return k<HA_NETFLIX_CHOICES?ha_netflix_labels[k]:"";
 if(m.drop_kind==DROP_DVD)return k<HA_DVD_CHOICES?ha_dvd_labels[k]:"";
 return k<HA_REGION_CHOICES?ha_region_labels[k]:"";
}
/* The list window: right-aligned to the page, its first value level with the
 * row, moved up when it would run past the page. */
static void render_drop(void){
 unsigned int i,top=m.selected>=m.first?m.selected-m.first:0;int x,y,h;
 if(m.drop_kind==DROP_NONE){close_list();return;}
 if(m.drop_kind==DROP_BRIGHTNESS){render_brightness();return;}
 if(!m.list&&create_list()!=0){close_list();m.drop_kind=DROP_NONE;return;}
 h=LIST_PAD+(int)m.drop_count*35+LIST_PAD-4;
 x=PAGE_X+PAGE_W-LIST_W;y=PAGE_Y+94+(int)top*35-LIST_PAD;
 if(y+h>PAGE_Y+PAGE_H)y=PAGE_Y+PAGE_H-h;
 gtk_widget_set_uposition(m.list,x,y);gtk_widget_set_usize(m.list,LIST_W,h);gtk_widget_set_usize(m.list_fixed,LIST_W,h);
 for(i=0;i<DROP_ROWS;i++){
  if(i<m.drop_count){
   CTK_button_set_text(cast(m.drop[i],CTK_button_get_type()),drop_text(i));
   gtk_widget_set_state(m.drop[i],i==m.drop_sel?2:0);gtk_widget_show(m.drop[i]);
  }else gtk_widget_hide(m.drop[i]);
 }
 gtk_widget_show(m.list);
}
static void render(void){
 unsigned int i,total=total_rows();char text[512],row[160];
 if(!m.active||!m.window)return;
 /* A backend loss/unknown result removes the choice that owns this popup.
  * Close it before rendering the newly compacted rows or their values. */
 if(m.drop_kind==DROP_POWER&&(!hs.power_enabled||!hs.power_known)){
  m.drop_kind=DROP_NONE;close_list();
 }
 if(m.drop_kind==DROP_CPU&&(!hs.cpu_enabled||!hs.cpu_known)){
  m.drop_kind=DROP_NONE;close_list();
 }
 if(m.log_view){render_log();return;}
 if(m.selected>=total)m.selected=total?total-1:0;
 if(m.selected<m.first)m.first=m.selected;
 if(m.selected>=m.first+VIEW_ROWS)m.first=m.selected-VIEW_ROWS+1;
 if(m.kind==PAGE_HELPER){
  gtk_label_set_text(cast(m.title,gtk_label_get_type()),"UB820 Helper");
  gtk_label_set_text(cast(m.info,gtk_label_get_type()),"Choices are kept on the USB stick");
 }else{
  snprintf(text,sizeof text,"External Subtitles\n%s",m.status.dir[0]?m.status.dir:"/mnt/usb");
  gtk_label_set_text(cast(m.title,gtk_label_get_type()),text);
  snprintf(text,sizeof text,"%s | %s%s%s",m.status.enabled?"On":"Off",m.status.current[0]?m.status.current:"No subtitle selected",m.pending?" | Waiting...":"",m.status.count==UB_MAX_ITEMS?" | First 64 entries":"");
  gtk_label_set_text(cast(m.info,gtk_label_get_type()),text);
 }
 for(i=0;i<VIEW_ROWS;i++){
  unsigned int n=m.first+i;
  if(n<total){CTK_button_set_text(cast(m.rows[i],CTK_button_get_type()),row_text(n,row,sizeof row));gtk_widget_show(m.rows[i]);gtk_widget_set_sensitive(m.rows[i],1);gtk_widget_set_state(m.rows[i],n==m.selected&&!m.preview?2:0);}
  else gtk_widget_hide(m.rows[i]);
 }
 render_drop();
 if(m.drop_kind==DROP_BRIGHTNESS)ha_copy(text,brightness.saving||brightness.op==UB_OP_BRIGHTNESS_COMMIT?"Saving brightness to USB...":"Left/Right: 1%   Up/Down: 10%\nOK: save   Return: cancel",sizeof text);
 else if(m.drop_kind!=DROP_NONE)ha_copy(text,"Up/Down: choose   OK: take   Return: keep as it was",sizeof text);
 else if(m.preview)snprintf(text,sizeof text,"%s\nOK: open   OPTIONS: close",notice()[0]?notice():m.kind==PAGE_HELPER?"":m.status.message);
 else snprintf(text,sizeof text,"%s\nUp/Down: choose   OK: select   %s",notice()[0]?notice():m.kind==PAGE_HELPER?"":m.status.message,m.overlay?"Return: close":"Return: back   OPTIONS: close");
 gtk_label_set_text(cast(m.footer,gtk_label_get_type()),text);
}

/* ---- External Subtitles requests ---------------------------------------- */
/* Check elapsed time, not timer callback counts: a busy UI may dispatch its
 * timer late. Also called on a click so an expired request cannot keep the
 * page stuck behind a generic busy message until the next timer arrives. */
static int subtitle_pending_expire(void){
 uint32_t now;
 if(!m.pending)return 0;
 if(!ha_action_now(&now)&&(int32_t)(now-m.pending_deadline)<0)return 0;
 m.pending=0;m.needs_list=1;
 if(brightness.active&&brightness.op){
  if(brightness.op==UB_OP_BRIGHTNESS_COMMIT){ha_copy(m.notice,"Brightness save result is unconfirmed; reopen to check",sizeof m.notice);brightness_forget();return 1;}
  if(brightness.op==UB_OP_BRIGHTNESS_CANCEL){ha_copy(m.notice,"Brightness restore is still awaiting confirmation",sizeof m.notice);return 1;}
  brightness.op=0;brightness.cancel=1;brightness.commit=0;
 }
 ha_copy(m.notice,m.epoch?"Subtitle helper did not answer; refresh files to retry":"Subtitle helper is not ready; check Helper log, then refresh files",sizeof m.notice);
 return 1;
}
static int publish_request(const struct ub_request *r,uint32_t now){
 char temp[512];int fd;size_t at=0;ssize_t n;
 snprintf(temp,sizeof temp,"%s.ui-%ld",UB_REQUEST_FILE,(long)getpid());
 fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -1;
 while(at<sizeof *r){n=write(fd,(const char *)r+at,sizeof *r-at);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);unlink(temp);return -1;}at+=(size_t)n;}
 if(close(fd)){unlink(temp);return -1;}
 if(rename(temp,UB_REQUEST_FILE)){unlink(temp);return -1;}
 m.pending=r->seq;m.pending_deadline=now+(m.epoch?12u:3u);return 0;
}
static int send_request(unsigned int op,const char *path){
 struct ub_request r;uint32_t now;
 subtitle_pending_expire();
 if(brightness.active){ha_copy(m.notice,"Finish the current brightness change first",sizeof m.notice);brightness_pump();render();return -1;}
 if(m.pending){ha_copy(m.notice,"Waiting for subtitle helper to answer",sizeof m.notice);render();return -1;}
 if(ha_action_now(&now))goto bad;
 if(op!=UB_OP_LIST&&(m.needs_list||!m.epoch)){ha_copy(m.notice,"Refresh files before selecting",sizeof m.notice);render();return -1;}
 if(path&&path[0]&&!bounded_path(path)){ha_copy(m.notice,"Path is too long or invalid",sizeof m.notice);render();return -1;}
 memset(&r,0,sizeof r);r.magic=UB_MAGIC;r.version=UB_VERSION;r.seq=++m.seq;
 if(!r.seq)r.seq=++m.seq;
 r.epoch=op==UB_OP_LIST?0:m.epoch;r.op=op;ha_copy(r.path,path,sizeof r.path);
 if(publish_request(&r,now))goto bad;
 m.notice[0]=0;render();return 0;
bad:ha_copy(m.notice,"Cannot contact subtitle helper",sizeof m.notice);render();return -1;
}
static int status_valid(struct ub_status *s){
 unsigned int i;if(s->magic!=UB_MAGIC||s->version!=UB_VERSION||s->count>UB_MAX_ITEMS||!s->epoch)return 0;
 if(s->text_level>255||s->brightness_percent>100||s->brightness_known>1||s->brightness_available>1||s->brightness_phase>UB_BRIGHTNESS_COMMIT)return 0;
 if(!memchr(s->dir,0,sizeof s->dir)||!memchr(s->current,0,sizeof s->current)||!memchr(s->message,0,sizeof s->message))return 0;
 for(i=0;i<s->count;i++)if((s->items[i].kind!=UB_KIND_FILE&&s->items[i].kind!=UB_KIND_DIR)||!memchr(s->items[i].name,0,UB_NAME_BYTES)||!memchr(s->items[i].path,0,UB_PATH_BYTES)||!bounded_path(s->items[i].path))return 0;
 return 1;
}
static void read_status(void){
 if(ub_br_private_read(UB_STATUS_FILE,&incoming,sizeof incoming)||!status_valid(&incoming))return;
 int same_backend=brightness.active&&!memcmp(brightness.token,incoming.brightness_token,16)&&brightness.backend_pid==incoming.brightness_pid&&brightness.backend_birth_lo==incoming.brightness_birth_lo&&brightness.backend_birth_hi==incoming.brightness_birth_hi;
 int accepted_commit=same_backend&&(brightness.saving||
  (brightness.op==UB_OP_BRIGHTNESS_COMMIT&&incoming.brightness_commit_seq==brightness.seq&&incoming.brightness_commit_epoch==brightness.epoch&&incoming.brightness_commit_percent==brightness.percent&&incoming.brightness_commit_level==brightness.level));
 /* Generation changes invalidate pending mutations. A fresh LIST is the only
  * epoch-independent request; never replay SELECT/ON/OFF into a new backend. */
 if(m.epoch&&m.epoch!=incoming.epoch){
  if(!accepted_commit){brightness_forget();m.pending=0;}
  m.needs_list=1;ha_copy(m.notice,accepted_commit?"Playback changed; text brightness save is finishing":"Helper restarted; refresh files before selecting",sizeof m.notice);
 }
 m.epoch=incoming.epoch;
 if(incoming.seq>m.seq)m.seq=incoming.seq;
 memcpy(&m.status,&incoming,sizeof m.status);
 if(brightness.active){
  if(!same_backend){brightness_forget();m.pending=0;m.needs_list=1;ha_copy(m.notice,"Subtitle engine changed; brightness edit closed",sizeof m.notice);}
  else if(brightness.op&&incoming.seq==brightness.seq){
   uint32_t edit_epoch=brightness.epoch;
   if(brightness_ack(&incoming)){m.pending=0;if(incoming.result==UB_OK&&edit_epoch==m.epoch)m.needs_list=0;}
  }
 }else if(m.pending&&incoming.seq==m.pending){m.pending=0;if(incoming.result==UB_OK)m.needs_list=0;}
 brightness_pump();render();
}
static int brightness_owner(uint32_t *pid,uint32_t *lo,uint32_t *hi){
 char text[2048],*p;uint64_t value=0;
 if(!ha_read_line("/proc/self/stat",text,sizeof text)||(p=strrchr(text,')'))==0||p[1]!=' ')return -1;
 p+=2;for(unsigned field=3;field<22;field++){p=strchr(p,' ');if(!p)return -1;while(*p==' ')p++;}
 if(*p<'0'||*p>'9')return -1;
 do{unsigned d=(unsigned)(*p++-'0');if(value>UINT64_C(1844674407370955161)||(value==UINT64_C(1844674407370955161)&&d>5))return -1;value=value*10u+d;}while(*p>='0'&&*p<='9');
 if(!value||(*p&&*p!=' '))return -1;
 *pid=(uint32_t)getpid();*lo=(uint32_t)value;*hi=(uint32_t)(value>>32);return *pid>1?0:-1;
}
static int brightness_owned(const struct ub_status *s){
 return s->brightness_edit==brightness.edit&&s->brightness_edit_epoch==brightness.epoch&&
  s->brightness_owner_pid==brightness.pid&&s->brightness_owner_start_lo==brightness.birth_lo&&s->brightness_owner_start_hi==brightness.birth_hi;
}
static void brightness_forget(void){
 if(m.pending==brightness.seq)m.pending=0;
 memset(&brightness,0,sizeof brightness);
 if(m.drop_kind==DROP_BRIGHTNESS){m.drop_kind=DROP_NONE;close_list();}
}
static void brightness_abort(void){
 if(!brightness.active||brightness.saving||brightness.op==UB_OP_BRIGHTNESS_COMMIT)return;
 brightness.cancel=1;brightness.commit=0;brightness.reset=0;
}
static int brightness_send(unsigned op){
 struct ub_request r;uint32_t now;
 if(m.pending||brightness.op||ha_action_now(&now))return -1;
 memset(&r,0,sizeof r);r.magic=UB_MAGIC;r.version=UB_VERSION;r.seq=++m.seq;if(!r.seq)r.seq=++m.seq;
 r.epoch=brightness.epoch;r.op=op;r.brightness.edit=op==UB_OP_BRIGHTNESS_BEGIN?0:brightness.edit;
 r.brightness.value=op==UB_OP_BRIGHTNESS_PREVIEW||op==UB_OP_BRIGHTNESS_COMMIT?brightness.percent:0;
 r.brightness.owner_pid=brightness.pid;r.brightness.owner_start_lo=brightness.birth_lo;r.brightness.owner_start_hi=brightness.birth_hi;memcpy(r.brightness.token,brightness.token,16);
 if(publish_request(&r,now)){ha_copy(m.notice,"Cannot contact subtitle helper",sizeof m.notice);return -1;}
 brightness.seq=r.seq;brightness.op=op;
 brightness.sent_percent=r.brightness.value;
 if(op==UB_OP_BRIGHTNESS_BEGIN)brightness.edit=r.seq;
 if(op==UB_OP_BRIGHTNESS_COMMIT){
  memset(&brightness.save,0,sizeof brightness.save);brightness.save.magic=UB_BRIGHTNESS_SAVE_MAGIC;brightness.save.version=UB_BRIGHTNESS_PROTOCOL;
  memcpy(brightness.save.token,brightness.token,16);brightness.save.pid=brightness.backend_pid;brightness.save.birth_lo=brightness.backend_birth_lo;brightness.save.birth_hi=brightness.backend_birth_hi;
  brightness.save.edit=brightness.edit;brightness.save.epoch=brightness.epoch;brightness.save.seq=r.seq;brightness.save.percent=brightness.percent;brightness.save.level=brightness.level;
 }
 return 0;
}
static int brightness_ack(const struct ub_status *s){
 unsigned op=brightness.op;
 if(s->result==UB_OK){
  if(op==UB_OP_BRIGHTNESS_BEGIN||op==UB_OP_BRIGHTNESS_PREVIEW){
   if(!brightness_owned(s)||s->brightness_phase!=UB_BRIGHTNESS_PREVIEW||!s->brightness_known)return 0;
   if(op==UB_OP_BRIGHTNESS_PREVIEW&&(s->brightness_percent!=brightness.sent_percent||s->text_level!=ub_brightness_level(brightness.sent_percent)))return 0;
  }else if(op==UB_OP_BRIGHTNESS_COMMIT){
   if((s->brightness_phase!=UB_BRIGHTNESS_COMMIT&&s->brightness_phase!=UB_BRIGHTNESS_NONE)||
      (s->brightness_phase==UB_BRIGHTNESS_COMMIT&&!brightness_owned(s))||
      (s->brightness_phase==UB_BRIGHTNESS_NONE&&s->brightness_edit))return 0;
   if(s->brightness_commit_seq!=brightness.seq||s->brightness_commit_epoch!=brightness.epoch||
      s->brightness_commit_percent!=brightness.percent||s->brightness_commit_level!=brightness.level)return 0;
  }else if(op==UB_OP_BRIGHTNESS_CANCEL&&(s->brightness_edit||s->brightness_phase!=UB_BRIGHTNESS_NONE||!s->brightness_known))return 0;
 }
 brightness.op=0;
 if(s->result!=UB_OK){
  ha_copy(m.notice,s->message[0]?s->message:"Brightness change was not confirmed",sizeof m.notice);
  brightness.commit=brightness.reset=0;
  if(op==UB_OP_BRIGHTNESS_CANCEL)brightness.cancel=0;
  if(op==UB_OP_BRIGHTNESS_BEGIN||!brightness_owned(s))brightness_forget();
  else if(s->brightness_known){brightness.percent=s->brightness_percent;brightness.level=s->text_level;m.drop_sel=brightness.percent;}
  return 1;
 }
 if(op==UB_OP_BRIGHTNESS_CANCEL){ha_copy(m.notice,"Brightness restored; no choice saved",sizeof m.notice);brightness_forget();return 1;}
 if(op==UB_OP_BRIGHTNESS_COMMIT){uint32_t now=0;brightness.saving=1;if(!ha_action_now(&now))brightness.save_deadline=now+BRIGHTNESS_SAVE_WAIT;ha_copy(m.notice,"Saving text brightness to USB...",sizeof m.notice);return 1;}
 brightness.level=s->text_level;
 if(op==UB_OP_BRIGHTNESS_BEGIN){if(!brightness.reset&&!brightness.edited)brightness.percent=s->brightness_percent;m.drop_sel=brightness.percent;}
 return 1;
}
static void brightness_pump(void){
 if(!brightness.active)return;
 if(brightness.saving){
  struct ub_brightness_save_ack ack;uint32_t now;
  if(ub_br_private_read(UB_BRIGHTNESS_SAVE_ACK,&ack,sizeof ack)||ack.result<UB_BRIGHTNESS_SAVED||ack.result>UB_BRIGHTNESS_SAVE_UNKNOWN||memcmp(&ack,&brightness.save,offsetof(struct ub_brightness_save_ack,result))){
   if(!brightness.save_deadline||ha_action_now(&now)||(int32_t)(now-brightness.save_deadline)>=0){ha_copy(m.notice,"Brightness applied; USB save result is unconfirmed",sizeof m.notice);brightness_forget();}
   return;
  }
  ha_copy(m.notice,ack.result==UB_BRIGHTNESS_SAVED?"Text brightness saved":ack.result==UB_BRIGHTNESS_SAVE_STALE?"Playback changed; brightness was not saved":ack.result==UB_BRIGHTNESS_SAVE_ERROR?"Brightness applied; not saved to USB":"Brightness applied; USB save result is unconfirmed",sizeof m.notice);
  brightness_forget();return;
 }
 if(brightness.op||m.pending)return;
 if(brightness.cancel){brightness_send(UB_OP_BRIGHTNESS_CANCEL);return;}
 if(m.status.brightness_percent!=brightness.percent||
    ((brightness.edited||brightness.commit||brightness.reset)&&m.status.text_level!=ub_brightness_level(brightness.percent))){brightness_send(UB_OP_BRIGHTNESS_PREVIEW);return;}
 if(brightness.commit)brightness_send(UB_OP_BRIGHTNESS_COMMIT);
}
static void brightness_begin(unsigned reset){
 subtitle_pending_expire();
 if(brightness.active){
  if(!brightness.cancel&&!brightness.saving&&brightness.op!=UB_OP_BRIGHTNESS_COMMIT){if(m.drop_kind!=DROP_BRIGHTNESS)close_list();m.drop_kind=DROP_BRIGHTNESS;m.drop_sel=brightness.percent;render();return;}
  ha_copy(m.notice,"The previous brightness change is still finishing",sizeof m.notice);render();return;
 }
 if(m.pending||m.needs_list||!m.epoch){ha_copy(m.notice,"Refresh files before changing brightness",sizeof m.notice);render();return;}
 if(!m.status.brightness_available||!m.status.brightness_known){ha_copy(m.notice,"Text brightness is not available yet",sizeof m.notice);render();return;}
 memset(&brightness,0,sizeof brightness);
 if(brightness_owner(&brightness.pid,&brightness.birth_lo,&brightness.birth_hi)){ha_copy(m.notice,"Cannot verify the menu process",sizeof m.notice);render();return;}
 unsigned token=0;for(unsigned i=0;i<16;i++)token|=m.status.brightness_token[i];
 if(!token||!m.status.brightness_pid||!(m.status.brightness_birth_lo|m.status.brightness_birth_hi)){ha_copy(m.notice,"Brightness helper is not ready",sizeof m.notice);render();return;}
 brightness.active=1;brightness.epoch=m.epoch;brightness.percent=reset?100:m.status.brightness_percent;brightness.level=m.status.text_level;brightness.reset=reset;brightness.commit=reset;
 brightness.backend_pid=m.status.brightness_pid;brightness.backend_birth_lo=m.status.brightness_birth_lo;brightness.backend_birth_hi=m.status.brightness_birth_hi;memcpy(brightness.token,m.status.brightness_token,16);
 if(m.drop_kind!=DROP_BRIGHTNESS)close_list();
 m.drop_kind=DROP_BRIGHTNESS;m.drop_sel=brightness.percent;
 if(brightness_send(UB_OP_BRIGHTNESS_BEGIN))brightness_forget();
 render();
}
static void brightness_key(unsigned key){
 unsigned value=brightness.percent;
 if(!brightness.active||m.drop_kind!=DROP_BRIGHTNESS)return;
 if(brightness.op==UB_OP_BRIGHTNESS_COMMIT||brightness.saving){if(key==KEY_BACK){m.drop_kind=DROP_NONE;close_list();}return;}
 if(key==KEY_BACK){brightness_abort();brightness_pump();render();return;}
 if(brightness.cancel)return;
 if(key==KEY_OK){brightness.commit=1;brightness_pump();render();return;}
 if(brightness.commit)return;
 if(key==KEY_LEFT){if(value)value--;}
 else if(key==KEY_RIGHT){if(value<100)value++;}
 else if(key==KEY_UP)value=value>90?100:value+10;
 else if(key==KEY_DOWN)value=value<10?0:value-10;
 else return;
 if(value!=brightness.percent)brightness.edited=1;
 brightness.percent=value;m.drop_sel=value;brightness_pump();render();
}
static int owner_alive(void){
 void *ctx=ub_get_ctx?ub_get_ctx(0x27):0;
 return ctx&&ctx==m.ctx&&m.owner&&root_window(ctx)==m.owner&&*field(ctx,PAGE_OFF)==0&&*field(ctx,0x33f8)==m.media_kind&&*field(ctx,0x33f4)==m.media_number&&*field(ctx,0x33e4)==m.deck_mode;
}
static void overlay_key(unsigned int key);
static void ensure_timer(void);
/* Key presses from apl_mgr (u32 key codes); the FIFO stays open for reading
 * so that apl_mgr knows the overlay can take keys. */
static void read_keys(int discard){
 uint32_t keys[16];ssize_t n;unsigned int i;
 if(!m.keys_open){
  if(m.keys_retry){m.keys_retry--;return;}
  m.keys_fd=open(UB_KEYS_FIFO,O_RDONLY|O_NONBLOCK);
  if(m.keys_fd<0){m.keys_retry=FIFO_RETRY;return;}
  m.keys_open=1;
 }
 while((n=read(m.keys_fd,keys,sizeof keys))>0)
  for(i=0;!discard&&i+1<=(unsigned int)n/sizeof *keys;i++)overlay_key(keys[i]);
}
static int overlay_renew(void){
 uint32_t now;
 if(ha_action_now(&now))return -1;
 m.overlay_deadline=now+3u;
 return ub_action_write(UB_OVERLAY_OPEN,(const char *)&m.overlay_deadline,sizeof m.overlay_deadline);
}
static int timer_tick(void *unused){
 uint32_t now;int discard=ha_exists(UB_OVERLAY_ABORT);
 (void)unused;
 if(m.overlay&&(discard||ha_action_now(&now)||(int32_t)(now-m.overlay_deadline)>=0)){close_child(0);discard=1;}
 unlink(UB_OVERLAY_ABORT);
 read_keys(discard);
 if(m.overlay&&overlay_renew())close_child(0);
 if(++m.ticks<POLL_TICKS)return 1;
 m.ticks=0;
 if(m.active&&!m.overlay&&!owner_alive())close_child(0);
 int edited=brightness.active;
 if(edited){read_status();if(subtitle_pending_expire())render();brightness_pump();}
 if(m.active){
  if(m.kind==PAGE_SUBS){
   if(!edited)read_status();
   if(subtitle_pending_expire())render();
  }else{
   int changed=ha_region_poll(&hs);changed|=ha_dvd_poll(&hs);changed|=ha_netflix_poll(&hs);changed|=ha_power_poll(&hs);changed|=ha_cpu_poll(&hs);changed|=ha_ssh_poll(&hs);changed|=ha_log_poll(&hs);
   if(changed&&!m.log_view)render();
  }
 }
 return 1;
}

/* ---- Child window --------------------------------------------------------- */
static void owner_destroyed(Widget w,void *unused){(void)unused;if(w==m.owner){if(!m.overlay)close_child(0);m.owner=0;memset(m.tiled,0,sizeof m.tiled);m.count=0;if(!brightness.active)m.pending=0;}}
static Widget label(Widget fixed,const char *name,int x,int y,int width,int height){
 Widget w=CTK_label_new("");if(!w)return 0;gtk_widget_set_name(w,name);gtk_fixed_put(cast(fixed,gtk_fixed_get_type()),w,x,y);gtk_widget_set_uposition(w,x,y);gtk_widget_set_usize(w,width,height);gtk_label_set_justify(cast(w,gtk_label_get_type()),0);gtk_widget_show(w);return w;
}
/* Native CTK plotbar, as used by stock FUN_0013762c: integer bounds and
 * indicator, with the player's existing Navi_PltA skin. It is a small adjacent
 * control, not a list containing one widget for every possible percentage. */
static int create_brightness(void){
 Widget bar;
 m.list=CTK_window_new();if(!m.list)return -1;
 gtk_widget_set_name(m.list,"ub820_brightness_window_CmnE_18");
 CTK_window_set_layer(cast(m.list,CTK_window_get_type()),m.overlay?3:2);
 m.list_fixed=CTK_fixed_new();if(!m.list_fixed)return -1;
 gtk_widget_set_name(m.list_fixed,"ub820_brightness_fixed_SWIN_C_18");
 CTK_fixed_set_shape(cast(m.list_fixed,CTK_fixed_get_type()),3);CTK_fixed_set_alpha(cast(m.list_fixed,CTK_fixed_get_type()),255);
 gtk_container_add(cast(m.list,gtk_container_get_type()),m.list_fixed);gtk_widget_show(m.list_fixed);
 m.drop[0]=label(m.list_fixed,"ub820_brightness_title_SLBL_A_18",10,6,320,24);
 m.drop[1]=label(m.list_fixed,"ub820_brightness_value_SLBL_A_18",10,32,320,26);
 bar=CTK_plotbar_new();if(!bar)return -1;
 gtk_widget_set_name(bar,"ub820_brightness_bar_Navi_PltA_18");
 gtk_fixed_put(cast(m.list_fixed,gtk_fixed_get_type()),bar,20,68);gtk_widget_set_uposition(bar,20,68);gtk_widget_set_usize(bar,300,22);
 CTK_plotbar_set_max_value(cast(bar,CTK_plotbar_get_type()),100);
 CTK_plotbar_set_min_value(cast(bar,CTK_plotbar_get_type()),0);gtk_widget_show(bar);m.drop[2]=bar;
 m.drop[3]=label(m.list_fixed,"ub820_brightness_hint_SLBL_B_16",10,96,320,22);
 if(!m.drop[0]||!m.drop[1]||!m.drop[3])return -1;
 if(ub_register_window&&ub_register_window(m.list)!=0)return -1;
 return 0;
}
static void render_brightness(void){
 char text[64];unsigned top=m.selected>=m.first?m.selected-m.first:0;int y;
 if(!m.list&&create_brightness()){close_list();m.drop_kind=DROP_NONE;return;}
 y=PAGE_Y+94+(int)top*35-LIST_PAD;
 if(y+124>PAGE_Y+PAGE_H)y=PAGE_Y+PAGE_H-124;
 gtk_widget_set_uposition(m.list,PAGE_X+PAGE_W-340,y);gtk_widget_set_usize(m.list,340,124);gtk_widget_set_usize(m.list_fixed,340,124);
 gtk_label_set_text(cast(m.drop[0],gtk_label_get_type()),"Text brightness");
 if(brightness.cancel)ha_copy(text,"Restoring previous brightness...",sizeof text);
 else if(brightness.saving||brightness.op==UB_OP_BRIGHTNESS_COMMIT)ha_copy(text,"Saving to USB...",sizeof text);
 else if(m.drop_sel==100)ha_copy(text,brightness.op?"Original (100%) - applying...":"Original (100%)",sizeof text);
 else snprintf(text,sizeof text,brightness.op?"%u%% - applying...":"%u%%",m.drop_sel);
 gtk_label_set_text(cast(m.drop[1],gtk_label_get_type()),text);
 CTK_plotbar_set_indicator(cast(m.drop[2],CTK_plotbar_get_type()),(int)m.drop_sel);
 gtk_label_set_text(cast(m.drop[3],gtk_label_get_type()),"0% is black text, not Off");gtk_widget_show(m.list);
}
static Widget row_button(const char *name,int x,int y,int width){
 Widget w=CTK_stock_button_new(1,"");if(!w)return 0;
 gtk_widget_set_name(w,name);gtk_fixed_put(cast(m.fixed,gtk_fixed_get_type()),w,x,y);gtk_widget_set_uposition(w,x,y);gtk_widget_set_usize(w,width,31);CTK_button_set_shape(cast(w,CTK_button_get_type()),6);
 return w;
}
/* The value list, built like the stock one (FUN_001a1454): a framed window
 * over the page (made after it, so it is drawn on top). Its rows keep the
 * default (centred) text: with a text draw attribute (left-aligned, as the
 * stock lists) CTK_button_set_text text showed only on the first opening,
 * and the highlighted row's text moved. */
static int create_list(void){
 unsigned int i;char name[64];
 m.list=CTK_window_new();if(!m.list)return -1;
 gtk_widget_set_name(m.list,"ub820_list_window_CmnE_18");
 CTK_window_set_layer(cast(m.list,CTK_window_get_type()),m.overlay?3:2);
 m.list_fixed=CTK_fixed_new();if(!m.list_fixed)return -1;
 gtk_widget_set_name(m.list_fixed,"ub820_list_fixed_SWIN_C_18");
 CTK_fixed_set_shape(cast(m.list_fixed,CTK_fixed_get_type()),3);CTK_fixed_set_alpha(cast(m.list_fixed,CTK_fixed_get_type()),255);
 gtk_container_add(cast(m.list,gtk_container_get_type()),m.list_fixed);gtk_widget_show(m.list_fixed);
 for(i=0;i<DROP_ROWS;i++){
  Widget w=CTK_stock_button_new(1,"");if(!w)return -1;
  snprintf(name,sizeof name,"ub820_list_%u_SBTN_A_18",i);gtk_widget_set_name(w,name);
  gtk_fixed_put(cast(m.list_fixed,gtk_fixed_get_type()),w,LIST_PAD,LIST_PAD+(int)i*35);gtk_widget_set_uposition(w,LIST_PAD,LIST_PAD+(int)i*35);gtk_widget_set_usize(w,LIST_ROW_W,31);
  CTK_button_set_shape(cast(w,CTK_button_get_type()),6);
  m.drop[i]=w;
 }
 if(ub_register_window&&ub_register_window(m.list)!=0)return -1;
 return 0;
}
static int create_child(void){
 unsigned int i;char name[64];
 m.window=CTK_window_new();if(!m.window)return -1;
 gtk_widget_set_name(m.window,"ub820_extsub_window_CmnE_18");gtk_widget_set_uposition(m.window,PAGE_X,PAGE_Y);
 CTK_window_set_layer(cast(m.window,CTK_window_get_type()),m.overlay?3:2);  /* the overlay over everything */
 m.fixed=CTK_fixed_new();if(!m.fixed)goto bad;
 gtk_widget_set_name(m.fixed,"ub820_extsub_fixed_SWIN_C_18");gtk_widget_set_usize(m.fixed,PAGE_W,PAGE_H);
 CTK_fixed_set_shape(cast(m.fixed,CTK_fixed_get_type()),0);CTK_fixed_set_alpha(cast(m.fixed,CTK_fixed_get_type()),255);
 gtk_container_add(cast(m.window,gtk_container_get_type()),m.fixed);gtk_widget_show(m.fixed);
 m.title=label(m.fixed,"ub820_extsub_title_SLBL_A_18",10,4,630,48);
 m.info=label(m.fixed,"ub820_extsub_info_SLBL_B_16",10,54,630,34);
 m.footer=label(m.fixed,"ub820_extsub_help_SLBL_B_16",10,416,630,60);
 if(!m.title||!m.info||!m.footer)goto bad;
 for(i=0;i<VIEW_ROWS;i++){
  snprintf(name,sizeof name,"ub820_extsub_row_%u_SBTN_A_18",i);
  if(!(m.rows[i]=row_button(name,10,94+(int)i*35,630)))goto bad;
  gtk_widget_show(m.rows[i]);
 }
 if(ub_register_window&&ub_register_window(m.window)!=0)goto bad;
 m.active=1;m.first=m.selected=0;m.drop_kind=DROP_NONE;gtk_widget_show(m.window);
 render();return 0;
bad:close_child(0);return -1;
}
static void open_child(unsigned int kind){
 if(m.active)return;
 m.kind=kind;m.notice[0]=0;if(!brightness.active)m.pending=0;
 ha_load(&hs);
 if(kind==PAGE_SUBS)read_status();
 m.media_kind=*field(m.ctx,0x33f8);m.media_number=*field(m.ctx,0x33f4);m.deck_mode=*field(m.ctx,0x33e4);
 if(create_child()!=0)return;
 if(kind==PAGE_SUBS)send_request(UB_OP_LIST,m.status.dir[0]?m.status.dir:"/mnt/usb");
}
static void open_drop(unsigned int kind){
 m.drop_kind=kind;
 if(kind==DROP_SUBS){m.drop_count=2;m.drop_sel=m.status.enabled?0:1;}
 else if(kind==DROP_POWER){
  if(!hs.power_enabled||!hs.power_known){set_notice("Power control is not ready; reopen Helper to refresh");m.drop_kind=DROP_NONE;render();return;}
  m.drop_count=2;m.drop_sel=hs.power_choice?0:1;
 }
 else if(kind==DROP_CPU){
  if(!hs.cpu_enabled||!hs.cpu_known){ha_cpu_unavailable(&hs);m.drop_kind=DROP_NONE;render();return;}
  m.drop_count=2;m.drop_sel=hs.cpu_choice?0:1;
 }
 else if(kind==DROP_NETFLIX){m.drop_count=HA_NETFLIX_CHOICES;m.drop_sel=hs.netflix_choice<HA_NETFLIX_CHOICES?hs.netflix_choice:0;}
 else if(kind==DROP_DVD){m.drop_count=HA_DVD_CHOICES;m.drop_sel=hs.dvd_choice<HA_DVD_CHOICES?hs.dvd_choice:0;}
 else{
  if(!hs.region_known){set_notice("Blu-ray region needs the region module ([region] in modules.cfg)");m.drop_kind=DROP_NONE;render();return;}
  m.drop_count=ha_region_count(&hs);m.drop_sel=ha_region_index(hs.region_choice);
  if(m.drop_sel>=m.drop_count)m.drop_sel=0;
 }
 render();
}
/* OK in the list: only a different value is sent (and saved). */
static void take_drop(void){
 unsigned int kind=m.drop_kind,k=m.drop_sel;
 m.drop_kind=DROP_NONE;
 if(kind==DROP_SUBS){
  if((k==0)!=(m.status.enabled!=0))send_request(k==0?UB_OP_ON:UB_OP_OFF,"");
 }else if(kind==DROP_REGION)ha_region_set(&hs,k);
 else if(kind==DROP_POWER)ha_power_set(&hs,k?0:1);
 else if(kind==DROP_CPU)ha_cpu_set(&hs,k?0:1);
 else if(kind==DROP_NETFLIX)ha_netflix_set(&hs,k);
 else if(kind==DROP_DVD)ha_dvd_set(&hs,k);
 render();
}
static void choose_helper(void){
 unsigned int ids[HELPER_ROWS],c=helper_rows(ids);
 if(m.selected>=c)return;
 switch(ids[m.selected]){
 case H_REGION:open_drop(DROP_REGION);break;
 case H_DVD:open_drop(DROP_DVD);break;
 case H_NETFLIX:open_drop(DROP_NETFLIX);break;
 case H_POWER:open_drop(DROP_POWER);break;
 case H_CPU:open_drop(DROP_CPU);break;
 case H_SSH:ha_ssh_toggle(&hs);render();break;
 case H_LOG:load_log();m.log_view=1;render();break;
 default:ha_log_save(&hs);render();break;
 }
}
static void choose_row(void){
 char path[UB_PATH_BYTES];unsigned int n=m.selected;
 if(m.kind==PAGE_HELPER){choose_helper();return;}
 if(n==0){
  const char *dir=m.status.dir[0]?m.status.dir:"/mnt/usb";size_t len;
  ha_copy(path,dir,sizeof path);len=strlen(path);while(len>8&&path[len-1]=='/')path[--len]=0;
  while(len>8&&path[len-1]!='/')path[--len]=0;
  if(len>8)path[len-1]=0;
  if(strlen(path)<8)ha_copy(path,"/mnt/usb",sizeof path);
  send_request(UB_OP_LIST,path);
 }else if(n==1)open_drop(DROP_SUBS);
 else if(n==2)brightness_begin(0);
 else if(n==3)brightness_begin(1);
 else if(n==4)send_request(UB_OP_LIST,m.status.dir[0]?m.status.dir:"/mnt/usb");
 else if(n-SUB_ACTIONS<m.status.count){struct ub_item *it=&m.status.items[n-SUB_ACTIONS];send_request(it->kind==UB_KIND_DIR?UB_OP_LIST:UB_OP_SELECT,it->path);}
}

/* ---- OPTIONS root ----------------------------------------------------------- */
/* The tile images, written to /tmp once per process. */
static const struct { const char *name; const unsigned char *png; size_t size; } tile_files[4]={
 {"subs",ub_tile_subs_png,sizeof ub_tile_subs_png},{"subs_focus",ub_tile_subs_focus_png,sizeof ub_tile_subs_focus_png},
 {"helper",ub_tile_helper_png,sizeof ub_tile_helper_png},{"helper_focus",ub_tile_helper_focus_png,sizeof ub_tile_helper_focus_png}};
static int tile_ready(void){
 static int ready;unsigned int i,hd;char path[64];
 if(ready)return ready>0;
 ready=-1;
 for(i=0;i<4;i++)for(hd=0;hd<2;hd++){
  int fd,ok;
  snprintf(path,sizeof path,"%s%s%s.png",TILE_DIR,tile_files[i].name,hd?"_HD":"");
  fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd<0)return 0;
  ok=write(fd,tile_files[i].png,tile_files[i].size)==(ssize_t)tile_files[i].size;
  if(close(fd)||!ok)return 0;
 }
 ready=1;return 1;
}
static void present_root(void){
 unsigned int i;
 if(!m.ctx||!m.owner||!ub_lookup)return;
 for(i=0;i<m.count;i++){
  unsigned int idx=m.base+i;int on;Widget w;
  if(idx>=ROOT_SLOTS)break;
  w=ub_lookup(m.owner,root_names[idx]);if(!w)continue;
  on=*field(m.ctx,PAGE_OFF)==0&&*field(m.ctx,SELECT_OFF)==idx;
  /* The slot keeps the stock button (focus, input, geometry); only its
   * normal (0) and selected (2) images become ours, as the stock builder
   * sets N and F images (FUN_00187d48). */
  if(!m.tiled[i]&&tile_ready()){
   char path[64];const char *kind=m.kinds[i]==PAGE_HELPER?"helper":"subs";Widget b=cast(w,CTK_imagebutton_get_type());
   snprintf(path,sizeof path,"%s%s.png",TILE_DIR,kind);CTK_imagebutton_set_bgpixmap(b,0,path);
   snprintf(path,sizeof path,"%s%s_focus.png",TILE_DIR,kind);CTK_imagebutton_set_bgpixmap(b,2,path);
   m.tiled[i]=1;
  }
  gtk_widget_show(w);gtk_widget_set_sensitive(w,1);gtk_widget_set_state(w,on?2:0);
 }
}
/* Our categories moved or changed: their slots get their tiles again, and
 * icons that are no longer ours are hidden (the stock builder shows its own). */
static void drop_tiles(unsigned int total){
 unsigned int i;
 for(i=0;i<m.count;i++){
  unsigned int idx=m.base+i;
  if(idx>=total&&idx<ROOT_SLOTS&&ub_lookup&&m.owner){Widget old=ub_lookup(m.owner,root_names[idx]);if(old)gtk_widget_hide(old);}
 }
 memset(m.tiled,0,sizeof m.tiled);
}
unsigned int ub_hook_root_builder(int update){
 unsigned int ret,n,k,i,want,was=MAX_CUSTOM;void *ctx;Widget owner;struct descriptor *rows;int subs;
 if(ub_get_ctx){
  ctx=ub_get_ctx(0x27);
  if(ctx&&ctx==m.ctx&&root_window(ctx)==m.owner&&m.count){
   unsigned int s=*field(ctx,SELECT_OFF);
   if(s>=m.base&&s<m.base+m.count&&custom_descriptor(&((struct descriptor *)((char *)ctx+ROW_OFF))[s]))was=s-m.base;
  }
 }
 ret=ub_original_root_builder?ub_original_root_builder(update):0;
 if(!ub_get_ctx)return ret;
 ctx=ub_get_ctx(0x27);if(!ctx)return ret;owner=root_window(ctx);
 if(m.ctx!=ctx||m.owner!=owner){if(!m.overlay)close_child(0);memset(m.tiled,0,sizeof m.tiled);m.count=0;m.ctx=ctx;m.owner=owner;if(!brightness.active)m.pending=0;if(owner)gtk_signal_connect(owner,"destroy",owner_destroyed,0);}
 n=*field(ctx,COUNT_OFF);rows=(struct descriptor *)((char *)ctx+ROW_OFF);
 /* already appended (a nested build) */
 for(k=0;k<n&&k<m.count&&custom_descriptor(&rows[n-1-k]);k++){}
 if(k&&k==m.count&&n<=ROOT_SLOTS){m.base=n-k;present_root();return n;}
 subs=ha_exists(UB_EXTSUB_ENABLED);
 want=subs?2u:1u;
 if(n+want>ROOT_SLOTS)want=n<ROOT_SLOTS?ROOT_SLOTS-n:0;
 if(!want||!owner)return ret;
 if(m.base!=n||m.count!=want)drop_tiles(n+want);
 m.base=n;m.count=want;
 m.kinds[0]=subs?PAGE_SUBS:PAGE_HELPER;m.kinds[1]=PAGE_HELPER;
 for(i=0;i<want;i++){memset(&rows[n+i],0,sizeof rows[n+i]);rows[n+i].id=CUSTOM_ID;rows[n+i].state=3;rows[n+i].child=0;}
 *field(ctx,COUNT_OFF)=n+want;*field(ctx,ENABLED_OFF)+=want;
 if(m.active){for(i=0;i<want&&m.kinds[i]!=m.kind;i++){}if(i<want)was=i;}
 if(was<want)*field(ctx,SELECT_OFF)=n+was;
 else if(*field(ctx,SELECT_OFF)==0xff)*field(ctx,SELECT_OFF)=n;
 present_root();ensure_timer();return n+want;
}
static void sync_preview(void);
void ub_hook_refresh(void){
 if(m.refresh_guard){if(ub_original_refresh)ub_original_refresh();return;}
 m.refresh_guard=1;
 if(m.active&&!m.overlay&&!owner_alive())close_child(0);
 if(ub_original_refresh)ub_original_refresh();
 if(m.ctx&&ub_get_ctx&&ub_get_ctx(0x27)==m.ctx&&root_window(m.ctx)==m.owner){present_root();sync_preview();if(m.active&&!m.overlay){gtk_widget_show(m.window);render();}}
 m.refresh_guard=0;
}
/* The page of the focused root tile is shown without being entered, as the
 * stock categories show theirs; it goes when the focus leaves our tiles. */
static void sync_preview(void){
 unsigned int s,kind;
 if(m.overlay||!m.ctx||!m.owner||!ub_get_ctx||ub_get_ctx(0x27)!=m.ctx||root_window(m.ctx)!=m.owner)return;
 if(m.active&&!m.preview)return;
 s=*field(m.ctx,SELECT_OFF);
 if(*field(m.ctx,PAGE_OFF)!=0||!m.count||s<m.base||s>=m.base+m.count||!custom_descriptor(&((struct descriptor *)((char *)m.ctx+ROW_OFF))[s])){
  if(m.active)close_child(0);
  return;
 }
 kind=m.kinds[s-m.base];
 if(m.active&&m.kind==kind)return;
 if(m.active)close_child(0);
 m.preview=1;open_child(kind);
 if(!m.active)m.preview=0;
}
/* A key for the open page (OPTIONS or overlay). */
enum { PK_DONE, PK_CLOSE, PK_EXIT, PK_OTHER };
static int page_key(unsigned int key){
 if(key==KEY_EXIT)return PK_EXIT;
 if(m.log_view&&key<0x90){
  if(key==KEY_UP&&m.log_first)m.log_first--;
  else if(key==KEY_DOWN&&m.log_first+VIEW_ROWS<m.log_count)m.log_first++;
  else if(key==KEY_OK||key==KEY_BACK||key==KEY_LEFT||key==KEY_RIGHT)m.log_view=0;
  render();return PK_DONE;
 }
 if(m.drop_kind==DROP_BRIGHTNESS&&key<0x90){brightness_key(key);return PK_DONE;}
 if(m.drop_kind!=DROP_NONE&&key<0x90){
  if(key==KEY_UP)m.drop_sel=m.drop_sel?m.drop_sel-1:m.drop_count-1;
  else if(key==KEY_DOWN){m.drop_sel++;if(m.drop_sel>=m.drop_count)m.drop_sel=0;}
  else if(key==KEY_OK||key==KEY_RIGHT){take_drop();return PK_DONE;}
  else if(key==KEY_BACK||key==KEY_LEFT)m.drop_kind=DROP_NONE;
  render();return PK_DONE;
 }
 if(key==KEY_BACK||key==KEY_LEFT)return PK_CLOSE;
 if(key==KEY_UP){m.selected=m.selected?m.selected-1:total_rows()-1;render();return PK_DONE;}
 if(key==KEY_DOWN){m.selected++;if(m.selected>=total_rows())m.selected=0;render();return PK_DONE;}
 if(key==KEY_OK||key==KEY_RIGHT){choose_row();return PK_DONE;}
 return key>=0x90?PK_OTHER:PK_DONE;
}
/* The overlay: the Netflix key opens and closes it; while it is open every
 * press comes here (playback and other keys are ignored). It is not opened
 * over the OPTIONS page. */
static void overlay_key(unsigned int key){
 if(key==KEY_NETFLIX){
  if(m.active&&m.overlay)close_child(0);
  else if(!m.active){
   m.overlay=1;m.kind=PAGE_HELPER;m.notice[0]=0;if(!brightness.active)m.pending=0;ha_load(&hs);
   if(create_child()!=0){m.overlay=0;return;}
   if(overlay_renew())close_child(0);
  }
  return;
 }
 if(!m.active||!m.overlay)return;
 switch(page_key(key)){
 case PK_CLOSE:case PK_EXIT:close_child(0);break;
 default:break;
 }
}
static void ensure_timer(void){
 if(m.timer)return;
 unlink(UB_OVERLAY_OPEN);unlink(UB_OVERLAY_ABORT);  /* none is open in a new apl_dispsrv */
 m.timer=g_timeout_add(TIMER_MS,timer_tick,0);
}
/* apl_dispsrv's main message callback (gdk_input_add, UI thread): starts the
 * payload's timer the first time it runs. */
void ub_hook_input(void *data,int source,int condition){
 ensure_timer();
 if(ub_original_input)ub_original_input(data,source,condition);
}
int ub_hook_key_router(void){
 unsigned int key=ub_get_key?ub_get_key(ub_event):0;void *ctx=ub_get_ctx?ub_get_ctx(0x27):0;unsigned int s;
 int rc;
 if(m.active&&m.overlay)return ub_original_key_router?ub_original_key_router():0;
 if(m.active&&!m.preview){
  if(!owner_alive()){close_child(0);return ub_original_key_router?ub_original_key_router():0;}
  switch(page_key(key)){
  case PK_EXIT:close_child(1);return ub_original_key_router?ub_original_key_router():0;
  /* Return: the focus goes back to the tile, the page stays as its preview */
  case PK_CLOSE:m.preview=1;m.drop_kind=DROP_NONE;m.log_view=0;render();ub_hook_refresh();return 0;
  /* Playback/status keys still reach stock global handling after closing the
   * child. Unknown direction/repeat keys must never mutate hidden stock rows. */
  case PK_OTHER:close_child(1);rc=ub_original_key_router?ub_original_key_router():0;sync_preview();return rc;
  default:return 0;
  }
 }
 if(ctx&&ctx==m.ctx&&root_window(ctx)==m.owner&&*field(ctx,PAGE_OFF)==0&&(key==KEY_OK||key==KEY_RIGHT)){
  s=*field(ctx,SELECT_OFF);
  if(m.count&&s>=m.base&&s<m.base+m.count){
   unsigned int kind=m.kinds[s-m.base];
   if(m.active&&m.kind!=kind)close_child(0);
   if(!m.active)open_child(kind);
   if(m.active){m.preview=0;render();}
   return 0;
  }
 }
 rc=ub_original_key_router?ub_original_key_router():0;
 sync_preview();
 return rc;
}
