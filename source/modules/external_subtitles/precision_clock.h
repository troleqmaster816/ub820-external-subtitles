#ifndef UB_SUBTITLE_PRECISION_CLOCK_H
#define UB_SUBTITLE_PRECISION_CLOCK_H
#include <stddef.h>
#include <stdint.h>

/* Local datagram ABI: explicitly sized words, identical on ARM32 and hosts.
 * One datagram is one coherent snapshot. The receiver must obtain pid/uid from
 * SCM_CREDENTIALS, never from the record alone. All times use CLOCK_MONOTONIC
 * milliseconds on the same kernel. Transport must be nonblocking and bounded.
 * This header owns no threads, syscalls, shared volatile data or interpolation.
 */
#define UB_PC_MAGIC 0x31435055u
#define UB_PC_REQUEST_MAGIC 0x31515055u
#define UB_PC_VERSION 2u
#define UB_PC_VALID 1u
#define UB_PC_PAUSED 2u
#define UB_PC_EVENT 4u
#define UB_PC_GET 1u
#define UB_PC_STOP 2u
#define UB_PC_TICKS_PER_SECOND 45000u
struct ub_pc_record {
 uint32_t magic,version,bytes,pid,instance_lo,instance_hi,sequence;
 uint32_t client_nonce_lo,client_nonce_hi,request_sequence;
 uint32_t monotonic_lo,monotonic_hi,ticks_lo,ticks_hi,flags,native_generation;
};
struct ub_pc_request {
 uint32_t magic,version,bytes,client_nonce_lo,client_nonce_hi,request_sequence,op;
};
_Static_assert(sizeof(struct ub_pc_record)==64,"precision clock response layout");
_Static_assert(sizeof(struct ub_pc_request)==28,"precision clock request layout");

struct ub_pc_state {
 uint64_t barrier_ms,instance,monotonic_ms,ticks;
 uint32_t pid,sequence,have_packet,valid,paused,native_generation;
};
static inline uint64_t ub_pc_u64(uint32_t lo,uint32_t hi) {
 return (uint64_t)lo|((uint64_t)hi<<32);
}
static inline void ub_pc_pack(struct ub_pc_record *r,uint32_t pid,uint64_t instance,
 uint32_t sequence,uint64_t client_nonce,uint32_t request_sequence,
 uint64_t monotonic_ms,uint64_t ticks,uint32_t valid) {
 r->magic=UB_PC_MAGIC;r->version=UB_PC_VERSION;r->bytes=sizeof(*r);r->pid=pid;
 r->instance_lo=(uint32_t)instance;r->instance_hi=(uint32_t)(instance>>32);
 r->sequence=sequence;r->client_nonce_lo=(uint32_t)client_nonce;
 r->client_nonce_hi=(uint32_t)(client_nonce>>32);r->request_sequence=request_sequence;
 r->monotonic_lo=(uint32_t)monotonic_ms;
 r->monotonic_hi=(uint32_t)(monotonic_ms>>32);
 r->ticks_lo=(uint32_t)ticks;r->ticks_hi=(uint32_t)(ticks>>32);
 r->flags=valid?UB_PC_VALID:0;r->native_generation=0;
}
static inline void ub_pc_request_pack(struct ub_pc_request *r,uint64_t client_nonce,
 uint32_t request_sequence) {
 r->magic=UB_PC_REQUEST_MAGIC;r->version=UB_PC_VERSION;r->bytes=sizeof(*r);
 r->client_nonce_lo=(uint32_t)client_nonce;r->client_nonce_hi=(uint32_t)(client_nonce>>32);
 r->request_sequence=request_sequence;r->op=UB_PC_GET;
}
static inline int ub_pc_request_valid(const struct ub_pc_request *r,size_t bytes) {
 return bytes==sizeof(*r)&&r->magic==UB_PC_REQUEST_MAGIC&&r->version==UB_PC_VERSION&&
  r->bytes==sizeof(*r)&&ub_pc_u64(r->client_nonce_lo,r->client_nonce_hi)!=0&&
  (r->op==UB_PC_GET||r->op==UB_PC_STOP);
}
/* Call at local renderer/session invalidation, before accepting new samples.
 * Same-millisecond samples are allowed: the request nonce and sequence provide
 * exact correlation while timestamps have only millisecond resolution.
 * The caller must also change its nonzero client nonce on every lifecycle
 * reset; the echoed nonce rejects earlier replies even within one millisecond.
 * Coarse alignment below is a sanity check, not proof of title identity.
 */
