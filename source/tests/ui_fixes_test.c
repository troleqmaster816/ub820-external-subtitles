/* OPTIONS menu fixes (partial list, lost Cancel, ended engine, overlay focus,
 * tile retry, FIFO close-on-exec, log option, CTK text, hidden root). Real
 * menu code with the brightness fixture's files, clock and CTK stubs; tile
 * images go to a private folder. */
#define _GNU_SOURCE
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <locale.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include "log_mode_fixture.h"
static char tile_folder[64];
static unsigned tile_opens,tile_failures;
static int abort_after_consume;
static int ui_test_unlink(const char *path){
 int rc=unlink(path);
 if(abort_after_consume&&strstr(path,"abort")){
  abort_after_consume=0;int fd=open(path,O_WRONLY|O_CREAT,0600);assert(fd>=0);close(fd);
 }
 return rc;
}
static int ui_test_open(const char *path,int flags,...){
 char moved[128];mode_t mode=0;
 if(flags&O_CREAT){va_list a;va_start(a,flags);mode=(mode_t)va_arg(a,int);va_end(a);}
 if(!strncmp(path,"/tmp/ub820_tile_",16)){
  tile_opens++;if(tile_failures){tile_failures--;errno=ENOSPC;return -1;}
  snprintf(moved,sizeof moved,"%s/%s",tile_folder,path+5);path=moved;
 }
 return open(path,flags,mode);
}
#define open ui_test_open
#define unlink ui_test_unlink
#define main brightness_ui_fixture_main
#include "subtitle_brightness_ui_test.c"
#undef main
#undef open
#undef unlink

