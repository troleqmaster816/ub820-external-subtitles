#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
static uintptr_t test_caller;
#define __builtin_return_address(depth) ((void *)test_caller)
#include "../modules/native_events/medm_wait.c"
#undef __builtin_return_address
static unsigned interruptions,calls,deliveries;
static int terminal;
static int wait_api(uint32_t queue,void *out){
 calls++;assert(queue==0x90000008);
 if(interruptions){interruptions--;errno=EINTR;return EINTR;}
 if(terminal)return terminal;
 *(uint32_t *)out=4;deliveries++;return 0;
}
int main(void){
 uint32_t out=2;const uintptr_t sites[]={0x268d8,0x26a78,0x26b0a,0x29404,0x2940e,0x2f248,0x2f338};
 ub_original_medm_wait=wait_api;
 for(unsigned i=0;i<7;i++){
  test_caller=sites[i]|1u;out=2;interruptions=3;calls=deliveries=0;terminal=0;errno=EAGAIN;
  assert(!ub_hook_medm_wait(0x90000008,&out)&&calls==4&&deliveries==1&&out==4&&errno==EAGAIN);
  interruptions=2;calls=0;terminal=2;assert(ub_hook_medm_wait(0x90000008,&out)==2&&calls==3);
  interruptions=0;calls=0;terminal=7;assert(ub_hook_medm_wait(0x90000008,&out)==7&&calls==1);
 }
 test_caller=0x123456;interruptions=2;calls=0;assert(ub_hook_medm_wait(0x90000008,&out)==4&&calls==1);
 puts("native_medm_wait_test passed: seven PCA/cache callers, unrelated pass-through, one completion, ENOENT/EIO and errno");return 0;
}
