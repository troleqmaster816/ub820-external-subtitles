/* Real backend worker, one controlled tick at a time: an old or repeated
 * request sequence is never replayed, but it gets one status publication so
 * a waiting client can resynchronise its counter. */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
#include <stdio.h>
int test_rename(const char *,const char *);
#include "../modules/external_subtitles/protocol.h"
#undef UB_REQUEST_FILE
#undef UB_STATUS_FILE
static char request_path[96],status_path[96];
#define UB_REQUEST_FILE request_path
#define UB_STATUS_FILE status_path
#define usleep test_usleep
#define rename test_rename
#include "../modules/external_subtitles/backend.c"
#undef usleep
#undef rename
#include <assert.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t sync_mutex=PTHREAD_MUTEX_INITIALIZER,native_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned granted,completed,publications,lists;static int stop_worker;
static uint32_t active_value,parser_value,display_value,clock_value=1;
static int32_t attributes[5];static unsigned char loaded_value;
int test_usleep(unsigned int usec){
 if(usec!=200000){struct timespec d={0,(long)usec*1000};nanosleep(&d,0);return 0;}
 pthread_mutex_lock(&sync_mutex);completed++;pthread_cond_broadcast(&changed);
 while(granted<completed)pthread_cond_wait(&changed,&sync_mutex);
 if(stop_worker){pthread_mutex_unlock(&sync_mutex);pthread_exit(0);}
 pthread_mutex_unlock(&sync_mutex);return 0;
}
int test_rename(const char *from,const char *to){
 pthread_mutex_lock(&sync_mutex);publications++;pthread_mutex_unlock(&sync_mutex);return rename(from,to);
}
int UFS_OpenDir(const char *path,uint32_t *handle){(void)path;*handle=1;__atomic_add_fetch(&lists,1,__ATOMIC_RELEASE);return 0;}
int UFS_ReadDir(uint32_t handle,void *record,uint32_t *count){(void)handle;(void)record;*count=0;return 0;}
int UFS_CloseDir(uint32_t handle){(void)handle;return 0;}
/* Let the worker run exactly one more loop iteration and wait for it. */
static unsigned tick(void){
 pthread_mutex_lock(&sync_mutex);unsigned target=++granted;pthread_cond_broadcast(&changed);
 while(completed<target+1)pthread_cond_wait(&changed,&sync_mutex);
 unsigned count=publications;pthread_mutex_unlock(&sync_mutex);return count;
}
static void send_request(uint32_t seq,uint32_t op,uint32_t epoch){
 struct ub_request r;char temp[128];memset(&r,0,sizeof r);
 r.magic=UB_MAGIC;r.version=UB_VERSION;r.seq=seq;r.op=op;r.epoch=epoch;
 snprintf(temp,sizeof temp,"%s.new",request_path);FILE *f=fopen(temp,"wb");assert(f);
 assert(fwrite(&r,1,sizeof r,f)==sizeof r);assert(!fclose(f));assert(!rename(temp,request_path));
}
static struct ub_status published(void){
 struct ub_status s;FILE *f=fopen(status_path,"rb");assert(f);assert(fread(&s,1,sizeof s,f)==sizeof s);fclose(f);return s;
}
int main(void){
 struct ub_api bindings={0};pthread_t thread;unsigned base,count,cases=0;
 snprintf(request_path,sizeof request_path,"/tmp/ub820-resync-%u.request",(unsigned)getpid());
 snprintf(status_path,sizeof status_path,"/tmp/ub820-resync-%u.status",(unsigned)getpid());
 bindings.active=&active_value;bindings.parser=&parser_value;bindings.display=&display_value;
 bindings.clock=&clock_value;bindings.attrs=attributes;bindings.loaded=&loaded_value;bindings.mutex=&native_mutex;
 ub_test_bind(&bindings);ub_test_reset();unlink(request_path);unlink(status_path);
 assert(!pthread_create(&thread,0,worker,0));
 pthread_mutex_lock(&sync_mutex);while(!completed)pthread_cond_wait(&changed,&sync_mutex);base=publications;pthread_mutex_unlock(&sync_mutex);
 assert(base==1);                                   /* startup status */
 unsigned scans=__atomic_load_n(&lists,__ATOMIC_ACQUIRE);
 send_request(5,UB_OP_LIST,0);assert(tick()==base+1&&published().seq==5);cases++;
 for(int i=0;i<6;i++){assert(tick()==base+1);}      /* unchanged file: no republishing */
 assert(__atomic_load_n(&lists,__ATOMIC_ACQUIRE)==scans+1);cases++;
 /* A client which restarted its counter: published once, never processed. */
 send_request(3,UB_OP_LIST,0);count=tick();assert(count==base+2);
 struct ub_status s=published();assert(s.seq==5&&__atomic_load_n(&lists,__ATOMIC_ACQUIRE)==scans+1);cases++;
 for(int i=0;i<6;i++){assert(tick()==count);}
 cases++;
 /* The same sequence with different content is a repeat, not a new request. */
 send_request(5,UB_OP_LIST,state.epoch);assert(tick()==count+1&&published().seq==5);
 assert(__atomic_load_n(&lists,__ATOMIC_ACQUIRE)==scans+1);cases++;
 /* The next newer sequence is processed normally. */
 send_request(6,UB_OP_LIST,0);assert(tick()==count+2&&published().seq==6&&__atomic_load_n(&lists,__ATOMIC_ACQUIRE)==scans+2);cases++;
 pthread_mutex_lock(&sync_mutex);stop_worker=1;granted=~0u;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&sync_mutex);
 pthread_join(thread,0);unlink(request_path);unlink(status_path);
 printf("subtitle_request_resync_test passed: %u cases; stale/repeated sequence published once, never replayed\n",cases);
 return 0;
}
