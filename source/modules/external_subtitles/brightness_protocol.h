#ifndef UB_SUBTITLE_BRIGHTNESS_PROTOCOL_H
#define UB_SUBTITLE_BRIGHTNESS_PROTOCOL_H
#include <stdint.h>
#ifndef UB_BRIGHTNESS_SEED
#define UB_BRIGHTNESS_SEED "/tmp/ub820_subtitle_brightness.seed"
#endif
#ifndef UB_BRIGHTNESS_SAVE_ACK
#define UB_BRIGHTNESS_SAVE_ACK "/tmp/ub820_subtitle_brightness.saved"
#endif
#ifndef UB_BRIGHTNESS_SAVE_CLAIM
#define UB_BRIGHTNESS_SAVE_CLAIM "/tmp/ub820_subtitle_brightness.claim"
#endif
/* Backend alone publishes an accepted COMMIT notice (result=-1). Supervisor
 * alone writes seed/claim/save-ACK. UI only uses the existing request channel. */
#ifndef UB_BRIGHTNESS_COMMIT_NOTICE
#define UB_BRIGHTNESS_COMMIT_NOTICE "/tmp/ub820_subtitle_brightness.commit"
#endif
#define UB_BRIGHTNESS_SEED_MAGIC UINT32_C(0x32434255)
#define UB_BRIGHTNESS_SAVE_MAGIC UINT32_C(0x32524255)
#define UB_BRIGHTNESS_PROTOCOL 2u
#define UB_BRIGHTNESS_NONE 0u
#define UB_BRIGHTNESS_PREVIEW 1u
#define UB_BRIGHTNESS_COMMIT 2u
enum ub_brightness_save_result {
 UB_BRIGHTNESS_SAVED=0,UB_BRIGHTNESS_SAVE_ERROR=1,
 UB_BRIGHTNESS_SAVE_STALE=2,UB_BRIGHTNESS_SAVE_UNKNOWN=3
};
struct ub_brightness_seed {
 uint32_t magic,version;
 unsigned char token[16];
 uint32_t pid,birth_lo,birth_hi,percent;
};
struct ub_brightness_save_ack {
 uint32_t magic,version;
 unsigned char token[16];
 uint32_t pid,birth_lo,birth_hi,edit,epoch,seq,percent,level;
 int32_t result;
};
_Static_assert(sizeof(struct ub_brightness_seed)==40,"brightness seed ABI");
_Static_assert(sizeof(struct ub_brightness_save_ack)==60,"brightness save ABI");
/* Relative RGB code intensity, not calibrated luminance/nits. Exact constant
 * reciprocals avoid unsupported __aeabi_uidiv imports in the ARM payload. */
static inline unsigned ub_brightness_level(unsigned percent){
 return (unsigned)(((uint64_t)(percent*255u+50u)*UINT64_C(0x51eb851f))>>37);
}
static inline unsigned ub_brightness_percent(unsigned level){
 return (unsigned)(((uint64_t)(level*100u+127u)*UINT64_C(0x80808081))>>39);
}
#endif
