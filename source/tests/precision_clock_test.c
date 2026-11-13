#include "../modules/external_subtitles/precision_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct ub_pc_record packet(uint32_t seq,uint64_t stamp,uint64_t ticks) {
 struct ub_pc_record r;ub_pc_pack(&r,71,0x123456789abcdef0ULL,seq,0x9876543210123456ULL,seq,stamp,ticks,1);return r;
}
static int accept(struct ub_pc_state *s,const struct ub_pc_record *r,uint64_t now) {
 return ub_pc_accept(s,r,sizeof(*r),71,0,71,0,0x9876543210123456ULL,r->request_sequence,now,250);
}
int main(void) {
 struct ub_pc_state s,before;struct ub_pc_record r;struct ub_pc_request request;uint64_t ticks=0;
 ub_pc_request_pack(&request,0x9876543210123456ULL,0xffffffffu);
 assert(ub_pc_request_valid(&request,sizeof(request)));
 assert(request.request_sequence==0xffffffffu);
 assert(!ub_pc_request_valid(&request,sizeof(request)-1));
 request.magic=UB_PC_MAGIC;assert(!ub_pc_request_valid(&request,sizeof(request)));
 ub_pc_request_pack(&request,0,1);assert(!ub_pc_request_valid(&request,sizeof(request)));
 ub_pc_reset(&s,1000);r=packet(0,1000,10*45000+12345);
 assert(accept(&s,&r,1000)==UB_PC_ACCEPTED); /* request and reply in the same ms */
 r=packet(1,1001,10*45000+12345);
 assert(accept(&s,&r,1010)==UB_PC_ACCEPTED);
 assert(ub_pc_sample(&s,1010,250,10,&ticks)&&ticks==10*45000+12345);
 /* No interpolation, and paused positions remain valid as new records arrive. */
 assert(ub_pc_sample(&s,1200,250,10,&ticks)&&ticks==10*45000+12345);
 r=packet(2,1200,ticks);assert(accept(&s,&r,1200)==UB_PC_ACCEPTED);
 assert(!ub_pc_sample(&s,1451,250,10,&ticks));
 assert(!ub_pc_sample(&s,1199,250,10,&ticks));
 /* Peer identity is authenticated externally; matching self-reported PID is insufficient. */
 before=s;r=packet(3,1201,ticks);
 assert(ub_pc_accept(&s,&r,sizeof(r),72,0,71,0,0x9876543210123456ULL,3,1201,250)==UB_PC_BAD_PEER);
 assert(ub_pc_accept(&s,&r,sizeof(r),71,1,71,0,0x9876543210123456ULL,3,1201,250)==UB_PC_BAD_PEER);
 assert(ub_pc_accept(&s,&r,sizeof(r),71,0,0,0,0x9876543210123456ULL,3,1201,250)==UB_PC_BAD_PEER);
 r.pid=72;assert(accept(&s,&r,1201)==UB_PC_BAD_PEER);assert(!memcmp(&s,&before,sizeof(s)));
 /* Requests are correlated to the latest local lifecycle, including same-ms resets. */
 r=packet(3,1201,ticks);
 assert(ub_pc_accept(&s,&r,sizeof(r),71,0,71,0,0x9876543210123457ULL,3,1201,250)==UB_PC_WRONG_REQUEST);
 assert(ub_pc_accept(&s,&r,sizeof(r),71,0,71,0,0x9876543210123456ULL,4,1201,250)==UB_PC_WRONG_REQUEST);
 assert(ub_pc_accept(&s,&r,sizeof(r),71,0,71,0,0,3,1201,250)==UB_PC_WRONG_REQUEST);
 assert(!memcmp(&s,&before,sizeof(s)));
 /* Datagram truncation, unknown versions/flags and malformed cookie are rejected. */
 r=packet(3,1201,ticks);assert(ub_pc_accept(&s,&r,sizeof(r)-1,71,0,71,0,0x9876543210123456ULL,3,1201,250)==UB_PC_BAD_RECORD);
 r.version++;assert(accept(&s,&r,1201)==UB_PC_BAD_RECORD);r=packet(3,1201,ticks);
 r.flags=8;assert(accept(&s,&r,1201)==UB_PC_BAD_RECORD);r=packet(3,1201,ticks);
 r.instance_lo=r.instance_hi=0;assert(accept(&s,&r,1201)==UB_PC_BAD_RECORD);
 assert(!memcmp(&s,&before,sizeof(s)));
 /* Future and stale timestamps, duplicates and out-of-order sequence cannot replace it. */
 r=packet(3,1202,ticks);assert(accept(&s,&r,1201)==UB_PC_STALE);
 r=packet(3,1201,ticks);assert(accept(&s,&r,1452)==UB_PC_STALE);
 r=packet(2,1201,ticks);assert(accept(&s,&r,1201)==UB_PC_OLD_SEQUENCE);
 r=packet(1,1201,ticks);assert(accept(&s,&r,1201)==UB_PC_OLD_SEQUENCE);
 r=packet(3,1199,ticks);assert(accept(&s,&r,1201)==UB_PC_OLD_SEQUENCE);
 assert(!memcmp(&s,&before,sizeof(s)));
 /* Full 64-bit positions survive transport; grossly different coarse clock fails closed. */
 ub_pc_reset(&s,2000);r=packet(0xfffffffeu,2001,((uint64_t)1<<32)+12345);
 assert(accept(&s,&r,2001)==UB_PC_ACCEPTED);
 assert(ub_pc_sample(&s,2001,250,95443,&ticks)&&ticks==((uint64_t)1<<32)+12345);
 assert(!ub_pc_sample(&s,2001,250,5,&ticks));
 r=packet(0xffffffffu,2002,11*45000);assert(accept(&s,&r,2002)==UB_PC_ACCEPTED);
 r=packet(0,2003,2*45000+9);assert(accept(&s,&r,2003)==UB_PC_ACCEPTED); /* wrap + backwards seek */
 assert(ub_pc_sample(&s,2003,250,2,&ticks)&&ticks==2*45000+9);
 r=packet(0xffffffffu,2004,11*45000);assert(accept(&s,&r,2004)==UB_PC_OLD_SEQUENCE);
 /* Explicit invalidation wins over prior valid data and also advances sequence. */
 r=packet(1,2004,0);r.flags=0;assert(accept(&s,&r,2004)==UB_PC_ACCEPTED);
 assert(!ub_pc_sample(&s,2004,250,0,&ticks));
 r=packet(0,2004,0);assert(accept(&s,&r,2004)==UB_PC_OLD_SEQUENCE);
 /* A new instance can reset sequence, but cannot replay an older timestamp. */
 r=packet(0,2003,0);r.instance_lo++;assert(accept(&s,&r,2004)==UB_PC_OLD_SEQUENCE);
 r=packet(0,2005,3*45000);r.instance_lo++;assert(accept(&s,&r,2005)==UB_PC_ACCEPTED);
 /* The barrier rejects earlier milliseconds; nonce/sequence reject old-session
  * replies even when legitimate new-session replies have the same timestamp. */
 ub_pc_reset(&s,2010);r=packet(10,2009,3*45000);assert(accept(&s,&r,2010)==UB_PC_STALE);
 r=packet(11,2010,3*45000);
 assert(ub_pc_accept(&s,&r,sizeof(r),71,0,71,0,0x9876543210123457ULL,11,2010,250)==UB_PC_WRONG_REQUEST);
 assert(accept(&s,&r,2010)==UB_PC_ACCEPTED);
 r=packet(12,2011,3*45000);assert(accept(&s,&r,2011)==UB_PC_ACCEPTED);
 /* Exact alignment boundaries: neighbouring whole seconds allowed, two apart rejected. */
 r=packet(13,2012,9*45000);assert(accept(&s,&r,2012)==UB_PC_ACCEPTED);
 assert(ub_pc_sample(&s,2012,250,10,&ticks));
 r=packet(14,2013,12*45000-1);assert(accept(&s,&r,2013)==UB_PC_ACCEPTED);
 assert(ub_pc_sample(&s,2013,250,10,&ticks));
 r=packet(15,2014,12*45000);assert(accept(&s,&r,2014)==UB_PC_ACCEPTED);
 assert(!ub_pc_sample(&s,2014,250,10,&ticks));
 /* Above 2^32 monotonic milliseconds: no truncation/wrap at ~49 days uptime. */
 ub_pc_reset(&s,((uint64_t)1<<32)+2014);
 r=packet(1,((uint64_t)1<<32)+2015,0);assert(accept(&s,&r,((uint64_t)1<<32)+2015)==UB_PC_ACCEPTED);
 assert(ub_pc_sample(&s,((uint64_t)1<<32)+2015,0,0,&ticks)&&!ticks);
 r=packet(2,((uint64_t)1<<32)+2016,0);r.flags=UB_PC_VALID|UB_PC_PAUSED;r.native_generation=17;
 assert(accept(&s,&r,((uint64_t)1<<32)+2016)==UB_PC_ACCEPTED);
 assert(s.paused&&s.native_generation==17);
 r=packet(3,((uint64_t)1<<32)+2017,0);r.flags=UB_PC_PAUSED;
 assert(accept(&s,&r,((uint64_t)1<<32)+2017)==UB_PC_BAD_RECORD);
 puts("precision_clock_test passed");return 0;
}
