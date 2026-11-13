#ifndef UB_SUBTITLE_PROTOCOL_H
#define UB_SUBTITLE_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include "brightness_protocol.h"
#define UB_MAGIC 0x31534255u
#define UB_VERSION 2u
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
#define UB_OP_SET_TEXT_LEVEL 6u
#define UB_OP_BRIGHTNESS_BEGIN 7u
#define UB_OP_BRIGHTNESS_PREVIEW 8u
#define UB_OP_BRIGHTNESS_COMMIT 9u
#define UB_OP_BRIGHTNESS_CANCEL 10u
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
 union {
  char path[UB_PATH_BYTES];
  struct {uint32_t value;unsigned char reserved[UB_PATH_BYTES-sizeof(uint32_t)];} text_level;
  struct {
   uint32_t edit,value,owner_pid,owner_start_lo,owner_start_hi;
   unsigned char token[16],reserved[220];
  } brightness;
 };
};
/* A typed new operation without changing the established file-operation ABI.
 * Text-level requests require value0..255 and every reserved byte zero. */
_Static_assert(sizeof(struct ub_request)==276,"subtitle request ABI");
_Static_assert(offsetof(struct ub_request,path)==20,"subtitle path ABI");
_Static_assert(offsetof(struct ub_request,text_level)==20,"subtitle text-level union ABI");
_Static_assert(sizeof(((struct ub_request *)0)->brightness)==UB_PATH_BYTES,"brightness request ABI");
struct ub_status {
 uint32_t magic,version,seq,epoch;
 int32_t result;
 uint32_t enabled,active,count;
 char current[UB_PATH_BYTES],dir[UB_PATH_BYTES],message[128];
 struct ub_item items[UB_MAX_ITEMS];
 /* V2 readback: native actual state, editing ownership and an explicit
  * successful COMMIT receipt. Preview alone never authorizes persistence. */
 uint32_t text_level,brightness_percent,brightness_known,brightness_available;
 uint32_t brightness_edit,brightness_edit_epoch,brightness_base_raw,brightness_phase;
 uint32_t brightness_owner_pid,brightness_owner_start_lo,brightness_owner_start_hi;
 uint32_t brightness_commit_seq,brightness_commit_epoch,brightness_commit_percent,brightness_commit_level;
 uint32_t brightness_pid,brightness_birth_lo,brightness_birth_hi;
 unsigned char brightness_token[16];
};
#endif
