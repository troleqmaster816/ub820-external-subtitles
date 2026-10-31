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
/* One firmware build a payload can go into: that executable's exact hash and
 * its addresses (hooks with their expected bytes and trampolines, bindings,
 * where the gateway page can go). The payload's code is the same for all. */
struct ub_payload_variant {
 const char *firmware,*sha256;
 const struct ub_hook_spec *hooks; unsigned hook_count;
 const struct ub_binding *bindings; unsigned binding_count;
 uint32_t gateway_hint;
};
struct ub_payload {
 const char *process,*sha256;          /* sha256: the 1.82 pin, variants[0] */
 const unsigned char *elf; size_t elf_size;
 const struct ub_payload_variant *variants; unsigned variant_count;
};
/* The variant for an executable whose SHA-256 is sha: one with that exact
 * hash, or the 1.82 variant for a 1.85 build whose code is byte-identical to
 * 1.82 (ub_firmware_injectable); NULL if none, then nothing is installed. */
const struct ub_payload_variant *ub_payload_variant(const struct ub_payload *payload,const char *sha);
int ub_install_payload(int pid,const struct ub_payload *payload);
/* The phase ub_install_payload was in at its last return; names the step a
 * retry stalled on (e.g. "stop", "thread-busy", "base-mmap", "gateway"). */
const char *ub_install_stage(void);
/* Diagnostic one-shot injection into pid; logs the result. Returns the remote
 * syscall rc. See `engage --probe`. */
int ub_probe(int pid);
/* 0 replaced (or already replaced), 1 current bytes match neither, negative:
 * the step that failed; the target is unchanged unless the swap succeeded. */
enum { UB_OVERLAY_ARGS=-1, UB_OVERLAY_STOP=-2, UB_OVERLAY_READ=-3, UB_OVERLAY_MAP=-4,
       UB_OVERLAY_FILL=-5, UB_OVERLAY_SEAL=-6, UB_OVERLAY_SWAP=-7, UB_OVERLAY_VERIFY=-8 };
int ub_overlay_pages(int pid,uint32_t address,const unsigned char *expected,const unsigned char *replacement,uint32_t length);
/* Stops every thread of pid (ptrace), calls fn(pid, mem, arg) with mem an
 * open /proc/<pid>/mem, and lets the process go on. Returns fn's result, or
 * -1 if the process could not be stopped (fn is then not called). */
int ub_while_stopped(int pid,int (*fn)(int pid,int mem,void *arg),void *arg);
const char *ub_overlay_error(int rc);
int ub_hash_file(const char *path,char result[65]);
/* Whether a file whose SHA-256 is have may stand in for the 1.82 file
 * pinned: 182 the same file, 185 a supported 1.85 build (code-identical, or
 * data-only with addresses a module selects by version), 0 neither. Accepted
 * 1.85 builds are not yet confirmed on a player. */
int ub_firmware_match(const char *have,const char *pinned);
/* 1 when a payload's code may be injected into have for a 1.82 pinned file:
 * the same file, or a 1.85 build whose code is byte-identical. A data-only
 * 1.85 match is NOT injectable. */
int ub_firmware_injectable(const char *have,const char *pinned);
#endif
