#ifndef UB_CPU_CONTROL_H
#define UB_CPU_CONTROL_H
#include <stdint.h>
#define UB_CPU_ARTIFACT_SHA256 "13d23505ec9585aa4eacfdfcd592f4708814a4e897c2aa01e7bcad320a36be70"
#define UB_CPU_ARTIFACT_BYTES 101336u
#define UB_CPU_MODULE_NAME "ub820_cpu_wait_v15"
#ifndef UB_CPU_DIR
#define UB_CPU_DIR "/tmp/.ub820_cpu"
#endif
#ifndef UB_CPU_NODE
#define UB_CPU_NODE "/dev/ub820_cpu_wait"
#endif
#ifndef UB_CPU_MODULES
#define UB_CPU_MODULES "/proc/modules"
#endif
#define UB_CPU_MAGIC UINT32_C(0x55424350)
#define UB_CPU_VERSION 1u
enum { CPU_QUERY=0,CPU_OFF=1,CPU_ON=2,CPU_RECONCILE=3 };
struct cpu_owner {
 uint32_t magic,version,pid,enabled;uint64_t born;
 uint32_t desired,reserved;
 unsigned char token[16];char boot[40];
};
struct cpu_job {
 uint32_t magic,version,operation,manual;uint64_t deadline;
 struct cpu_owner owner;unsigned char nonce[16];
};
struct cpu_receipt {
 uint32_t magic,version,size,reserved;uint64_t base;
 uint32_t pid[2];uint64_t born[2];char boot[40],sha[65];
 int32_t preference;uint32_t save_pending;unsigned char preference_nonce[16];
};
struct cpu_latch {
 uint32_t magic,version,pending,operation;char boot[40];unsigned char nonce[16];
};
struct cpu_result {
 uint32_t magic,version;int32_t mode,code;uint32_t suppress_auto_on,off_attempted;
 int32_t preference;unsigned char preference_nonce[16];
 unsigned char nonce[16];char message[160];
};
struct cpu_native {
 unsigned phase,pinned,enabled;int rc,fault,begin_rc;
 unsigned pid[2],bypass[2],rejected[2];char build[8];
};
struct cpu_instance { uint64_t base;unsigned size,references; };

/* Decisions shared by the supervisor and the isolated worker. No I/O here. */
enum { CPU_MODE_UNKNOWN=-1,CPU_MODE_OFF=0,CPU_MODE_ON=1 };
enum { CPU_OK=0,CPU_BUSY=1,CPU_INVALID=2,CPU_UNCERTAIN=3,CPU_STALE=4 };
static inline int cpu_owner_equal(const struct cpu_owner *a,const struct cpu_owner *b){
 unsigned diff=0;
 if(a->pid!=b->pid||a->born!=b->born)return 0;
 for(unsigned i=0;i<16;i++)diff|=a->token[i]^b->token[i];
 for(unsigned i=0;i<40;i++)diff|=(unsigned char)a->boot[i]^(unsigned char)b->boot[i];
 return !diff;
}
static inline int cpu_on_allowed(const struct cpu_owner *now,const struct cpu_owner *job,int latched){
 return !latched&&now->enabled==1&&now->desired==1&&cpu_owner_equal(now,job);
}
static inline int cpu_completion_needs_off(const struct cpu_owner *now,int mode){
 return mode==CPU_MODE_ON&&(!now->enabled||!now->desired);
}
static inline int cpu_native_mode(const struct cpu_native *s){
 if(s->rc||s->fault||s->begin_rc||s->rejected[0]||s->rejected[1]||
    !s->pid[0]||!s->pid[1]||s->pid[0]==s->pid[1])return CPU_MODE_UNKNOWN;
 if((s->phase==0||s->phase==3)&&!s->enabled&&s->pinned==(s->phase==3)&&s->bypass[0]==1&&s->bypass[1]==1)return CPU_MODE_OFF;
 if(s->phase==1&&s->pinned==1&&s->enabled==1&&!s->bypass[0]&&!s->bypass[1])return CPU_MODE_ON;
 return CPU_MODE_UNKNOWN;
}
#endif
