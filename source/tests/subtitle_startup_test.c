/* Include the real supervisor step, with only its OS/device transport mocked. */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <assert.h>
#include "ub.h"
#include "../modules/external_subtitles/protocol.h"
static int loaded_value,status_present=1,status_active,style_result,style_calls,on_writes,gate_writes,load_events,maps_done;
static int preference_writes,font_fail,font_publications,lock_busy,child_running,child_status,killed,marker_created,marker_removed;
static int dat_open_error;
static int test_open(const char *path,int flags,...){
 if(!strcmp(path,UB_STATUS_FILE))return status_present?100:-1;
 if(!strcmp(path,"/proc/42/mem"))return 101;
 if(!strcmp(path,"/proc/42/maps")){maps_done=0;return 102;}
 if(!strcmp(path,"/tmp/ti_font_size.tmp"))return font_fail?-1:103;
 if(!strcmp(path,"/tmp/apl_play3dlib_3ddynamic_lock"))return 104;
 if(!strcmp(path,"/tmp/apl_play3dlib_play_dat.dat")){if(dat_open_error&&!(flags&(O_WRONLY|O_RDWR))){errno=dat_open_error;return -1;}return 105;}
 if(!strcmp(path,"/tmp/ub820_srt_precision.off")){assert(flags&O_CREAT);marker_created++;return 106;}
 return -1;
}
static ssize_t test_read(int fd,void *buf,size_t size){
 if(fd==100){struct ub_status status={0};status.magic=UB_MAGIC;status.version=UB_VERSION;status.active=status_active;assert(size==sizeof status);memcpy(buf,&status,size);return size;}
 if(fd==102){const char *maps="293a0000-293d8000 r-xp 00000000 00:01 1 /known/libtiwin.so.0.0.0\n";if(maps_done)return 0;maps_done=1;assert(size>strlen(maps));memcpy(buf,maps,strlen(maps));return strlen(maps);}
 if(fd==105){memset(buf,0,size);return size;}
 return -1;
}
static ssize_t test_pread(int fd,void *buf,size_t size,off_t offset){assert(fd==101 && offset==0x434e8 && size==1);*(unsigned char *)buf=loaded_value;return 1;}
static ssize_t test_write(int fd,const void *buf,size_t size){
 if(fd==105){on_writes++;return size;}
 assert(fd==103&&size==4&&!memcmp(buf,"122\n",4));return size;
}
static ssize_t test_pwrite(int fd,const void *buf,size_t size,off_t offset){(void)buf;assert(fd==101 && size==4 && offset==0x43504);gate_writes++;return size;}
static int test_close(int fd){(void)fd;return 0;}
/* The 1 Hz supervisor must never wait for the play-data lock. */
static int test_flock(int fd,int flags){
 assert(fd==104);if(flags&LOCK_UN)return 0;
 assert((flags&LOCK_EX)&&(flags&LOCK_NB));
 if(lock_busy){errno=EWOULDBLOCK;return -1;}return 0;
}
static int test_rename(const char *from,const char *to){
 assert(!strcmp(from,"/tmp/ti_font_size.tmp")&&!strcmp(to,"/tmp/ti_font_size"));font_publications++;return 0;
}
static int test_unlink(const char *path){if(!strcmp(path,"/tmp/ub820_srt_precision.off"))marker_removed++;return 0;}
static int fork_failed;
static pid_t test_fork(void){load_events++;return fork_failed?-1:123;}
/* ...and never wait for evtSend: only nonblocking reaping. */
static pid_t test_waitpid(pid_t pid,int *status,int flags){
 assert(pid==123&&flags==WNOHANG);if(child_running)return 0;*status=child_status;return pid;
}
static int test_kill(pid_t pid,int sig){if(pid==123){assert(sig==SIGKILL);killed++;return 0;}return kill(pid,sig);}
#define open test_open
#define read test_read
#define pread test_pread
#define write test_write
#define pwrite test_pwrite
#define close test_close
#define flock test_flock
#define rename test_rename
#define unlink test_unlink
#define fork test_fork
#define waitpid test_waitpid
#define kill test_kill
#include "../modules/external_subtitles/module.c"
#undef open
#undef read
#undef pread
#undef write
#undef pwrite
#undef close
#undef flock
#undef rename
#undef unlink
#undef fork
#undef waitpid
#undef kill
int ub_hash_file(const char *path,char result[65]){assert(!strcmp(path,"/known/libtiwin.so.0.0.0"));strcpy(result,"6d2216fcd15814defdfc2357ddb5b517bf85398d26d1f530d61bed22087994bf");return 0;}
int ub_patch_code(int pid,const struct ub_code_patch *patches,unsigned count){(void)patches;assert(pid==42 && count==6);style_calls++;return style_result;}
const char *ub_patch_stage(void){return "test";}
void ub_log(const char *module,int level,const char *format,...){(void)module;(void)level;(void)format;}
int ub_setting_set(const char *module,const char *key,const char *value){(void)module;(void)key;(void)value;preference_writes++;return 0;}
/* Retain the actual parts descriptor for its lifecycle-policy assertion.
 * Its unrelated row/translation callbacks must not run in this fixture. */
