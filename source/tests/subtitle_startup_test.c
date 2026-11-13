/* Include the real supervisor step, with only its OS/device transport mocked. */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <assert.h>
#include "ub.h"
#include "../modules/external_subtitles/protocol.h"
static int loaded_value,status_present=1,status_active,style_result,style_calls,on_writes,gate_writes,load_events,maps_done;
static int preference_writes;
static int test_open(const char *path,int flags,...){
 (void)flags;
 if(!strcmp(path,UB_STATUS_FILE))return status_present?100:-1;
 if(!strcmp(path,"/proc/42/mem"))return 101;
 if(!strcmp(path,"/proc/42/maps")){maps_done=0;return 102;}
 if(!strcmp(path,"/tmp/ti_font_size"))return 103;
 if(!strcmp(path,"/tmp/apl_play3dlib_3ddynamic_lock"))return 104;
 if(!strcmp(path,"/tmp/apl_play3dlib_play_dat.dat"))return 105;
 return -1;
}
static ssize_t test_read(int fd,void *buf,size_t size){
 if(fd==100){struct ub_status status={0};status.magic=UB_MAGIC;status.version=UB_VERSION;status.active=status_active;assert(size==sizeof status);memcpy(buf,&status,size);return size;}
 if(fd==102){const char *maps="293a0000-293d8000 r-xp 00000000 00:01 1 /known/libtiwin.so.0.0.0\n";if(maps_done)return 0;maps_done=1;assert(size>strlen(maps));memcpy(buf,maps,strlen(maps));return strlen(maps);}
 if(fd==105){memset(buf,0,size);return size;}
 return -1;
}
static ssize_t test_pread(int fd,void *buf,size_t size,off_t offset){assert(fd==101 && offset==0x434e8 && size==1);*(unsigned char *)buf=loaded_value;return 1;}
static ssize_t test_write(int fd,const void *buf,size_t size){(void)buf;if(fd==105)on_writes++;else assert(fd==103);return size;}
static ssize_t test_pwrite(int fd,const void *buf,size_t size,off_t offset){(void)buf;assert(fd==101 && size==4 && offset==0x43504);gate_writes++;return size;}
static int test_close(int fd){(void)fd;return 0;}
static int test_flock(int fd,int flags){(void)fd;(void)flags;return 0;}
static pid_t test_fork(void){load_events++;return 123;}
static pid_t test_waitpid(pid_t pid,int *status,int flags){(void)flags;*status=0;return pid;}
#define open test_open
#define read test_read
#define pread test_pread
#define write test_write
#define pwrite test_pwrite
#define close test_close
#define flock test_flock
#define fork test_fork
#define waitpid test_waitpid
#include "../modules/external_subtitles/module.c"
#undef open
#undef read
#undef pread
#undef write
#undef pwrite
#undef close
#undef flock
#undef fork
#undef waitpid
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
 struct ub_target t=target();struct ub_part part={0};
 /* Disc sessions end normally; their clock must remain eligible next time. */
 assert(!parts[0].guarded && !strcmp(parts[0].process,"apl_bdv"));
 /* A worker status published immediately after injection cannot skip setup. */
 engine_step(&part,&t);assert(style_calls==1 && t.state[2]==1 && on_writes==1 && gate_writes==1 && load_events==1);
 engine_step(&part,&t);assert(style_calls==1 && t.state[0]==15 && on_writes==1 && load_events==1);
 /* An already-loaded engine gets style verification without forcing On. */
 t=target();loaded_value=1;engine_step(&part,&t);assert(style_calls==2 && t.state[2]==1 && t.state[0]==15 && on_writes==1 && load_events==1);
 t=target();loaded_value=0;status_active=1;engine_step(&part,&t);assert(style_calls==3 && t.state[0]==15 && on_writes==1 && load_events==1);
 /* A busy code range defers startup; successful retry styles exactly once. */
 t=target();status_active=0;style_result=-1;engine_step(&part,&t);assert(!t.state[2] && !t.state[0] && on_writes==1 && load_events==1);
 style_result=0;engine_step(&part,&t);assert(t.state[2]==1 && on_writes==2 && load_events==2);
 t=target();style_result=-2;engine_step(&part,&t);assert(t.failed && t.state[0]==15 && on_writes==2 && load_events==2);
 assert(!preference_writes); /* Startup is not a confirmed menu choice. */
 puts("subtitle_startup_test passed: early status cannot skip style; loaded/user-Off preserved; retry/quarantine handled; no preference save");
 return 0;
}