/* What gdk_mbstowcs needs: the whole string converts in a UTF-8 locale. */
static int converts(const char *text){return mbstowcs(0,text,0)!=(size_t)-1;}
static uint32_t context[0x3500/sizeof(uint32_t)];
static void *menu_context(int kind){return kind==0x27?context:0;}
static struct ub_status listing(unsigned seq,int result,unsigned count){
 struct ub_status s=m.status;s.seq=seq;s.result=result;s.count=count;s.brightness_edit=0;s.brightness_phase=UB_BRIGHTNESS_NONE;
 for(unsigned i=0;i<count;i++){s.items[i].kind=UB_KIND_FILE;snprintf(s.items[i].name,sizeof s.items[i].name,"f%02u.srt",i);snprintf(s.items[i].path,sizeof s.items[i].path,"/mnt/usb/f%02u.srt",i);}
 snprintf(s.message,sizeof s.message,"%s",result==UB_LIMIT?"Directory limited to64 entries; shorten long paths":"Choose a subtitle file");
 return s;
}
static void tick(void){m.ticks=POLL_TICKS-1;assert(timer_tick(0)==1);}
int main(void){
 char dir[]="/tmp/ub820-ui-fixes-XXXXXX",row[200],name[UB_NAME_BYTES];unsigned cases=0;struct ub_request r,p,q;struct ub_status st;
 assert(setlocale(LC_CTYPE,"C.UTF-8"));
 assert(mkdtemp(dir));snprintf(request_path,sizeof request_path,"%s/request",dir);snprintf(status_path,sizeof status_path,"%s/status",dir);snprintf(save_path,sizeof save_path,"%s/save",dir);snprintf(keys_path,sizeof keys_path,"%s/keys",dir);snprintf(abort_path,sizeof abort_path,"%s/abort",dir);
 snprintf(tile_folder,sizeof tile_folder,"%s",dir);
 /* A marker published after atomic consumption remains for the next tick. */
 reset(100,255);m.overlay=1;m.overlay_deadline=seconds+3;m.ticks=0;abort_after_consume=1;
 assert(timer_tick(0)==1&&m.overlay&&!abort_after_consume&&!access(abort_path,F_OK));
 assert(timer_tick(0)==1&&!m.overlay&&access(abort_path,F_OK));cases++;

 /* 1. After a timeout a matching partial LIST (UB_LIMIT) makes its files selectable and keeps the limit message. */
 reset(50,128);assert(!send_request(UB_OP_LIST,"/mnt/usb"));seconds+=12;assert(subtitle_pending_expire()&&m.needs_list);
 assert(!send_request(UB_OP_LIST,"/mnt/usb"));r=queued();
 st=listing(r.seq-1,UB_LIMIT,UB_MAX_ITEMS);put(status_path,&st,sizeof st);read_status();assert(m.pending==r.seq&&m.needs_list);
 st=listing(r.seq,UB_LIMIT,UB_MAX_ITEMS);put(status_path,&st,sizeof st);read_status();
 assert(!m.pending&&!m.needs_list&&!m.notice[0]&&strstr(widget(m.footer)->text,"limited")&&strstr(widget(m.info)->text,"First 64 entries"));
 m.selected=SUB_ACTIONS+1;choose_row();p=queued();assert(p.op==UB_OP_SELECT&&!strcmp(p.path,"/mnt/usb/f01.srt"));cases++;
 /* A UB_LIMIT answer to anything but LIST never lifts the refresh requirement. */
 seconds+=12;assert(subtitle_pending_expire()&&m.needs_list);m.pending=p.seq;m.pending_op=UB_OP_SELECT;
 st=listing(p.seq,UB_LIMIT,3);put(status_path,&st,sizeof st);read_status();assert(!m.pending&&m.needs_list);cases++;

 /* 2. Both Cancels lost (no status), page closed: the edit is closed after the resend times out; LIST works again. */
 reset(50,128);begin(50,128);brightness_key(KEY_BACK);r=queued();assert(r.op==UB_OP_BRIGHTNESS_CANCEL);
 assert(!unlink(status_path));close_child(0);assert(!m.active&&brightness.active);
 seconds+=12;tick();p=queued();assert(brightness.active&&p.op==UB_OP_BRIGHTNESS_CANCEL&&p.seq!=r.seq&&m.pending==p.seq);
 seconds+=12;tick();assert(!brightness.active&&!m.pending&&strstr(m.notice,"not confirmed"));
 assert(!send_request(UB_OP_LIST,"/mnt/usb"));q=queued();assert(q.op==UB_OP_LIST);cases++;
 /* A late OK to the old Cancel does not touch a new edit. */
 st=listing(q.seq,UB_OK,2);put(status_path,&st,sizeof st);read_status();assert(!m.needs_list);
 q=begin(50,128);assert(brightness.active&&brightness.edit==q.seq&&!brightness.op);
 response(r,UB_OK,50,128);response(p,UB_OK,50,128);assert(brightness.active&&brightness.edit==q.seq&&!brightness.cancel);
 brightness_key(KEY_RIGHT);q=queued();assert(q.op==UB_OP_BRIGHTNESS_PREVIEW&&q.brightness.value==51);cases++;
 /* The latest status shows no edit of ours: a timed-out Cancel closes the edit at once. */
 reset(50,128);begin(50,128);brightness_key(KEY_BACK);r=queued();
 st=m.status;st.seq=r.seq-1;st.brightness_edit=0;st.brightness_phase=UB_BRIGHTNESS_NONE;put(status_path,&st,sizeof st);read_status();
 seconds+=12;assert(subtitle_pending_expire()&&!brightness.active);brightness_pump();assert(queued().seq==r.seq);cases++;

 /* 3. The engine that published the status has ended (zombie, then reaped). */
 {int gate[2];pid_t child;uint64_t born=0;char byte;
  assert(!pipe(gate));child=fork();assert(child>=0);
  if(!child){close(gate[1]);if(read(gate[0],&byte,1)<0){}_exit(0);}
  close(gate[0]);
  for(unsigned i=0;i<200&&ub_br_birth((unsigned)child,&born);i++)usleep(1000);
  assert(born);
  reset(50,128);m.status.brightness_pid=(uint32_t)child;m.status.brightness_birth_lo=(uint32_t)born;m.status.brightness_birth_hi=(uint32_t)(born>>32);
  begin(50,128);brightness_key(KEY_RIGHT);r=queued();assert(r.op==UB_OP_BRIGHTNESS_PREVIEW&&m.pending==r.seq);
  st=m.status;st.seq=r.seq-1;put(status_path,&st,sizeof st);read_status();assert(brightness.active&&m.pending==r.seq);  /* alive */
  close(gate[1]);for(unsigned i=0;i<2000&&!status_publisher_gone(&st);i++)usleep(1000);   /* zombie */
  read_status();assert(!brightness.active&&!m.pending&&m.needs_list&&strstr(m.notice,"ended"));
  assert(!send_request(UB_OP_LIST,"/mnt/usb")&&m.pending);read_status();assert(!m.pending);
  assert(waitpid(child,0,0)==child);assert(status_publisher_gone(&st));
  assert(!send_request(UB_OP_LIST,"/mnt/usb")&&m.pending);read_status();assert(!m.pending&&strstr(m.notice,"ended"));
  /* An accepted save is the supervisor's: it keeps waiting for the save acknowledgement. */
  child=fork();assert(child>=0);if(!child){pause();_exit(0);}
  for(unsigned i=0;i<200&&(born=0,ub_br_birth((unsigned)child,&born));i++)usleep(1000);
  reset(80,204);m.status.brightness_pid=(uint32_t)child;m.status.brightness_birth_lo=(uint32_t)born;m.status.brightness_birth_hi=(uint32_t)(born>>32);
  begin(80,204);brightness_key(KEY_OK);r=queued();response(r,UB_OK,80,204);assert(brightness.saving);
  kill(child,SIGKILL);assert(waitpid(child,0,0)==child);read_status();assert(brightness.active&&brightness.saving&&strstr(m.notice,"ended"));
  save_ack(UB_BRIGHTNESS_SAVED);assert(!brightness.active&&strstr(m.notice,"saved"));
  cases++;}
 /* 12. COMMIT answered and the matching SAVED written before the first read,
  *     then the engine ended: the save result is reported, not lost. */
 {int gate[2];pid_t child;uint64_t born=0;char byte;
  assert(!pipe(gate));child=fork();assert(child>=0);
  if(!child){close(gate[1]);if(read(gate[0],&byte,1)<0){}_exit(0);}
  close(gate[0]);
  for(unsigned i=0;i<200&&ub_br_birth((unsigned)child,&born);i++)usleep(1000);
  assert(born);
  reset(80,204);m.status.brightness_pid=(uint32_t)child;m.status.brightness_birth_lo=(uint32_t)born;m.status.brightness_birth_hi=(uint32_t)(born>>32);
  begin(80,204);brightness_key(KEY_OK);r=queued();deferred_read=1;response(r,UB_OK,80,204);deferred_read=0;
  {struct ub_brightness_save_ack a=brightness.save;a.result=UB_BRIGHTNESS_SAVED;put(save_path,&a,sizeof a);}
  close(gate[1]);assert(waitpid(child,0,0)==child);read_status();
  assert(!brightness.active&&strstr(m.notice,"saved")&&m.needs_list);cases++;}
 /* 13. Both Cancels lost while the engine still previews that edit: the
  *     next status showing it takes the edit up again and sends Cancel. */
 {uint32_t edit;
  reset(50,128);begin(50,128);edit=brightness.edit;brightness_key(KEY_BACK);r=queued();assert(r.op==UB_OP_BRIGHTNESS_CANCEL);
  assert(!unlink(status_path));
  seconds+=12;tick();p=queued();assert(p.op==UB_OP_BRIGHTNESS_CANCEL);
  seconds+=12;tick();assert(!brightness.active&&orphan.edit==edit);
  assert(!send_request(UB_OP_LIST,"/mnt/usb"));q=queued();
  st=listing(q.seq,UB_OK,2);st.brightness_edit=edit;st.brightness_edit_epoch=orphan.epoch;st.brightness_phase=UB_BRIGHTNESS_PREVIEW;
  st.brightness_owner_pid=orphan.pid;st.brightness_owner_start_lo=orphan.birth_lo;st.brightness_owner_start_hi=orphan.birth_hi;st.brightness_percent=20;
  put(status_path,&st,sizeof st);read_status();
  assert(brightness.active&&brightness.cancel&&!orphan.edit);
  q=queued();assert(q.op==UB_OP_BRIGHTNESS_CANCEL&&q.brightness.edit==edit&&m.pending==q.seq);
  response(q,UB_OK,50,128);assert(!brightness.active&&strstr(m.notice,"restored"));
  /* A status without that edit ends the record. */
  reset(50,128);begin(50,128);brightness_key(KEY_BACK);r=queued();assert(!unlink(status_path));
  seconds+=12;tick();seconds+=12;tick();assert(orphan.edit);
  st=listing(m.seq,UB_OK,2);put(status_path,&st,sizeof st);read_status();assert(!orphan.edit&&!brightness.active);cases++;}
 /* Close the page before both CANCEL deadlines; normal timer polling alone
  * must recover a still-visible preview when the backend reports it again. */
 reset(100,255);begin(100,255);
 brightness.percent=20;brightness.edited=1;brightness_pump();response(queued(),UB_OK,20,51);st=m.status;
 brightness_key(KEY_BACK);close_child(0);assert(!unlink(status_path));
 seconds+=12;tick();seconds+=12;tick();assert(orphan.edit&&!brightness.active&&!m.active);
 put(status_path,&st,sizeof st);tick();q=queued();assert(q.op==UB_OP_BRIGHTNESS_CANCEL&&brightness.active);
 response(q,UB_OK,100,255);assert(!brightness.active&&m.status.brightness_percent==100);cases++;
 /* An accepted COMMIT belongs to the supervisor across engine replacement.
  * The new publisher may arrive before the supervisor writes the save ACK. */
 reset(80,204);begin(80,204);brightness_key(KEY_OK);response(queued(),UB_OK,80,204);assert(brightness.saving);
 st=listing(1,UB_OK,0);st.epoch++;st.brightness_token[0]^=1;put(status_path,&st,sizeof st);read_status();
 assert(brightness.active&&brightness.saving&&m.needs_list);
 save_ack(UB_BRIGHTNESS_SAVED);assert(!brightness.active&&strstr(m.notice,"saved")&&m.needs_list);cases++;
 /* The supervisor may finish before the UI reads either the previous
  * engine's COMMIT status or the replacement engine's first status. */
 reset(80,204);begin(80,204);brightness_key(KEY_OK);r=queued();deferred_read=1;response(r,UB_OK,80,204);deferred_read=0;
 {struct ub_brightness_save_ack a=brightness.save;a.result=UB_BRIGHTNESS_SAVED;put(save_path,&a,sizeof a);}
 st=listing(1,UB_OK,0);st.epoch++;st.brightness_token[0]^=1;put(status_path,&st,sizeof st);read_status();
 assert(!brightness.active&&strstr(m.notice,"saved")&&m.needs_list);cases++;
 /* A transient renderer failure after closing the brightness editor leaves
  * the matching preview open. Restore it once the renderer accepts CANCEL. */
 reset(100,255);begin(100,255);
 brightness.percent=20;brightness.edited=1;brightness_pump();response(queued(),UB_OK,20,51);
 brightness_key(KEY_BACK);r=queued();close_child(0);response(r,UB_IO_ERROR,20,51);
 q=queued();assert(q.seq!=r.seq&&q.op==UB_OP_BRIGHTNESS_CANCEL&&q.brightness.edit==r.brightness.edit);
 response(q,UB_OK,100,255);assert(!brightness.active&&!m.pending&&m.status.brightness_percent==100);cases++;
 /* 14. The normal key path never shows a preview under a hidden root. */
 {struct descriptor *d;
  memset(&m,0,sizeof m);memset(context,0,sizeof context);
  *field(context,ROOT_WINDOW_OFF)=1;*field(context,SHOWN_OFF)=0;*field(context,PAGE_OFF)=0;
  *field(context,SELECT_OFF)=0;m.base=0;m.count=1;m.kinds[0]=PAGE_HELPER;
  m.ctx=context;m.owner=root_window(context);ub_get_ctx=menu_context;
  d=(struct descriptor *)((char *)context+ROW_OFF);d->id=CUSTOM_ID;d->child=0;
  ub_hook_key_router();assert(!m.active&&!m.preview);
  *field(context,SHOWN_OFF)=1;ub_hook_key_router();assert(m.active&&m.preview);close_child(0);
  ub_get_ctx=0;memset(&m,0,sizeof m);cases++;}
 /* A status without start time cannot be checked and is used as before. */
 st=m.status;st.brightness_birth_lo=st.brightness_birth_hi=0;assert(!status_publisher_gone(&st));
 st.brightness_pid=(uint32_t)getpid();{uint64_t born=0;assert(!ub_br_birth((unsigned)getpid(),&born));st.brightness_birth_lo=(uint32_t)born+1;st.brightness_birth_hi=(uint32_t)(born>>32);}
 assert(status_publisher_gone(&st));cases++;  /* same pid, other process start */

 /* 8. The key FIFO's read end is closed on exec. */
 memset(&m,0,sizeof m);assert(!mkfifo(keys_path,0600));read_keys(0);
 assert(m.keys_open&&(fcntl(m.keys_fd,F_GETFD)&FD_CLOEXEC));close(m.keys_fd);m.keys_open=0;cases++;

 /* 11. A root hidden by the native "hide all panels" path (bit 0 of ctx+0x5b4 clear) is not a live owner. */
 memset(&m,0,sizeof m);memset(context,0,sizeof context);
 *field(context,ROOT_WINDOW_OFF)=1;*field(context,COUNT_OFF)=2;*field(context,SELECT_OFF)=0;
 m.ctx=context;m.owner=root_window(context);ub_get_ctx=menu_context;
 *field(context,SHOWN_OFF)=1;assert(owner_alive());*field(context,SHOWN_OFF)=3;assert(owner_alive());
 *field(context,SHOWN_OFF)=2;assert(!owner_alive());*field(context,SHOWN_OFF)=1;
 *field(context,0x33e4)=1;assert(!owner_alive());*field(context,0x33e4)=0;cases++;

 /* 6. Building OPTIONS while the overlay is open leaves the stock focus alone. */
 m.overlay=1;m.active=1;m.kind=PAGE_HELPER;ub_hook_root_builder(0);
 assert(*field(context,COUNT_OFF)>2&&*field(context,SELECT_OFF)==0);
 m.overlay=0;memset(&m,0,sizeof m);*field(context,COUNT_OFF)=2;*field(context,SELECT_OFF)=0;
 m.ctx=context;m.owner=root_window(context);m.active=1;m.kind=PAGE_HELPER;ub_hook_root_builder(0);
 assert(m.kinds[*field(context,SELECT_OFF)-m.base]==PAGE_HELPER);cases++;  /* an open OPTIONS page still keeps its tile */
 m.active=0;ub_get_ctx=0;

 /* 7. A failed tile write is retried on a later build (bounded, TILE_TRIES). */
 tile_failures=2;tile_opens=0;
 assert(!tile_ready()&&tile_opens==1);assert(!tile_ready()&&tile_opens==2);
 assert(tile_ready()&&tile_opens==10);assert(tile_ready()&&tile_opens==10);cases++;

 /* 9. "Save log to USB" is offered only while the log module's mode file exists. */
 {unsigned ids[HELPER_ROWS],n;memset(&hs,0,sizeof hs);ha_load(&hs);
  n=helper_rows(ids);assert(!hs.log_off&&ids[n-1]==H_SAVE);
  assert(!unlink(log_mode_path));assert(ha_log_poll(&hs)&&hs.log_off);
  n=helper_rows(ids);assert(ids[n-1]==H_LOG);
  ha_log_save(&hs);assert(!hs.save_seq&&strstr(hs.notice,"log module is off"));
  ha_load(&hs);assert(hs.log_off&&helper_rows(ids)==n);cases++;}

 /* 10. CTK text: complete valid UTF-8 only, at most HA_TEXT_CHARS characters. */
 assert(!strcmp(ha_display(row,sizeof row,"abc\xc3",HA_TEXT_CHARS),"abc?"));
 assert(!strcmp(ha_display(row,sizeof row,"\xc3\xa9t\xc3\xa9",HA_TEXT_CHARS),"\xc3\xa9t\xc3\xa9"));
 assert(!strcmp(ha_display(row,sizeof row,"a\xe2\x82",HA_TEXT_CHARS),"a??"));
 assert(!strcmp(ha_display(row,sizeof row,"\xed\xa0\x80|\xc0\xaf|\xf4\x90\x80\x80|\xf0\x9f\x98\x80",HA_TEXT_CHARS),"???|??|????|\xf0\x9f\x98\x80"));
 assert(!strcmp(ha_display(row,sizeof row,"a\tb\x7f",HA_TEXT_CHARS),"a b "));
 {char line[600];memset(line,'x',sizeof line-1);line[sizeof line-1]=0;
  ha_display(row,sizeof row,line,HA_TEXT_CHARS);assert(strlen(row)==HA_TEXT_CHARS+3&&!strcmp(row+HA_TEXT_CHARS,"..."));
  for(unsigned i=0;i+2<sizeof line;i+=2){line[i]='\xc3';line[i+1]='\xa9';}line[sizeof line-1]=0;
  ha_display(row,40,line,HA_TEXT_CHARS);assert(strlen(row)<40&&converts(row)&&strstr(row,"..."));
  /* Log lines (up to ~510 bytes) cut inside a character reach CTK as valid, bounded text. */
  reset(50,128);memset(line,'y',sizeof line);line[500]='\xe2';line[501]='\x82';line[502]=0;memcpy(line,"z\xc3",2);
  m.log_lines[0]=line;m.log_count=1;m.log_view=1;render();
  assert(converts(widget(m.rows[0])->text)&&strlen(widget(m.rows[0])->text)<=HA_TEXT_CHARS+3&&!strncmp(widget(m.rows[0])->text,"z?",2));
  m.log_view=0;m.log_count=0;}
 /* Invalid raw path bytes are only sanitized for display. */
 reset(50,128);strcpy(m.status.dir,"/mnt/usb/Caf\xe9");strcpy(m.status.current,"/mnt/usb/sub\xe9.srt");
 render();assert(converts(widget(m.title)->text)&&converts(widget(m.info)->text));
 assert(strstr(widget(m.title)->text,"Caf?")&&strstr(widget(m.info)->text,"sub?.srt"));
 assert((unsigned char)m.status.dir[strlen(m.status.dir)-1]==0xe9);cases++;
 /* A file name the backend cut inside a character. */
 reset(50,128);memset(name,'n',sizeof name);memcpy(name+UB_NAME_BYTES-3,"\xe2\x82",2);name[UB_NAME_BYTES-1]=0;
 m.status.count=1;m.status.items[0].kind=UB_KIND_FILE;memcpy(m.status.items[0].name,name,sizeof name);strcpy(m.status.items[0].path,"/mnt/usb/n.srt");
 render();assert(converts(widget(m.rows[SUB_ACTIONS])->text)&&strstr(widget(m.rows[SUB_ACTIONS])->text,"..."));
 m.status.items[0].name[4]=0;m.status.items[0].name[3]='\xc3';render();assert(!strcmp(widget(m.rows[SUB_ACTIONS])->text,"nnn?"));cases++;

 close_child(0);unlink(request_path);unlink(status_path);unlink(save_path);unlink(keys_path);unlink(abort_path);
 for(unsigned i=0;i<4;i++)for(unsigned hd=0;hd<2;hd++){char path[160];snprintf(path,sizeof path,"%s/ub820_tile_%s%s.png",dir,tile_files[i].name,hd?"_HD":"");unlink(path);}
 assert(!rmdir(dir));
 printf("ui_fixes_test passed (%u scenarios)\n",cases);return 0;
}