static inline void ub_pc_reset(struct ub_pc_state *s,uint64_t now_ms) {
 s->barrier_ms=now_ms;s->instance=0;s->monotonic_ms=0;s->ticks=0;
 s->pid=0;s->sequence=0;s->have_packet=0;s->valid=0;s->paused=0;s->native_generation=0;
}
enum ub_pc_result {
 UB_PC_ACCEPTED=1,UB_PC_BAD_RECORD=-1,UB_PC_BAD_PEER=-2,
 UB_PC_STALE=-3,UB_PC_OLD_SEQUENCE=-4,UB_PC_WRONG_REQUEST=-5
};
/* Rejection leaves the previous state intact. An accepted flags=0 record
 * explicitly invalidates the sample, while retaining its replay watermark.
 * instance must change whenever the producer restarts its sequence counter.
 * Within an instance, sequence ordering assumes fewer than 2^31 lost records.
 */
static inline int ub_pc_accept(struct ub_pc_state *s,const struct ub_pc_record *r,
 size_t bytes,uint32_t peer_pid,uint32_t peer_uid,uint32_t expected_pid,
 uint32_t expected_uid,uint64_t expected_client_nonce,uint32_t expected_request_sequence,
 uint64_t now_ms,uint32_t max_age_ms) {
 uint64_t stamp,instance;
 if(bytes!=sizeof(*r)||r->magic!=UB_PC_MAGIC||r->version!=UB_PC_VERSION||
    r->bytes!=sizeof(*r)||(r->flags&~(UB_PC_VALID|UB_PC_PAUSED))||
    ((r->flags&UB_PC_PAUSED)&&!(r->flags&UB_PC_VALID)))return UB_PC_BAD_RECORD;
 instance=ub_pc_u64(r->instance_lo,r->instance_hi);
 if(!instance)return UB_PC_BAD_RECORD;
 if(!expected_pid||peer_pid!=expected_pid||r->pid!=peer_pid||
    peer_uid!=expected_uid)return UB_PC_BAD_PEER;
 if(!expected_client_nonce||
    ub_pc_u64(r->client_nonce_lo,r->client_nonce_hi)!=expected_client_nonce||
    r->request_sequence!=expected_request_sequence)return UB_PC_WRONG_REQUEST;
 stamp=ub_pc_u64(r->monotonic_lo,r->monotonic_hi);
 if(stamp<s->barrier_ms||stamp>now_ms||now_ms-stamp>max_age_ms)
  return UB_PC_STALE;
 if(s->have_packet) {
  if(s->pid==r->pid&&s->instance==instance) {
   uint32_t delta=r->sequence-s->sequence;
   if(!delta||delta>=0x80000000u||stamp<s->monotonic_ms)
    return UB_PC_OLD_SEQUENCE;
  } else if(stamp<=s->monotonic_ms)return UB_PC_OLD_SEQUENCE;
 }
 s->instance=instance;s->monotonic_ms=stamp;
 s->ticks=ub_pc_u64(r->ticks_lo,r->ticks_hi);s->pid=r->pid;
 s->sequence=r->sequence;s->have_packet=1;s->valid=r->flags&UB_PC_VALID;
 s->paused=(r->flags&UB_PC_PAUSED)!=0;s->native_generation=r->native_generation;
 return UB_PC_ACCEPTED;
}
/* Return the latest received position verbatim, including backwards seeks and
 * repeated paused positions. Never advance it using elapsed wall time. Accept
 * at most one whole second of disagreement with the native coarse clock.
 * No 64-bit division: keep freestanding ARM payloads free of division helpers.
 */
static inline int ub_pc_sample(const struct ub_pc_state *s,uint64_t now_ms,
 uint32_t max_age_ms,uint32_t native_seconds,uint64_t *ticks) {
 uint64_t low,high;
 if(!s->have_packet||!s->valid||s->monotonic_ms>now_ms||
    now_ms-s->monotonic_ms>max_age_ms)return 0;
 low=(uint64_t)(native_seconds?native_seconds-1:0)*UB_PC_TICKS_PER_SECOND;
 high=((uint64_t)native_seconds+2)*UB_PC_TICKS_PER_SECOND;
 if(s->ticks<low||s->ticks>=high)return 0;
 *ticks=s->ticks;return 1;
}
#endif
