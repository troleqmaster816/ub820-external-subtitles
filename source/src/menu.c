/* UB820 1.82 native OPTIONS extension. Installed hooks run ONLY on the normal
 * display UI thread. This file is a freestanding PIC DSO, never a standalone
 * player executable. Loader bindings and originals are described in ui_hooks.json.
 */
#include "protocol.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

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

#define PAGE_OFF 0x490u
#define SELECT_OFF 0x494u
#define COUNT_OFF 0x6d4u
#define ENABLED_OFF 0x644u
#define ROOT_WINDOW_OFF 0x400u
#define ROW_OFF 0x764u
/* Use a valid root-category identity for unhooked native keyguide tables.
 * Distinguish our row by (id0,child0), owner and compacted index; the native
 * Operating Menu category is (id0,child1). Never leak a private numeric ID or
 * negative child into stock initializer/preview helpers. */
#define CUSTOM_ID 0u
#define VIEW_ROWS 9u
#define ACTION_ROWS 5u
#define KEY_EXIT 0x80u
#define KEY_BACK 0x81u
#define KEY_OK 0x82u
#define KEY_UP 0x85u
#define KEY_DOWN 0x86u
#define KEY_LEFT 0x87u
#define KEY_RIGHT 0x88u
struct descriptor { uint32_t id,type,label,state; int32_t child; };
static int custom_descriptor(const struct descriptor *d){return d->id==CUSTOM_ID&&d->child==0;}
struct menu_state {
 void *ctx; Widget owner,window,fixed,title,info,footer,rows[VIEW_ROWS],caption;
 unsigned int index,selected,first,timer,seq,pending,pending_ticks,epoch,needs_list,refresh_guard;
 uint32_t media_kind,media_number,deck_mode;
 int active; struct ub_status status; char notice[128];
};
static struct menu_state m;
static struct ub_status incoming;
static const char *const root_names[6]={"smngbr_ibtn_01","smngbr_ibtn_02","smngbr_ibtn_03","smngbr_ibtn_04","smngbr_ibtn_05","smngbr_ibtn_06"};
static uint32_t *field(void *p,unsigned int off){return (uint32_t *)((char *)p+off);}
static Widget root_window(void *p){return (Widget)(uintptr_t)*field(p,ROOT_WINDOW_OFF);}
static Widget cast(Widget w,GType type){return gtk_type_check_object_cast(w,type);}
static void copy_string(char *out,const char *in,size_t n){size_t i=0;if(!n)return;while(i+1<n&&in&&in[i]){out[i]=in[i];i++;}out[i]=0;}
static int bounded_path(const char *p){return p&&p[0]=='/'&&strnlen(p,UB_PATH_BYTES)<=UB_PATH_LIMIT;}
static void unregister_window(Widget w){unsigned int i;if(ub_window_registry)for(i=0;i<100;i++)if(ub_window_registry[i]==(uintptr_t)w)ub_window_registry[i]=0;}
static void close_child(int restore){
 Widget w=m.window;m.window=0;m.active=0;
 if(w){unregister_window(w);gtk_widget_destroy(w);}
 m.fixed=m.title=m.info=m.footer=0;memset(m.rows,0,sizeof m.rows);
 if(restore&&ub_original_refresh&&m.ctx&&root_window(m.ctx)==m.owner)ub_original_refresh();
}
static unsigned int total_rows(void){return ACTION_ROWS+m.status.count;}
static const char *row_text(unsigned int n,char *buf,size_t cap){
 size_t i;
 static const char *const actions[ACTION_ROWS]={"[..] Up one folder","Off","On","Reload current subtitle","Refresh files"};
 if(n<ACTION_ROWS)return actions[n];
 n-=ACTION_ROWS;if(n>=m.status.count)return "";
 snprintf(buf,cap,"%s%s",m.status.items[n].kind==UB_KIND_DIR?"[Folder] ":"",m.status.items[n].name);
 for(i=0;buf[i];i++)if((unsigned char)buf[i]<32||buf[i]==127)buf[i]=' ';
 return buf;
}
static void render(void){
 unsigned int i,total=total_rows();char text[512],row[160];
 if(!m.active||!m.window)return;
 if(m.selected>=total)m.selected=total?total-1:0;
 if(m.selected<m.first)m.first=m.selected;
 if(m.selected>=m.first+VIEW_ROWS)m.first=m.selected-VIEW_ROWS+1;
 snprintf(text,sizeof text,"External Subtitles\n%s",m.status.dir[0]?m.status.dir:"/mnt/usb");
 gtk_label_set_text(cast(m.title,gtk_label_get_type()),text);
 snprintf(text,sizeof text,"%s | %s%s%s",m.status.enabled?"On":"Off",m.status.current[0]?m.status.current:"No subtitle selected",m.pending?" | Waiting...":"",m.status.count==UB_MAX_ITEMS?" | First 64 entries":"");
 gtk_label_set_text(cast(m.info,gtk_label_get_type()),text);
 for(i=0;i<VIEW_ROWS;i++){
  unsigned int n=m.first+i;
  if(n<total){CTK_button_set_text(cast(m.rows[i],CTK_button_get_type()),row_text(n,row,sizeof row));gtk_widget_show(m.rows[i]);gtk_widget_set_sensitive(m.rows[i],1);gtk_widget_set_state(m.rows[i],n==m.selected?2:0);}
  else gtk_widget_hide(m.rows[i]);
 }
 snprintf(text,sizeof text,"%s\nUp/Down: choose   OK: select   Return: back   OPTIONS: close",m.notice[0]?m.notice:m.status.message);
 gtk_label_set_text(cast(m.footer,gtk_label_get_type()),text);
}
static int send_request(unsigned int op,const char *path){
 struct ub_request r;char temp[512];int fd;size_t at=0;ssize_t n;
 if(m.pending){copy_string(m.notice,"A request is already running",sizeof m.notice);render();return -1;}
 if(op!=UB_OP_LIST&&(m.needs_list||!m.epoch)){copy_string(m.notice,"Refresh files before selecting",sizeof m.notice);render();return -1;}
 if(path&&path[0]&&!bounded_path(path)){copy_string(m.notice,"Path is too long or invalid",sizeof m.notice);render();return -1;}
 memset(&r,0,sizeof r);r.magic=UB_MAGIC;r.version=UB_VERSION;r.seq=++m.seq;
 if(!r.seq)r.seq=++m.seq;
 r.epoch=op==UB_OP_LIST?0:m.epoch;r.op=op;copy_string(r.path,path,sizeof r.path);
 snprintf(temp,sizeof temp,"%s.ui-%ld",UB_REQUEST_FILE,(long)getpid());
 fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)goto bad;
 while(at<sizeof r){n=write(fd,(char *)&r+at,sizeof r-at);if(n<=0){close(fd);unlink(temp);goto bad;}at+=(size_t)n;}
 if(close(fd)!=0){unlink(temp);goto bad;}
 if(rename(temp,UB_REQUEST_FILE)!=0){unlink(temp);goto bad;}
 m.pending=r.seq;m.pending_ticks=0;m.notice[0]=0;render();return 0;
