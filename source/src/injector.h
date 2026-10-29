#ifndef UB_INJECTOR_H
#define UB_INJECTOR_H
#include <stdint.h>
#include <stddef.h>
struct ub_hook_spec {
 uint32_t target,length;
 const char *replacement,*original_pointer;
 const unsigned char *expected,*trampoline;
 uint32_t trampoline_length;
};
struct ub_binding { const char *symbol; uint32_t value; };
struct ub_payload {
 const char *process,*sha256;
 const unsigned char *elf; size_t elf_size;
 const struct ub_hook_spec *hooks; unsigned hook_count;
 const struct ub_binding *bindings; unsigned binding_count;
 uint32_t gateway_hint;
};
int ub_install_payload(int pid,const struct ub_payload *payload);
int ub_hash_file(const char *path,char result[65]);
#endif
