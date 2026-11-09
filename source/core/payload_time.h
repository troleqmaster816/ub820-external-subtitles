#ifndef UB_PAYLOAD_TIME_H
#define UB_PAYLOAD_TIME_H
/* Payloads bind to the player's glibc 2.20, not the cross compiler's libc.
 * New ARM headers redirect clock_gettime to __clock_gettime64 by default.
 * Bind the legacy symbol with its actual 32-bit timespec ABI explicitly. */
#include <stdint.h>
#ifdef UB_HOST_TEST
#include <time.h>
static int ub_payload_now(uint32_t *seconds){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;*seconds=(uint32_t)t.tv_sec;return 0;}
#else
struct ub_payload_time32 {int32_t sec,nsec;};
extern int ub_payload_clock_gettime32(int,struct ub_payload_time32 *) __asm__("clock_gettime");
static int ub_payload_now(uint32_t *seconds){struct ub_payload_time32 t;if(ub_payload_clock_gettime32(1,&t))return -1;*seconds=(uint32_t)t.sec;return 0;}
#endif
#endif
