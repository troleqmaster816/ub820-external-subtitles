/* Audited indefinite public API waits only. Caller PCs name the instruction
 * after BL/BLX, with the Thumb bit removed. No ioctl/request replay. */
#ifndef UB_NATIVE_WAIT_SCOPE_H
#define UB_NATIVE_WAIT_SCOPE_H
#include <stdint.h>
enum { NATIVE_WAIT_DB=1,NATIVE_WAIT_SETTINGS=2,NATIVE_WAIT_UFS=3 };
#define UB_NATIVE_SETTINGS_FLAG 0xa0c35301u
static inline int native_medm_wait_site(uintptr_t pc){
 switch(pc&~(uintptr_t)1){
 case 0x268d8:case 0x26a78:case 0x26b0a:case 0x29404:
 case 0x2940e:case 0x2f248:case 0x2f338:return 1;
 default:return 0;
 }
}
static inline int native_ufs_wait_site(uintptr_t pc,uintptr_t base){
 if(!base||pc<base)return 0;
 switch((pc&~(uintptr_t)1)-base){
 /* libufs PT_LOAD starts at 0x28330000 in both verified cohorts. */
 case 0x52be:case 0x1313c:case 0x5e81c:case 0x5f052:return 1;
 default:return 0;
 }
}
#endif
