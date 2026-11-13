/* One EV metadata/control message to the already-existing manager queue.
 * Reads only the fixed40-byte RAM packet. No UFS/FPC/EEP/media operation.
 * Exit0 means queued, NOT applied; the manager payload supplies native ACK. */
#include "power_protocol.h"

static int same(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}

#ifdef UB_EVENTS_TEST
extern int ub_events_test_read(struct ub_power_packet *);
extern int ub_events_test_now(uint32_t *);
extern int ub_events_test_send(const struct ub_power_packet *);
#define packet_read ub_events_test_read
#define packet_now ub_events_test_now
#define packet_send ub_events_test_send
#else
extern int open(const char *,int,...);
extern int read(int,void *,unsigned);
extern int close(int);
extern int *__errno_location(void);
extern int EV_send_message(uint32_t,unsigned,const void *,uint32_t);
struct time32 {int sec,nsec;};
extern long syscall(long,...);
#define READ_FLAGS (0x80000|0x20000|0x800) /* CLOEXEC, NOFOLLOW, NONBLOCK */
static int packet_read(struct ub_power_packet *packet){
 int fd=open(UB_POWER_PACKET,READ_FLAGS),n,rc=0;unsigned used=0;unsigned char extra;
 if(fd<0)return -1;
 while(used<sizeof *packet){
  do{n=read(fd,(unsigned char *)packet+used,(unsigned)sizeof *packet-used);}while(n<0&&*__errno_location()==4);
  if(n<=0){rc=-1;break;}used+=(unsigned)n;
 }
 if(!rc){do{n=read(fd,&extra,1);}while(n<0&&*__errno_location()==4);if(n!=0)rc=-1;}
 if(close(fd))rc=-1;
 return rc;
}
/* ARM EABI clock_gettime263 exists on the player's4.4 kernel. This avoids
 * a GLIBC_2.17 clock symbol and never probes newer time64 syscalls. */
static int packet_now(uint32_t *now){struct time32 t;if(syscall(263L,1,&t))return -1;*now=(uint32_t)t.sec;return 0;}
static int packet_send(const struct ub_power_packet *packet){return EV_send_message(UB_POWER_QUEUE,sizeof *packet,packet,UB_POWER_SENDER);}
#endif

int events_main(int argc,char **argv){
 struct ub_power_packet packet;uint32_t now;
 if(argc!=2||!same(argv[1],UB_POWER_PACKET))return 2;
 if(packet_read(&packet))return 3;
 if(packet.magic!=UB_POWER_MAGIC||packet.version!=UB_POWER_VERSION||!packet.seq||packet.auto_off>1||packet.reserved)return 4;
 if(packet_now(&now))return 5;
 if(!packet.expires||(int32_t)(now-packet.expires)>=0)return 6;
 return packet_send(&packet)?7:0;
}

#ifndef UB_EVENTS_TEST
/* The stock dynamic linker runs dependency initializers before this entry. */
__asm__(
 ".text\n.arm\n.align 2\n.global _start\n.type _start,%function\n"
 "_start:\n"
 " mov fp,#0\n"
 " mov lr,#0\n"
 " ldr r0,[sp]\n"
 " add r1,sp,#4\n"
 " bic sp,sp,#7\n"
 " blx events_main\n"
 " mov r7,#248\n"
 " svc #0\n"
);
#endif
