#ifndef UB_POWER_PROTOCOL_H
#define UB_POWER_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define UB_POWER_QUEUE UINT32_C(0x90c35100)
#define UB_POWER_SENDER UINT32_C(0x55425057)
#define UB_POWER_MAGIC UINT32_C(0x55425057)
#define UB_POWER_VERSION 1u
#define UB_POWER_TOKEN "/tmp/ub820_power.token"
#define UB_POWER_NATIVE "/tmp/ub820_power.native"
#define UB_POWER_PACKET "/tmp/ub820_power.packet"
#define UB_POWER_TOOL "/tmp/.ub820_events"
struct ub_power_packet {
 uint32_t magic,version;
 unsigned char token[16];
 uint32_t seq,auto_off,expires,reserved;
};
_Static_assert(sizeof(struct ub_power_packet)==40,"Power event ABI");
enum { UB_POWER_OK=0,UB_POWER_REJECTED=1,UB_POWER_NATIVE_ERROR=2,UB_POWER_EXPIRED=3,
       UB_POWER_EEP_ERROR=4,UB_POWER_FPC_ERROR=5,UB_POWER_CONTEXT_ERROR=6,UB_POWER_IO_ERROR=7 };
#define UB_POWER_NATIVE_OK UB_POWER_OK
#define UB_POWER_NATIVE_REJECT UB_POWER_REJECTED
#define UB_POWER_NATIVE_EXPIRED UB_POWER_EXPIRED
#define UB_POWER_NATIVE_EEP UB_POWER_EEP_ERROR
#define UB_POWER_NATIVE_FPC UB_POWER_FPC_ERROR
#define UB_POWER_NATIVE_CONTEXT UB_POWER_CONTEXT_ERROR
#define UB_POWER_NATIVE_IO UB_POWER_IO_ERROR
static inline int ub_power_packet_valid(const struct ub_power_packet *p,const unsigned char token[16],uint32_t now){
 return p&&p->magic==UB_POWER_MAGIC&&p->version==UB_POWER_VERSION&&
        p->seq&&p->auto_off<=1&&!p->reserved&&!memcmp(p->token,token,16)&&
        p->expires&&(int32_t)(now-p->expires)<0;
}
static inline void ub_power_token_hex(char out[33],const unsigned char token[16]){
 static const char hex[]="0123456789abcdef";
 for(unsigned i=0;i<16;i++){out[2*i]=hex[token[i]>>4];out[2*i+1]=hex[token[i]&15];}
 out[32]=0;
}
#endif
