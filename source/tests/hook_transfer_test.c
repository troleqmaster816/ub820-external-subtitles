/* Own encoded jumps and owned anonymous pages only. No target process/device. */
#define _GNU_SOURCE
#include "../core/injector.c"
#include <assert.h>
static int answer(void){return 73;}
int main(void){
 unsigned char bytes[32],near[4];uint32_t address;
 assert(!branch_thumb(0x10000,0x20001,near));
 assert(!hook_transfer(0x10000,0x20001,0x33000001,4,bytes)&&!memcmp(bytes,near,4));
 assert(!hook_transfer(0x10000,0x20001,0x33000001,8,bytes)&&!memcmp(bytes,near,4));
 assert(bytes[4]==0&&bytes[5]==0xbf&&bytes[6]==0&&bytes[7]==0xbf);
 assert(hook_transfer(0x1d9b8,0x28800001,0x33000001,4,bytes)==-1);
 assert(hook_transfer(0x1d9b8,0x28800001,0x33000001,6,bytes)==-1);
 assert(!hook_transfer(0x1d9b8,0x28800001,0x33000001,8,bytes));
 assert(!memcmp(bytes,"\xdf\xf8\x00\xf0",4));memcpy(&address,bytes+4,4);assert(address==0x33000001);
 assert(!hook_transfer(0x1d9b8,0x28800001,0x33000001,12,bytes));
 assert(bytes[8]==0&&bytes[9]==0xbf&&bytes[10]==0&&bytes[11]==0xbf);
 assert(hook_transfer(0x1d9ba,0x20001,0x33000001,8,bytes)==-1);
 assert(hook_transfer(0x1d9b8,0x28800001,0,8,bytes)==-1);
 assert(hook_transfer(0x1d9b8,0x28800001,0x33000001,33,bytes)==-1);
#ifdef __arm__
 unsigned char *page=mmap(0,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(page!=MAP_FAILED);uint32_t from=(uint32_t)(uintptr_t)page,to=(uint32_t)(uintptr_t)answer;
 /* Force distant gateway, then execute the actual absolute Thumb transfer. */
 uint32_t distant=from>0x10000000u?0x10001u:0x28800001u;
 assert(!hook_transfer(from,distant,to,8,bytes));memcpy(page,bytes,8);
 __builtin___clear_cache((char *)page,(char *)page+8);
 assert(((int (*)(void))((uintptr_t)page|1u))()==73);
 /* Actual nearby B.W -> gateway -> the same compiled Thumb function. */
 assert(!hook_transfer(from,(from+64)|1u,to,4,bytes));memcpy(page,bytes,4);
 memcpy(page+64,"\xdf\xf8\x00\xf0",4);memcpy(page+68,&to,4);
 __builtin___clear_cache((char *)page,(char *)page+72);
 assert(((int (*)(void))((uintptr_t)page|1u))()==73);munmap(page,4096);
#else
 (void)answer;
#endif
 puts("hook_transfer_test passed");return 0;
}
