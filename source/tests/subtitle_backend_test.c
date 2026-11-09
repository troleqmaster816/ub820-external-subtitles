/* Real backend worker: UFS/status I/O must not hold the native hook gate. */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
#include "../modules/external_subtitles/protocol.h"
#undef UB_REQUEST_FILE
#undef UB_STATUS_FILE
#define UB_REQUEST_FILE "/tmp/ub820-subtitle-backend-test.request"
#define UB_STATUS_FILE "/tmp/ub820-subtitle-backend-test.status"
#define usleep test_usleep
#define rename test_rename
#include "../modules/external_subtitles/backend.c"
#undef usleep
#undef rename
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static pthread_mutex_t sync_mutex=PTHREAD_MUTEX_INITIALIZER,native_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int ufs_entered,ufs_release,rename_entered,rename_release,stop_worker,draw_done,native_draw_calls;
static uint32_t active_value=1,parser_value=1,display_value=1,clock_value=1;
static int32_t attributes[5];
static unsigned char loaded_value=1;

static void wait_for(int *flag) {
 struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=3;
 pthread_mutex_lock(&sync_mutex);
 while(!*flag){int rc=pthread_cond_timedwait(&changed,&sync_mutex,&deadline);assert(!rc);}
 pthread_mutex_unlock(&sync_mutex);
}
static void set_flag(int *flag) {
 pthread_mutex_lock(&sync_mutex);*flag=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&sync_mutex);
}
int test_usleep(unsigned int usec) {
 if(usec==200000){
  pthread_mutex_lock(&sync_mutex);int stop=stop_worker;pthread_mutex_unlock(&sync_mutex);
  if(stop)pthread_exit(0);
 }
 struct timespec delay={usec/1000000,(long)(usec%1000000)*1000};nanosleep(&delay,0);return 0;
}
int test_rename(const char *from,const char *to) {
 pthread_mutex_lock(&sync_mutex);rename_entered=1;pthread_cond_broadcast(&changed);
 while(!rename_release)pthread_cond_wait(&changed,&sync_mutex);
 pthread_mutex_unlock(&sync_mutex);return rename(from,to);
}
int UFS_OpenDir(const char *path,uint32_t *handle){(void)path;*handle=1;return 0;}
int UFS_ReadDir(uint32_t handle,void *record,uint32_t *count){
 (void)handle;(void)record;pthread_mutex_lock(&sync_mutex);ufs_entered=1;pthread_cond_broadcast(&changed);
 while(!ufs_release)pthread_cond_wait(&changed,&sync_mutex);
 pthread_mutex_unlock(&sync_mutex);*count=0;return 0;
}
int UFS_CloseDir(uint32_t handle){(void)handle;return 0;}
static int native_open(const char *path){(void)path;return 0;}
static int native_draw(uint32_t time){(void)time;native_draw_calls++;return 0;}
static void *draw(void *unused){(void)unused;ub_hook_draw(1);set_flag(&draw_done);return 0;}

int main(void){
 struct ub_api bindings={0};struct ub_status published;pthread_t worker_tid,draw_tid;
 bindings.active=&active_value;bindings.parser=&parser_value;bindings.display=&display_value;
 bindings.clock=&clock_value;bindings.attrs=attributes;bindings.loaded=&loaded_value;bindings.mutex=&native_mutex;
 ub_test_bind(&bindings);ub_test_reset();ub_original_open=native_open;ub_original_draw=native_draw;
 unlink(UB_REQUEST_FILE);unlink(UB_STATUS_FILE);
 /* The startup scan blocks in UFS while Draw must finish normally. */
 assert(!pthread_create(&worker_tid,0,worker,0));wait_for(&ufs_entered);
 assert(!pthread_create(&draw_tid,0,draw,0));wait_for(&draw_done);pthread_join(draw_tid,0);
 assert(native_draw_calls==1 && atomic_read(&gate)==0);
 /* New playback epoch while UFS is blocked invalidates the catalogue. */
 uint32_t old_epoch=state.epoch;
 assert(!ub_hook_open("/mnt/usb/sub/new.srt"));assert(state.epoch!=old_epoch);
 set_flag(&ufs_release);wait_for(&rename_entered);
 /* Filesystem publication is blocked too, but Draw still finishes. */
 draw_done=0;assert(!pthread_create(&draw_tid,0,draw,0));wait_for(&draw_done);pthread_join(draw_tid,0);
 assert(native_draw_calls==2 && atomic_read(&gate)==0);
 assert(state.result==UB_STALE && state.count==0);
 /* A dirty notification raised during publication survives for the next tick. */
 assert(status_dirty==0);assert(!ub_hook_open("/mnt/usb/sub/next.srt"));assert(status_dirty==1);
 set_flag(&stop_worker);set_flag(&rename_release);pthread_join(worker_tid,0);
 assert(status_dirty==1);
 FILE *file=fopen(UB_STATUS_FILE,"rb");assert(file);
 assert(fread(&published,1,sizeof published,file)==sizeof published);fclose(file);
 assert(published.result==UB_STALE && published.count==0 && published.epoch!=old_epoch);
 /* Invalid requests do not replace the last published directory. */
 char before[UB_PATH_BYTES];copy(before,state.dir,sizeof before);
 gate_lock();assert(catalogue("/etc")==UB_BAD_PATH);assert(equal(before,state.dir));gate_unlock();
 unlink(UB_STATUS_FILE);
 puts("subtitle_backend_test passed: Draw during UFS/status I/O; stale scan discarded; dirty event retained");
 return 0;
}
