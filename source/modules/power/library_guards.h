#ifndef UB_POWER_LIBRARY_GUARDS_H
#define UB_POWER_LIBRARY_GUARDS_H
#include <stdint.h>
struct ub_power_library_guard {const char *name;uint32_t base;const char *sha182,*sha185;};
static const struct ub_power_library_guard ub_power_library_guards[]={
 {"libev.so",UINT32_C(0x281e0000),
  "e5391f9728f1b6e3e30f61e6b1672eef9f43ee692694b8b6b513feaf23969c8d",
  "d48123aecb85995a14cb762b7e1ba6e43a6ae49f973ebd5b6307648f3584844d"},
 {"libeep.so",UINT32_C(0x282a0000),
  "f96b86235dc43bd0e91d8c36616c3117cdfb98986faf863097684513799994fc",
  "95392e85df97d3692f0e7f74f048fe225da7950ff99cada38d1c493064b3c440"},
 {"libfpc.so",UINT32_C(0x29080000),
  "68994b0338236c9cd91fa734b30e56553b328512538d54ce7544a9ec9c1c269b",
  "848cf84d13295b84b6de1562dab2fe162a5831276efb38506d7668d2822fc5f1"}
};
#define UB_POWER_LIBRARY_GUARD_COUNT (sizeof ub_power_library_guards/sizeof ub_power_library_guards[0])
#endif
