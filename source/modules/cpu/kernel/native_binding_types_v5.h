#ifndef UB_NATIVE_BINDING_COMPILE_TYPES_H
#define UB_NATIVE_BINDING_COMPILE_TYPES_H
/* Private offline prototype types. Not an activation/module interface. */
#include <linux/types.h>
typedef int (*ub_verified_patch_word_fn)(void *,u32);
struct ub_native_byte_guard {
 const char *symbol;
 unsigned long address;
 const u8 *expected;
 unsigned length;
};
#endif
