#ifndef UB_SUBTITLE_PROTOCOL_H
#define UB_SUBTITLE_PROTOCOL_H
#include <stdint.h>
#define UB_MAGIC 0x31534255u
#define UB_VERSION 1u
#define UB_MAX_ITEMS 64u
#define UB_PATH_BYTES 256u
#define UB_PATH_LIMIT 250u
#define UB_NAME_BYTES 128u
#define UB_REQUEST_FILE "/tmp/ub820_submenu.request"
#define UB_STATUS_FILE "/tmp/ub820_submenu.status"
#define UB_OP_LIST 1u
#define UB_OP_SELECT 2u
#define UB_OP_OFF 3u
#define UB_OP_ON 4u
#define UB_OP_RELOAD 5u
#define UB_KIND_FILE 1u
#define UB_KIND_DIR 2u
#define UB_OK 0
#define UB_BUSY 1
#define UB_NO_ENGINE 2
#define UB_BAD_PATH 3
#define UB_IO_ERROR 4
#define UB_PARSE_ERROR 5
#define UB_STALE 6
#define UB_LIMIT 7
struct ub_item { uint32_t kind; char name[UB_NAME_BYTES]; char path[UB_PATH_BYTES]; };
struct ub_request {
 uint32_t magic,version,seq,epoch,op;
 char path[UB_PATH_BYTES];
};
struct ub_status {
 uint32_t magic,version,seq,epoch;
 int32_t result;
 uint32_t enabled,active,count;
 char current[UB_PATH_BYTES],dir[UB_PATH_BYTES],message[128];
 struct ub_item items[UB_MAX_ITEMS];
};
#endif
