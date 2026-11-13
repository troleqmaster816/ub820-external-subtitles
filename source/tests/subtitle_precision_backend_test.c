/* Controller integration with synthetic clocks and a mutex-owning native Draw. */
#define UB_HOST_TEST 1
#include "../modules/external_subtitles/backend.c"
#include <assert.h>
#include <string.h>

static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static uint32_t active=1,parser=1,display=1,coarse=1027,cue[2],mapped,seek_flag;
static int32_t attrs[5];static unsigned char loaded=1;
static unsigned fallback_calls,precise_calls,seek_calls,draw_calls,last_cc;
static int draw_failure,draw_adjust;
static uint32_t pack(uint32_t sec,uint32_t cc){return ((sec/3600)<<24)|(((sec%3600)/60)<<16)|((sec%60)<<8)|cc;}
static uint32_t get_state(void){precise_calls++;return 1;}
static int32_t sync_seconds(void){return 0;}
static uint32_t get_seek(void){return seek_flag;}
static void set_seek(uint32_t value){seek_flag=value;if(value)seek_calls++;}
static int search(void *p,uint32_t h,uint32_t m,uint32_t s,uint32_t cc){
 (void)h;(void)m;(void)s;assert(p==cue);last_cc=cc;return 0;
}
static int next(void *p){(void)p;return 4;}
static int zero_op(void){return 0;}
static int present(void){mapped=1;return 0;}
static int unmap(void){mapped=0;return 0;}
static void store_display(uint32_t d){(void)d;}
static int needs_update(void){return 0;}
static int original_update(uint32_t d,uint32_t s){(void)d;(void)s;fallback_calls++;return 0;}
static int original_draw(uint32_t sec){
 int rc;draw_calls++;if(draw_failure)return 7;pthread_mutex_lock(&mutex);rc=ub_hook_update(display,sec+(uint32_t)draw_adjust);pthread_mutex_unlock(&mutex);return rc;
}
static int original_open(const char *path){(void)path;return 0;}
int UFS_OpenDir(const char *p,uint32_t *h){(void)p;(void)h;return -1;}
int UFS_ReadDir(uint32_t h,void *p,uint32_t *n){(void)h;(void)p;(void)n;return -1;}
int UFS_CloseDir(uint32_t h){(void)h;return 0;}
static void *foreign_update(void *p){(void)p;pthread_mutex_lock(&mutex);ub_hook_update(1,1027);pthread_mutex_unlock(&mutex);return 0;}
static struct ub_pc_state sample(uint64_t now,uint32_t sec,uint32_t cc){
 struct ub_pc_state s;memset(&s,0,sizeof s);s.have_packet=s.valid=1;s.monotonic_ms=now;s.ticks=(uint64_t)sec*45000+cc*450;return s;
}
int main(void){
 struct ub_api a={0};pthread_t thread;uint64_t now;uint32_t sec,cc,generation;
 a.active=&active;a.parser=&parser;a.display=&display;a.clock=&coarse;a.attrs=attrs;a.loaded=&loaded;a.mutex=&mutex;a.seek=set_seek;
 ub_test_bind(&a);ub_test_reset();ub_original_draw=original_draw;ub_original_update=original_update;ub_original_open=original_open;
 ub_precision_renderer=(struct ub_precision_render_api){get_state,sync_seconds,get_seek,set_seek,search,next,zero_op,zero_op,present,unmap,store_display,cue,&mapped,needs_update};
 cue[0]=pack(1027,64);cue[1]=pack(1029,59);seek_flag=1;
 assert(!ub_hook_update(1,1027));assert(fallback_calls==1 && !precise_calls);
 gate_lock();assert(!precision_draw(1027,64));gate_unlock();assert(last_cc==64 && precise_calls==1);
 gate_lock();precision_draw(1027,80);precision_draw(1027,65);gate_unlock();assert(seek_calls==1 && last_cc==65);
 /* A different native thread must retain seconds even while context is live. */
 __atomic_store_n(&precision_call_owner,pthread_self(),__ATOMIC_RELAXED);
 __atomic_store_n(&precision_call_seconds,1027,__ATOMIC_RELAXED);atomic_write(&precision_call_active,1);
 assert(!pthread_create(&thread,0,foreign_update,0));pthread_join(thread,0);atomic_write(&precision_call_active,0);
 assert(fallback_calls==2);
 assert(!ub_payload_now_ms(&now));precision_stamp=now;precision_valid=1;precision_seconds=1027;precision_cc=80;
 seek_flag=1;assert(!ub_hook_draw(1027));assert(last_cc==80);
 precision_stamp=now-201;seek_flag=1;assert(!ub_hook_draw(1027));assert(fallback_calls==2 && last_cc==80); /* never rewind fallback */
 precision_last_ticks=(uint64_t)1028*45000+80*450;seek_flag=1;
 assert(!ub_hook_draw(1027));assert(precision_last_ticks==(uint64_t)1028*45000+80*450); /* one second ahead */
 generation=precision_generation;assert(!ub_hook_draw(1040));assert(!precision_valid && precision_generation!=generation);
 /* Commit revalidates after gate acquisition, rejects prior lifecycle and
  * expired/aligned-to-old-title samples, and propagates source invalidation. */
 atomic_write(&session,1);precision_drawn=0;coarse=1027;
 assert(!ub_payload_now_ms(&now));struct ub_pc_state s=sample(now,1027,80);
 gate_lock();assert(precision_commit(&s,precision_generation));gate_unlock();assert(precision_valid);
 unsigned before=draw_calls;
 gate_lock();assert(precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before); /* unchanged cache */
 assert(!ub_hook_update(1,1027));
 gate_lock();assert(precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before+1); /* paused native refresh */
 s.valid=0;gate_lock();assert(!precision_commit(&s,precision_generation));gate_unlock();assert(!precision_valid);
 s=sample(now-201,1027,80);before=draw_calls;
 gate_lock();assert(!precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before);
 assert(!ub_payload_now_ms(&now));s=sample(now,1027,80);
 gate_lock();assert(!precision_commit(&s,precision_generation-1));gate_unlock();assert(draw_calls==before);
 coarse=1100;gate_lock();assert(!precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before);coarse=1027;
 /* When native playback stops requesting Draw, the worker cannot perpetually
  * repaint over menus. An explicit native refresh can still update a pause. */
 assert(!ub_payload_now_ms(&now));s=sample(now,1027,80);precision_native_ms=now-1501;
 precision_refresh_consumed=atomic_read(&precision_refresh_requested);before=draw_calls;
 gate_lock();assert(precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before);
 assert(!ub_hook_update(1,1027));
 gate_lock();assert(precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before+1);
 gate_lock();assert(precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before+1);
 /* A failed render is retried by a new native Draw/explicit refresh, rather
  * than a busy retry every 20ms against a failed graphics surface. */
 precision_native_ms=now;draw_failure=1;before=draw_calls;s=sample(now,1027,81);
 gate_lock();assert(precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before+1);
 gate_lock();assert(precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before+1);
 draw_failure=0;assert(precision_force_seek);unsigned seeks=seek_calls;
 gate_lock();assert(!precision_draw(1027,80));gate_unlock();assert(!precision_render_failed && seek_calls==seeks+1);
 /* Even an unexpected same-owner ABI mismatch must not create refresh work
  * which wakes the clock worker again after stock playback drawing stopped. */
 uint32_t requests=atomic_read(&precision_refresh_requested);draw_adjust=1;
 gate_lock();assert(!precision_draw(1027,80));gate_unlock();draw_adjust=0;
 assert(atomic_read(&precision_refresh_requested)==requests);
 generation=precision_generation;assert(!ub_hook_open("/mnt/usb/sub/sub.srt"));assert(precision_generation!=generation);
 assert(!ub_payload_now_ms(&now));s=sample(now,1027,80);before=draw_calls;
 atomic_write(&precision_refresh_generation,generation);__atomic_add_fetch(&precision_refresh_requested,1,__ATOMIC_RELEASE);
 gate_lock();assert(precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before); /* old-session refresh */
 assert(!ub_hook_update(1,1027));
 gate_lock();assert(precision_commit(&s,precision_generation));gate_unlock();assert(draw_calls==before+1);
 /* Exhaust all centisecond and second boundaries in the supported title range. */
 for(uint32_t i=0;i<86400;i++)for(unsigned k=0;k<2;k++){
  uint32_t t=i*45000+(k?44999:0);precision_split(t,&sec,&cc);assert(sec==t/45000 && cc==(t%45000)/450);
 }
 for(uint32_t i=0;i<45000;i++){precision_split(1027*45000+i,&sec,&cc);assert(sec==1027 && cc==i/450);}
 puts("subtitle_precision_backend_test passed: owner isolation, paused refresh, lifecycle, stale commit and clock arithmetic");return 0;
}
