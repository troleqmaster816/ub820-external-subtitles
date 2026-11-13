#ifndef UB_PAYLOAD_TIME_H
#define UB_PAYLOAD_TIME_H
/* Payloads bind to the player's glibc 2.20, not the cross compiler's libc.
 * New ARM headers redirect clock_gettime to __clock_gettime64 by default.
 * Bind the legacy symbol with its actual 32-bit timespec ABI explicitly. */
#include <stdint.h>
#ifdef UB_HOST_TEST
#include <time.h>
static inline int ub_payload_now(uint32_t *seconds){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;*seconds=(uint32_t)t.tv_sec;return 0;}
static inline int ub_payload_now_ms(uint64_t *ms){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;*ms=(uint64_t)t.tv_sec*1000+(uint32_t)t.tv_nsec/1000000u;return 0;}
#else
struct ub_payload_time32 {int32_t sec,nsec;};
extern int ub_payload_clock_gettime32(int,struct ub_payload_time32 *) __asm__("clock_gettime");
static inline int ub_payload_now(uint32_t *seconds){struct ub_payload_time32 t;if(ub_payload_clock_gettime32(1,&t))return -1;*seconds=(uint32_t)t.sec;return 0;}
static inline int ub_payload_now_ms(uint64_t *ms){struct ub_payload_time32 t;if(ub_payload_clock_gettime32(1,&t))return -1;/* Exact for the timespec range 0<=nsec<1e9; no ARM division helper. */*ms=(uint64_t)(uint32_t)t.sec*1000+(((uint64_t)(uint32_t)t.nsec*1125899907ULL)>>50);return 0;}
#endif
#endif