const struct ub_payload ub_payload_external_subtitles_precision_source={0};
const struct ub_payload ub_payload_external_subtitles_engine={0};
const struct ub_payload ub_payload_external_subtitles_native_text={0};
int ub_mem_read(int pid,uint32_t address,void *buffer,size_t length){(void)pid;(void)address;(void)buffer;(void)length;abort();}
int ub_mem_write(int pid,uint32_t address,const void *buffer,size_t length){(void)pid;(void)address;(void)buffer;(void)length;abort();}
void ub_changed(struct ub_part *part){(void)part;abort();}
uint32_t ub_map_base(int pid,const char *suffix,char *path,size_t size){(void)pid;(void)suffix;(void)path;(void)size;abort();}
int ub_settled(const char *process){(void)process;abort();}
const struct ub_payload_variant *ub_payload_variant(const struct ub_payload *payload,const char *sha){(void)payload;(void)sha;abort();}
int ub_install_payload(int pid,const struct ub_payload *payload){(void)pid;(void)payload;abort();}
const char *ub_install_stage(void){abort();}
static struct ub_target target(void){struct ub_target value={0};value.pid=42;return value;}
int main(void){
 struct ub_target t=target();struct ub_part part={0};unsigned cases=0;
 /* Disc sessions end normally; their clock must remain eligible next time. */
 assert(!parts[0].guarded && !strcmp(parts[0].process,"apl_bdv"));
 /* A worker status published immediately after injection cannot skip setup. */
 engine_step(&part,&t);assert(style_calls==1 && t.state[2]==1 && on_writes==1 && gate_writes==1 && load_events==1 && font_publications==1);
 engine_step(&part,&t);assert(style_calls==1 && t.state[0]==15 && on_writes==1 && load_events==1);cases++;
 /* An already-loaded engine gets style verification without forcing On. */
 t=target();loaded_value=1;engine_step(&part,&t);assert(style_calls==2 && t.state[2]==1 && t.state[0]==15 && on_writes==1 && load_events==1);
 t=target();loaded_value=0;status_active=1;engine_step(&part,&t);assert(style_calls==3 && t.state[0]==15 && on_writes==1 && load_events==1);cases++;
 /* A busy code range defers startup; successful retry styles exactly once. */
 t=target();status_active=0;style_result=-1;engine_step(&part,&t);assert(!t.state[2] && !t.state[0] && on_writes==1 && load_events==1);
 style_result=0;engine_step(&part,&t);assert(t.state[2]==1 && on_writes==2 && load_events==2);
 t=target();style_result=-2;engine_step(&part,&t);assert(t.failed && t.state[0]==15 && on_writes==2 && load_events==2);cases++;
 assert(!preference_writes); /* Startup is not a confirmed menu choice. */
 /* An unreadable (not missing) play-data file is never rewritten with zeros;
  * a missing one is created from zero as before. */
 {int writes=on_writes,events=load_events;t=target();t.state[2]=t.state[3]=1;dat_open_error=EIO;
  engine_step(&part,&t);assert(on_writes==writes&&load_events==events&&!t.state[0]&&t.state[7]==1);
  dat_open_error=ENOENT;engine_step(&part,&t);assert(on_writes==writes+1&&load_events==events+1);
  dat_open_error=0;on_writes=writes;load_events=events;cases++;}
 /* Font calibration: a failed write defers startup and is retried alone,
  * without repeating the code transaction; it is published by rename. */
 status_present=0;style_result=0;t=target();font_fail=1;int styled=style_calls,published=font_publications;
 engine_step(&part,&t);assert(style_calls==styled+1 && t.state[2]==1 && on_writes==2 && load_events==2 && font_publications==published);
 engine_step(&part,&t);assert(style_calls==styled+1 && on_writes==2 && load_events==2);
 font_fail=0;engine_step(&part,&t);assert(style_calls==styled+1 && font_publications==published+1 && on_writes==3 && load_events==3);cases++;
 /* A calibration that never succeeds does not block subtitles for ever. */
 t=target();font_fail=1;child_running=0;
 for(int i=0;i<15&&load_events==3;i++)engine_step(&part,&t);
 assert(load_events==4 && style_calls==styled+2);font_fail=0;cases++;
 /* A busy play-data lock defers startup to a later tick, no wait, no event. */
 t=target();lock_busy=1;int events=load_events,writes=on_writes;
 engine_step(&part,&t);assert(load_events==events && on_writes==writes && !t.state[0]);
 lock_busy=0;engine_step(&part,&t);assert(load_events==events+1 && on_writes==writes+1 && t.state[0]==1);cases++;
 /* A running evtSend is never waited for, and no second event is sent. */
 child_running=1;for(int i=0;i<4;i++)engine_step(&part,&t);
 assert(load_events==events+1 && !killed);cases++;
 /* A hung evtSend is stopped; its unknown result is reconciled with the engine
  * state for a tick before a single new event. */
 engine_step(&part,&t);assert(killed==1 && load_events==events+1);
 child_running=0;child_status=SIGKILL;engine_step(&part,&t);assert(load_events==events+1);
 engine_step(&part,&t);assert(load_events==events+2 && t.state[0]==2);cases++;
 /* ...and evidence of a loaded engine ends startup with no new event. */
 child_status=0;loaded_value=1;engine_step(&part,&t);assert(load_events==events+2 && t.state[0]==15);loaded_value=0;cases++;
 /* A lock held longer than any failure limit still only defers. */
 t=target();lock_busy=1;events=load_events;
 for(int i=0;i<40;i++)engine_step(&part,&t);
 assert(load_events==events && !t.state[0]);
 lock_busy=0;engine_step(&part,&t);assert(load_events==events+1 && t.state[0]==1);cases++;
 /* That playback ends while its event runs: the every-tick part reaps it
  * (stopping it at the deadline), and its unknown result does not hold back
  * the next instance. */
 {struct ub_part loader={0};int k=killed;child_running=1;evt_polled=0;
  for(int i=0;i<EVT_DEADLINE;i++)evt_step(&loader,0);
  assert(killed==k+1 && evt_child);
  child_running=0;child_status=SIGKILL;evt_step(&loader,0);assert(!evt_child);
  struct ub_target next=target();next.born=7;events=load_events;
  engine_step(&part,&next);assert(load_events==events+1 && next.state[0]==1);cases++;}
 /* precision=0 disables the source/client; precision=1 permits the source
  * and removes the engine marker. */
 precision=0;engine_prepare(&part,&t);assert(marker_created==1 && !marker_removed);
 t=target();precision_prepare(&part,&t);assert(t.failed);
 precision=1;engine_prepare(&part,&t);assert(marker_created==1 && marker_removed==1);
 t=target();precision_prepare(&part,&t);assert(!t.failed);cases++;
 /* An idle backend status predates delivery. Only a successful sender can
  * use it to end retries. Never retry after the engine becomes active. */
 t=target();t.born=9;child_status=0;status_present=1;events=load_events;
 fork_failed=1;engine_step(&part,&t);assert(load_events==events+1&&!evt_child);
 fork_failed=0;engine_step(&part,&t);assert(load_events==events+2&&evt_child);
 child_status=1<<8;engine_step(&part,&t);assert(load_events==events+3&&evt_child);
 child_status=0;engine_step(&part,&t);assert(!evt_child&&t.state[0]==15&&load_events==events+3);cases++;
 t=target();t.born=10;engine_step(&part,&t);assert(evt_child);
 child_status=1<<8;status_active=1;engine_step(&part,&t);
 assert(!evt_child&&t.state[0]==15&&load_events==events+4);cases++;
 printf("subtitle_startup_test passed: %u cases; early status cannot skip style; loaded/user-Off preserved; retry/quarantine handled; no preference save; calibration retried alone; nonblocking lock and evtSend; precision marker\n",cases);
 return 0;
}
