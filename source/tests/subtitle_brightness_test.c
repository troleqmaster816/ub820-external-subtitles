/* Real backend/protocol with own native callbacks and private RAM fixtures. */
#define _GNU_SOURCE
static char br_seed_path[160],br_ack_path[160],br_claim_path[160],br_notice_path[160],br_status_path[160],br_request_path[160];
#define UB_BRIGHTNESS_SEED br_seed_path
#define UB_BRIGHTNESS_SAVE_ACK br_ack_path
#define UB_BRIGHTNESS_SAVE_CLAIM br_claim_path
#define UB_BRIGHTNESS_COMMIT_NOTICE br_notice_path
#include "../modules/external_subtitles/protocol.h"
#include "../modules/external_subtitles/brightness_io.h"
static int notice_write_failure;
static int fixture_notice_write(const char *path,const void *data,size_t size){
 if(!strcmp(path,br_notice_path)&&notice_write_failure){errno=EIO;return -1;}
 return ub_br_private_write(path,data,size);
}
#define ub_br_private_write fixture_notice_write
#undef UB_STATUS_FILE
#undef UB_REQUEST_FILE
#define UB_STATUS_FILE br_status_path
#define UB_REQUEST_FILE br_request_path
int brightness_test_usleep(unsigned);
#define usleep brightness_test_usleep
#define main previous_luminance_fixture_main
#include "subtitle_luminance_backend_test.c"
#undef main
#undef usleep
#undef ub_br_private_write
#include <sys/wait.h>

