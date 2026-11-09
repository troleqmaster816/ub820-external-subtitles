/* Exercise actual key hook with a real FIFO and delivery fault injection. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "helper_menu.h"
#ifndef UB_KEYS_TEST_DIR
#define UB_KEYS_TEST_DIR "/tmp/ub820-keys-test"
#endif
#undef UB_KEYS_FIFO
#undef UB_OVERLAY_OPEN
#undef UB_OVERLAY_ABORT
#undef UB_NETFLIX_HELPER
#undef UB_NETFLIX_OFF
#define UB_KEYS_FIFO UB_KEYS_TEST_DIR "/keys"
#define UB_OVERLAY_OPEN UB_KEYS_TEST_DIR "/open"
#define UB_OVERLAY_ABORT UB_KEYS_TEST_DIR "/abort"
#define UB_NETFLIX_HELPER UB_KEYS_TEST_DIR "/helper"
#define UB_NETFLIX_OFF UB_KEYS_TEST_DIR "/off"
static uint32_t seconds=100;
static int mode,calls,stock_calls,reader=-1,close_reader;
static int test_clock(clockid_t id,struct timespec *out){assert(id==CLOCK_MONOTONIC);out->tv_sec=seconds;out->tv_nsec=0;return 0;}
static ssize_t test_write(int fd,const void *buf,size_t n){
 calls++;
 if(mode==1&&calls==1){errno=EINTR;return -1;}
 if(mode==2){errno=EAGAIN;return -1;}
 if(mode==3)return 2;
 return write(fd,buf,n);
}
static int test_open(const char *path,int flags,...){
 int create_mode=0;
 if(flags&O_CREAT){va_list ap;va_start(ap,flags);create_mode=va_arg(ap,int);va_end(ap);}
 if(close_reader&&!strncmp(path,"/proc/self/fd/",14)&&(flags&(O_ACCMODE|O_NONBLOCK))==O_NONBLOCK){close(reader);reader=-1;close_reader=0;}
 return open(path,flags,create_mode);
}
#define clock_gettime test_clock
#define write test_write
#define open test_open
#include "../modules/remote/keys.c"
#undef open
#undef write
#undef clock_gettime
static int stock(int *ctx){(void)ctx;stock_calls++;return 42;}
static void lease(uint32_t expires){int fd=open(UB_OVERLAY_OPEN,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);assert(write(fd,&expires,sizeof expires)==sizeof expires);close(fd);}
int main(void){
 int ctx[64]={0};uint32_t key;
 mkdir(UB_KEYS_TEST_DIR,0700);unlink(UB_KEYS_FIFO);assert(!mkfifo(UB_KEYS_FIFO,0600));
 ctx[CTX_EVENT]=1;ctx[CTX_KEY]=0x85;ub_original_key_filter=stock;
 unlink(UB_OVERLAY_OPEN);assert(ub_hook_key_filter(ctx)==42&&stock_calls==1);
 reader=open(UB_KEYS_FIFO,O_RDONLY|O_NONBLOCK);assert(reader>=0);lease(103);
 assert(ub_hook_key_filter(ctx)==1&&stock_calls==1);assert(read(reader,&key,sizeof key)==sizeof key&&key==0x85);
 mode=1;calls=0;assert(ub_hook_key_filter(ctx)==1&&calls==2);assert(read(reader,&key,sizeof key)==sizeof key);
 mode=2;calls=0;assert(ub_hook_key_filter(ctx)==1&&calls==1);assert(!access(UB_OVERLAY_ABORT,F_OK));assert(stock_calls==1);unlink(UB_OVERLAY_ABORT);
 mode=3;assert(to_overlay(0x85,1)==-1); /* incomplete record is never success */
 mode=0;seconds=104;assert(ub_hook_key_filter(ctx)==42&&stock_calls==2); /* stale UI cannot trap remote */
 seconds=100;lease(103);close_reader=1;assert(to_overlay(0x85,1)==0&&reader==-1); /* reader disappears: no SIGPIPE */
 assert(to_overlay(0x85,1)==-1); /* no external reader at initial open */
 unlink(UB_KEYS_FIFO);unlink(UB_OVERLAY_OPEN);unlink(UB_OVERLAY_ABORT);rmdir(UB_KEYS_TEST_DIR);
 puts("keys_delivery_test passed");return 0;
}