bad:copy_string(m.notice,"Cannot contact subtitle helper",sizeof m.notice);render();return -1;
}
static int status_valid(struct ub_status *s){
 unsigned int i;if(s->magic!=UB_MAGIC||s->version!=UB_VERSION||s->count>UB_MAX_ITEMS||!s->epoch)return 0;
 if(!memchr(s->dir,0,sizeof s->dir)||!memchr(s->current,0,sizeof s->current)||!memchr(s->message,0,sizeof s->message))return 0;
 for(i=0;i<s->count;i++)if((s->items[i].kind!=UB_KIND_FILE&&s->items[i].kind!=UB_KIND_DIR)||!memchr(s->items[i].name,0,UB_NAME_BYTES)||!memchr(s->items[i].path,0,UB_PATH_BYTES)||!bounded_path(s->items[i].path))return 0;
 return 1;
}
static void read_status(void){
 int fd;ssize_t n;size_t at=0;char extra;
 fd=open(UB_STATUS_FILE,O_RDONLY);if(fd<0)return;
 while(at<sizeof incoming){n=read(fd,(char *)&incoming+at,sizeof incoming-at);if(n<=0)break;at+=(size_t)n;}
 n=read(fd,&extra,1);close(fd);if(at!=sizeof incoming||n!=0||!status_valid(&incoming))return;
 /* Generation changes invalidate pending mutations. A fresh LIST is the only
  * epoch-independent request; never replay SELECT/ON/OFF into a new backend. */
 if(m.epoch&&m.epoch!=incoming.epoch){m.pending=0;m.needs_list=1;copy_string(m.notice,"Helper restarted; refresh files before selecting",sizeof m.notice);}
 m.epoch=incoming.epoch;
 if(incoming.seq>m.seq)m.seq=incoming.seq;
 if(m.pending&&incoming.seq==m.pending){m.pending=0;if(incoming.result==UB_OK)m.needs_list=0;}
 memcpy(&m.status,&incoming,sizeof m.status);render();
}
static int owner_alive(void){
 void *ctx=ub_get_ctx?ub_get_ctx(0x27):0;
 return ctx&&ctx==m.ctx&&m.owner&&root_window(ctx)==m.owner&&*field(ctx,PAGE_OFF)==0&&*field(ctx,0x33f8)==m.media_kind&&*field(ctx,0x33f4)==m.media_number&&*field(ctx,0x33e4)==m.deck_mode;
}
static int timer_tick(void *unused){(void)unused;if(m.active){if(!owner_alive())close_child(0);else{read_status();if(m.pending&&++m.pending_ticks>=40){m.pending=0;m.needs_list=1;copy_string(m.notice,"Helper timed out; refresh files to retry",sizeof m.notice);render();}}}return 1;}
static void owner_destroyed(Widget w,void *unused){(void)unused;if(w==m.owner){close_child(0);m.owner=m.caption=0;m.pending=0;}}
static Widget label(Widget fixed,const char *name,int x,int y,int width,int height){
 Widget w=CTK_label_new("");if(!w)return 0;gtk_widget_set_name(w,name);gtk_fixed_put(cast(fixed,gtk_fixed_get_type()),w,x,y);gtk_widget_set_uposition(w,x,y);gtk_widget_set_usize(w,width,height);gtk_label_set_justify(cast(w,gtk_label_get_type()),0);gtk_widget_show(w);return w;
}
static int create_child(void){
 unsigned int i;char name[64];
 m.window=CTK_window_new();if(!m.window)return -1;
 gtk_widget_set_name(m.window,"ub820_extsub_window_CmnE_18");gtk_widget_set_uposition(m.window,153,37);
 CTK_window_set_layer(cast(m.window,CTK_window_get_type()),2);
 m.fixed=CTK_fixed_new();if(!m.fixed)goto bad;
 gtk_widget_set_name(m.fixed,"ub820_extsub_fixed_SWIN_C_18");gtk_widget_set_usize(m.fixed,650,480);
 CTK_fixed_set_shape(cast(m.fixed,CTK_fixed_get_type()),0);CTK_fixed_set_alpha(cast(m.fixed,CTK_fixed_get_type()),255);
 gtk_container_add(cast(m.window,gtk_container_get_type()),m.fixed);gtk_widget_show(m.fixed);
 m.title=label(m.fixed,"ub820_extsub_title_SLBL_A_18",10,4,630,48);
 m.info=label(m.fixed,"ub820_extsub_info_SLBL_B_16",10,54,630,34);
 m.footer=label(m.fixed,"ub820_extsub_help_SLBL_B_16",10,416,630,60);
 if(!m.title||!m.info||!m.footer)goto bad;
 for(i=0;i<VIEW_ROWS;i++){
  Widget w=CTK_stock_button_new(1,"");if(!w)goto bad;m.rows[i]=w;
  snprintf(name,sizeof name,"ub820_extsub_row_%u_SBTN_A_18",i);gtk_widget_set_name(w,name);
  gtk_fixed_put(cast(m.fixed,gtk_fixed_get_type()),w,10,94+(int)i*35);gtk_widget_set_uposition(w,10,94+(int)i*35);gtk_widget_set_usize(w,630,31);CTK_button_set_shape(cast(w,CTK_button_get_type()),6);gtk_widget_show(w);
 }
 if(ub_register_window&&ub_register_window(m.window)!=0)goto bad;
 m.active=1;m.first=m.selected=0;gtk_widget_show(m.window);render();return 0;
bad:close_child(0);return -1;
}
static void open_child(void){
 if(m.active)return;
 m.notice[0]=0;m.pending=0;read_status();
 m.media_kind=*field(m.ctx,0x33f8);m.media_number=*field(m.ctx,0x33f4);m.deck_mode=*field(m.ctx,0x33e4);
 if(create_child()!=0)return;
 send_request(UB_OP_LIST,m.status.dir[0]?m.status.dir:"/mnt/usb");
}
static void choose_row(void){
 char path[UB_PATH_BYTES];unsigned int n=m.selected;
 if(n==0){
  const char *dir=m.status.dir[0]?m.status.dir:"/mnt/usb";size_t len;
  copy_string(path,dir,sizeof path);len=strlen(path);while(len>8&&path[len-1]=='/')path[--len]=0;
  while(len>8&&path[len-1]!='/')path[--len]=0;
  if(len>8)path[len-1]=0;
  if(strlen(path)<8)copy_string(path,"/mnt/usb",sizeof path);
  send_request(UB_OP_LIST,path);
 }else if(n==1)send_request(UB_OP_OFF,"");
 else if(n==2)send_request(UB_OP_ON,"");
 else if(n==3)send_request(UB_OP_RELOAD,"");
 else if(n==4)send_request(UB_OP_LIST,m.status.dir[0]?m.status.dir:"/mnt/usb");
 else if(n-ACTION_ROWS<m.status.count){struct ub_item *it=&m.status.items[n-ACTION_ROWS];send_request(it->kind==UB_KIND_DIR?UB_OP_LIST:UB_OP_SELECT,it->path);}
}
static void present_root(void){
 Widget w,fixed;int y;
 if(!m.ctx||!m.owner||m.index>=6||!ub_lookup)return;
 w=ub_lookup(m.owner,root_names[m.index]);if(!w)return;
 gtk_widget_show(w);gtk_widget_set_sensitive(w,1);
 gtk_widget_set_state(w,*field(m.ctx,PAGE_OFF)==0&&*field(m.ctx,SELECT_OFF)==m.index?2:0);
 /* Root icons are81x60. Add a native multiline label over the spare icon,
  * retaining the original button's focus/highlight/input geometry. */
 if(!m.caption){fixed=ub_lookup(m.owner,"menu_fixed_submenu");if(fixed){y=34+(int)m.index*66;m.caption=label(fixed,"ub820_extsub_root_SLBL_A_16",66,y+7,88,46);}}
 if(m.caption){gtk_label_set_text(cast(m.caption,gtk_label_get_type()),"External\nSubtitles");gtk_widget_show(m.caption);gtk_widget_set_state(m.caption,*field(m.ctx,PAGE_OFF)==0&&*field(m.ctx,SELECT_OFF)==m.index?2:0);}
}
unsigned int ub_hook_root_builder(int update){
 unsigned int ret,n,was_custom=0;void *ctx;Widget owner;struct descriptor *rows;
 if(ub_get_ctx){ctx=ub_get_ctx(0x27);if(ctx&&ctx==m.ctx&&root_window(ctx)==m.owner&&*field(ctx,SELECT_OFF)==m.index&&custom_descriptor(&((struct descriptor *)((char *)ctx+ROW_OFF))[m.index]))was_custom=1;}
 ret=ub_original_root_builder?ub_original_root_builder(update):0;
 if(!ub_get_ctx)return ret;
 ctx=ub_get_ctx(0x27);if(!ctx)return ret;owner=root_window(ctx);
 if(m.ctx!=ctx||m.owner!=owner){close_child(0);m.caption=0;m.ctx=ctx;m.owner=owner;m.pending=0;if(owner)gtk_signal_connect(owner,"destroy",owner_destroyed,0);}
 n=*field(ctx,COUNT_OFF);rows=(struct descriptor *)((char *)ctx+ROW_OFF);
 if(n&&n<=5&&custom_descriptor(&rows[n-1])){m.index=n-1;present_root();return n;}
 if(n>4||!owner)return ret;
 if(m.caption&&m.index!=n){
  if(m.index>n&&ub_lookup){Widget old=ub_lookup(m.owner,root_names[m.index]);if(old)gtk_widget_hide(old);}
  gtk_widget_destroy(m.caption);m.caption=0;
 }
 m.index=n;memset(&rows[n],0,sizeof rows[n]);rows[n].id=CUSTOM_ID;rows[n].state=3;rows[n].child=0;
 *field(ctx,COUNT_OFF)=n+1;*field(ctx,ENABLED_OFF)+=1;
 if(was_custom||m.active||*field(ctx,SELECT_OFF)==0xff)*field(ctx,SELECT_OFF)=n;
 present_root();if(!m.timer)m.timer=g_timeout_add(250,timer_tick,0);return n+1;
}
void ub_hook_refresh(void){
 if(m.refresh_guard){if(ub_original_refresh)ub_original_refresh();return;}
 m.refresh_guard=1;
 if(m.active&&!owner_alive())close_child(0);
 if(ub_original_refresh)ub_original_refresh();
 if(m.ctx&&ub_get_ctx&&ub_get_ctx(0x27)==m.ctx&&root_window(m.ctx)==m.owner){present_root();if(m.active){gtk_widget_show(m.window);render();}}
 m.refresh_guard=0;
}
int ub_hook_key_router(void){
 unsigned int key=ub_get_key?ub_get_key(ub_event):0;void *ctx=ub_get_ctx?ub_get_ctx(0x27):0;
 if(m.active){
  if(!owner_alive()){close_child(0);return ub_original_key_router?ub_original_key_router():0;}
  if(key==KEY_EXIT){close_child(1);return ub_original_key_router?ub_original_key_router():0;}
  if(key==KEY_BACK||key==KEY_LEFT){close_child(1);present_root();return 0;}
  if(key==KEY_UP){m.selected=m.selected?m.selected-1:total_rows()-1;render();return 0;}
  if(key==KEY_DOWN){m.selected++;if(m.selected>=total_rows())m.selected=0;render();return 0;}
  if(key==KEY_OK||key==KEY_RIGHT){choose_row();return 0;}
  /* Playback/status keys still reach stock global handling after closing the
   * child. Unknown direction/repeat keys must never mutate hidden stock rows. */
  if(key>=0x90){close_child(1);return ub_original_key_router?ub_original_key_router():0;}
  return 0;
 }
 if(ctx&&ctx==m.ctx&&root_window(ctx)==m.owner&&*field(ctx,PAGE_OFF)==0&&*field(ctx,SELECT_OFF)==m.index&&(key==KEY_OK||key==KEY_RIGHT)){open_child();return 0;}
 return ub_original_key_router?ub_original_key_router():0;
}