static int stop_after_startup;
int brightness_test_usleep(unsigned value){
 if(value==200000&&stop_after_startup)pthread_exit(0);
 struct timespec delay={value/1000000u,(long)(value%1000000u)*1000};return nanosleep(&delay,0);
}
static int native_draw_unchanged(uint32_t seconds){(void)seconds;return 0;}
static uint64_t self_birth;
static void seed(unsigned percent,int corrupt){
 struct ub_brightness_seed s={0};s.magic=UB_BRIGHTNESS_SEED_MAGIC;s.version=UB_BRIGHTNESS_PROTOCOL;
 s.pid=(uint32_t)getpid();s.birth_lo=(uint32_t)self_birth;s.birth_hi=(uint32_t)(self_birth>>32);s.percent=percent;
 memset(s.token,0x42,16);if(corrupt)s.magic++;
 assert(!ub_br_private_write(br_seed_path,&s,sizeof s));
}
static void reset_fixture(unsigned percent){
 struct ub_api a={0};seed(percent,0);unlink(br_ack_path);unlink(br_claim_path);unlink(br_notice_path);notice_write_failure=0;
 fg_calls=render_calls=present_calls=open_calls=close_calls=update_calls=start_calls=seq=0;
 fg_failure=render_failures=in_close=open_error=0;active_value=parser_value=display_value=mapped_value=1;
 gc.foreground=0xffffffff;gc.background=0xff000000;gc_value=&gc;ti_value=&ti;dirty_value=seek_value=0;
 a.start=start;a.dirty=dirty;a.seek=seek;a.update=update;a.set_attr=set_attr;a.set_display=set_display;
 a.active=&active_value;a.parser=&parser_value;a.display=&display_value;a.clock=&clock_value;
 a.attrs=attrs;a.loaded=&loaded_value;a.mutex=&mutex;a.set_foreground=foreground;a.ti_display=&ti_value;a.text_gc=&gc_value;
 ub_test_bind(&a);ub_test_reset();ub_original_open=native_open;ub_original_close=native_close;ub_original_draw=native_draw_unchanged;
 ub_precision_renderer=(struct ub_precision_render_api){.prepare=prepare,.render=render,.present=present,.unmap=unmap,.cue=cue_value,.mapped=&mapped_value};
}
static struct ub_request br_request(unsigned op,unsigned edit,unsigned value,unsigned owner,uint64_t birth){
 struct ub_request r=request(op);r.brightness.edit=edit;r.brightness.value=value;r.brightness.owner_pid=owner;
 r.brightness.owner_start_lo=(uint32_t)birth;r.brightness.owner_start_hi=(uint32_t)(birth>>32);
 memcpy(r.brightness.token,state.brightness_token,16);return r;
}
static int br_send(unsigned op,unsigned edit,unsigned value,struct ub_status *out){
 struct ub_request r=br_request(op,edit,value,(unsigned)getpid(),self_birth);return ub_test_process(&r,out);
}
static void native_open_fixture(void){assert(!ub_hook_open("/mnt/usb/sub/example.srt"));mapped_value=1;ub_test_refresh(&state);}
static struct ub_brightness_save_ack save_ack(int result){
 struct ub_brightness_save_ack ack={0};ack.magic=UB_BRIGHTNESS_SAVE_MAGIC;ack.version=UB_BRIGHTNESS_PROTOCOL;
 memcpy(ack.token,brightness.seed.token,16);ack.pid=brightness.seed.pid;ack.birth_lo=brightness.seed.birth_lo;ack.birth_hi=brightness.seed.birth_hi;
 ack.edit=brightness.id;ack.epoch=brightness.commit_epoch;ack.seq=brightness.commit_seq;
 ack.percent=brightness.commit_percent;ack.level=brightness.commit_level;ack.result=result;return ack;
}
static void tick(void){gate_lock();brightness_tick();snapshot();gate_unlock();}
int main(void){
 char directory[]="/tmp/ub820-brightness-XXXXXX";assert(mkdtemp(directory));assert(!ub_br_birth((unsigned)getpid(),&self_birth));
 snprintf(br_seed_path,sizeof br_seed_path,"%s/seed",directory);snprintf(br_ack_path,sizeof br_ack_path,"%s/ack",directory);
 snprintf(br_claim_path,sizeof br_claim_path,"%s/claim",directory);snprintf(br_status_path,sizeof br_status_path,"%s/status",directory);
 snprintf(br_notice_path,sizeof br_notice_path,"%s/commit",directory);
 snprintf(br_request_path,sizeof br_request_path,"%s/request",directory);
 snprintf(play_path,sizeof play_path,"%s/play",directory);snprintf(lock_path,sizeof lock_path,"%s/lock",directory);
 struct ub_status out;unsigned cases=0,edit,epoch,before;
 for(unsigned p=0;p<=100;p++){assert(ub_brightness_level(p)==(p*255u+50u)/100u);assert(ub_brightness_percent(ub_brightness_level(p))==p);}cases++;
 for(unsigned l=0;l<=255;l++)assert(ub_brightness_percent(l)==(l*100u+127u)/255u);
 cases++;
 reset_fixture(100);native_open_fixture();assert(!fg_calls&&state.brightness_known&&state.brightness_available&&state.text_level==255);cases++;
 assert(!br_send(UB_OP_BRIGHTNESS_BEGIN,0,0,&out));edit=out.brightness_edit;assert(edit&&out.brightness_phase==UB_BRIGHTNESS_PREVIEW);cases++;
 assert(!br_send(UB_OP_BRIGHTNESS_PREVIEW,edit,50,&out));assert(gc.foreground==0xff808080&&text_level==128&&out.brightness_percent==50);cases++;
 before=fg_calls;assert(send_level(80,&out)==UB_BUSY&&fg_calls==before);cases++;
 assert(!br_send(UB_OP_BRIGHTNESS_CANCEL,edit,0,&out));assert(!out.brightness_edit&&gc.foreground==0xffffffff&&text_level==255);cases++;
 /* Exact raw/alpha baseline is not reconstructed from rounded percent. */
 gc.foreground=0x7f505050;text_level=80;mapped_value=1;
 assert(!br_send(UB_OP_BRIGHTNESS_BEGIN,0,0,&out));edit=out.brightness_edit;
 assert(!br_send(UB_OP_BRIGHTNESS_PREVIEW,edit,31,&out)&&gc.foreground==0x7f4f4f4f);
 assert(!br_send(UB_OP_BRIGHTNESS_CANCEL,edit,0,&out)&&gc.foreground==0x7f505050&&text_level==80);cases++;
 assert(!br_send(UB_OP_BRIGHTNESS_BEGIN,0,0,&out));edit=out.brightness_edit;
 assert(br_send(UB_OP_BRIGHTNESS_COMMIT,edit,31,&out)==UB_STALE);assert(brightness.phase==UB_BRIGHTNESS_PREVIEW);cases++;
 assert(!br_send(UB_OP_BRIGHTNESS_PREVIEW,edit,40,&out));notice_write_failure=1;
 assert(br_send(UB_OP_BRIGHTNESS_COMMIT,edit,40,&out)==UB_IO_ERROR&&brightness.phase==UB_BRIGHTNESS_PREVIEW&&access(br_notice_path,F_OK));cases++;
 notice_write_failure=0;assert(!br_send(UB_OP_BRIGHTNESS_COMMIT,edit,40,&out));
 assert(out.brightness_phase==UB_BRIGHTNESS_COMMIT&&out.brightness_commit_seq==out.seq&&out.brightness_commit_level==102);cases++;
 assert(br_send(UB_OP_BRIGHTNESS_BEGIN,0,0,&out)==UB_BUSY);assert(br_send(UB_OP_BRIGHTNESS_CANCEL,edit,0,&out)==UB_BUSY);cases++;
 struct ub_brightness_save_ack ack=save_ack(UB_BRIGHTNESS_SAVED),bad_ack=ack;bad_ack.seq++;
 assert(!ub_br_private_write(br_ack_path,&bad_ack,sizeof bad_ack));tick();assert(brightness.phase==UB_BRIGHTNESS_COMMIT);cases++;
 assert(!ub_br_private_write(br_ack_path,&ack,sizeof ack));tick();assert(brightness.phase==UB_BRIGHTNESS_NONE&&text_level==102);cases++;
 /* COMMIT was accepted before Close: preserve its own epoch/receipt while
  * native lifetime advances, and never reinterpret it as a new preview. */
 assert(!br_send(UB_OP_BRIGHTNESS_BEGIN,0,0,&out));edit=out.brightness_edit;epoch=out.epoch;
 assert(!br_send(UB_OP_BRIGHTNESS_PREVIEW,edit,60,&out));assert(!br_send(UB_OP_BRIGHTNESS_COMMIT,edit,60,&out));ack=save_ack(UB_BRIGHTNESS_SAVE_ERROR);
 assert(!ub_hook_close());assert(!ub_hook_open("/mnt/usb/sub/next.srt"));ub_test_refresh(&out);
 assert(out.epoch!=epoch&&out.brightness_commit_epoch==epoch&&out.brightness_edit_epoch==epoch&&text_level==153&&gc.foreground==0xff999999);cases++;
 assert(!ub_br_private_write(br_ack_path,&ack,sizeof ack));tick();assert(!brightness.phase&&text_level==153);cases++;
 /* Ordinary preview is abandoned on a native Close/Open. */
 mapped_value=1;assert(!br_send(UB_OP_BRIGHTNESS_BEGIN,0,0,&out));edit=out.brightness_edit;epoch=out.epoch;
 struct ub_request stale=br_request(UB_OP_BRIGHTNESS_COMMIT,edit,20,(unsigned)getpid(),self_birth);
 assert(!br_send(UB_OP_BRIGHTNESS_PREVIEW,edit,20,&out));assert(!ub_hook_close());assert(!ub_hook_open("/mnt/usb/sub/third.srt"));
 assert(!brightness.phase&&text_level==153&&gc.foreground==0xff999999);assert(ub_test_process(&stale,&out)==UB_STALE);cases++;
 /* Native UI owner dies but remains zombie until waitpid: birth alone must
  * not keep its preview alive. The existing worker makes one restoration. */
 pid_t child=fork();assert(child>=0);if(!child){for(;;)pause();}
 uint64_t child_birth=0;for(unsigned i=0;i<100&&ub_br_birth((unsigned)child,&child_birth);i++)brightness_test_usleep(1000);
 assert(child_birth);struct ub_request r=br_request(UB_OP_BRIGHTNESS_BEGIN,0,0,(unsigned)child,child_birth);
 assert(!ub_test_process(&r,&out));edit=out.brightness_edit;r=br_request(UB_OP_BRIGHTNESS_PREVIEW,edit,10,(unsigned)child,child_birth);assert(!ub_test_process(&r,&out));
 assert(!kill(child,SIGKILL));brightness_test_usleep(20000);tick();assert(!brightness.phase&&text_level==153&&gc.foreground==0xff999999);
 int status;assert(waitpid(child,&status,0)==child);before=fg_calls;tick();assert(fg_calls==before);cases++;
 reset_fixture(50);assert(brightness.seed_pending&&text_level==128);native_open_fixture();assert(gc.foreground==0xff808080&&state.brightness_available);cases++;
 reset_fixture(50);assert(!ub_hook_draw(clock_value));ub_test_refresh(&out);assert(gc.foreground==0xff808080&&!brightness.seed_pending&&out.brightness_available);cases++;
 reset_fixture(50);atomic_write(&session,1);stop_after_startup=1;pthread_t thread;
 assert(!pthread_create(&thread,0,worker,0));assert(!pthread_join(thread,0));stop_after_startup=0;
 assert(gc.foreground==0xff808080&&!brightness.seed_pending&&state.brightness_available&&render_calls==1&&rendered_color==0xff808080);cases++;
 seed(50,1);ub_test_reset();assert(text_level==255&&!brightness.seed_ok&&!state.brightness_available);cases++;
 unlink(br_seed_path);ub_test_reset();assert(text_level==255&&!brightness.seed_ok);cases++;
 reset_fixture(100);native_open_fixture();r=br_request(UB_OP_BRIGHTNESS_BEGIN,0,0,(unsigned)getpid(),self_birth);r.brightness.token[0]^=1;
 assert(ub_test_process(&r,&out)==UB_BAD_PATH&&!brightness.phase);cases++;
 r=br_request(UB_OP_BRIGHTNESS_BEGIN,0,0,(unsigned)getpid(),self_birth+1);assert(ub_test_process(&r,&out)==UB_STALE&&!brightness.phase);cases++;
 unlink(br_seed_path);unlink(br_ack_path);unlink(br_claim_path);unlink(br_notice_path);unlink(br_status_path);unlink(br_request_path);unlink(play_path);unlink(lock_path);assert(!rmdir(directory));
 printf("subtitle_brightness_test passed: %u cases; preview/ARGBcancel, immutablecommit, ownerzombie, seed/liveattach and epoch safety\n",cases);return 0;
}
