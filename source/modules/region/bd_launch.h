#ifndef UB_BD_LAUNCH_H
#define UB_BD_LAUNCH_H
#include <stdint.h>
#ifndef UB_BD_RAM_DIR
#define UB_BD_RAM_DIR "/tmp/.ub820_region"
#endif
#define UB_BD_PRELOAD UB_BD_RAM_DIR "/libbd_preload.so"
#define UB_BD_LAUNCH_CONFIG UB_BD_RAM_DIR "/launch.cfg"
#define UB_BD_LAUNCH_MAGIC 0x31445242u
struct ub_bd_launch_config { uint32_t magic,version; int32_t policy; uint32_t reserved; };
static int ub_bd_valid_policy(int p){return p==-1||p==0||p==1||p==2||p==4||p==7;}
#endif
