#ifndef UB_NATIVE_CPU_WAIT_SYNC_V15_H
#define UB_NATIVE_CPU_WAIT_SYNC_V15_H
/* OFFLINE resident sleeping-worker adapter. No module load authorization.
 * Activation is permanently pinned after any possible text publication until
 * actual reset. This adapter deliberately has NO drain/free/unload authority.
 * Caller owns the control mutex and real framework/activation module leases.
 * init ONCE on fresh storage. begin/end SAME TASK span the whole command.
 */
#include <linux/types.h>
#include "native_binding_types_v5.h"
struct task_struct;
struct ub_cpu_wait_sync_v15 {
 ub_verified_patch_word_fn write_word;
 void *sites[2];
 u32 original[2];
 struct task_struct *holder;
 unsigned hotplug_held,build;
 int guard_checked;
};
int ub_cpu_wait_sync_init_v15(struct ub_cpu_wait_sync_v15 *context,unsigned build,
 ub_verified_patch_word_fn writer,void *const sites[2],const u32 original[2]);
int ub_cpu_wait_hotplug_begin_v15(struct ub_cpu_wait_sync_v15 *context);
void ub_cpu_wait_hotplug_end_v15(struct ub_cpu_wait_sync_v15 *context);
int ub_cpu_wait_patch_one_v15(struct ub_cpu_wait_sync_v15 *context,unsigned site,u32 expected,u32 replacement);
#endif
