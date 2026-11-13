#ifndef UB_NATIVE_CPU_WAIT_V15_H
#define UB_NATIVE_CPU_WAIT_V15_H
/* Ordinary offline component, no module entry. Once activated this sleeping
 * component must remain resident. Controller owns task/object lifetime and
 * initializes contexts before publication; no concurrent context reset. */
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/types.h>
struct task_struct;
struct ub_cpu_wait_context_v15 {
 wait_queue_head_t *queue;
 spinlock_t *event_lock;
 const u64 *event_word;
 unsigned bypass;    /* worker/controller access under event_lock */
 unsigned rejected;  /* worker-only sticky rejection, controller READ_ONCE */
 struct task_struct *expected_task; /* held reference, identity comparison */
};
extern struct ub_cpu_wait_context_v15 ub_fpd_wait_context_v15;
extern struct ub_cpu_wait_context_v15 ub_antena_wait_context_v15;
void ub_cpu_wait_v15(struct ub_cpu_wait_context_v15 *context);
#endif
