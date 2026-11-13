/* OFFLINE ordinary component. No init/module metadata, no native execution
 * authorization. Sleeping return addresses require permanent residency after
 * activation. Static lock/lifecycle/admission proof belongs to controller. */
#include <linux/sched.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include "native_cpu_wait_v15.h"

struct ub_cpu_wait_context_v15 ub_fpd_wait_context_v15;
struct ub_cpu_wait_context_v15 ub_antena_wait_context_v15;

void ub_cpu_wait_v15(struct ub_cpu_wait_context_v15 *context)
{
 wait_queue_t entry;
 unsigned long flags;
 u64 pending;
 unsigned bypass;
 long result;

 if (READ_ONCE(context->rejected))
  return;
 /* Necessary checks, not a generic proof that sleeping here is safe. Never
  * repair task flags, preempt_count, IRQ state or unknown lock ownership. */
 if (READ_ONCE(context->expected_task) != current ||
     irqs_disabled() || preempt_count()) {
  WRITE_ONCE(context->rejected, 1);
  return;
 }
 spin_lock_irqsave(context->event_lock, flags);
 bypass = context->bypass;
 spin_unlock_irqrestore(context->event_lock, flags);
 if (bypass)
  return;

 entry.flags = 0;
 INIT_LIST_HEAD(&entry.task_list);
 for (;;) {
  /* Publish wait state BEFORE checking the event predicate. Never hold the
   * event lock across prepare/finish (which acquire the queue lock). */
  result = prepare_to_wait_event(context->queue, &entry, TASK_IDLE);
  if (result) {
   finish_wait(context->queue, &entry);
   spin_lock_irqsave(context->event_lock, flags);
   context->bypass = 1;
   /* Ordinary ON may clear bypass, but must never revive a rejected
    * protocol/context. Keep this failure sticky across resident ON/OFF. */
   WRITE_ONCE(context->rejected, 1);
   spin_unlock_irqrestore(context->event_lock, flags);
   return;
  }
  spin_lock_irqsave(context->event_lock, flags);
  pending = *context->event_word;
  bypass = context->bypass;
  spin_unlock_irqrestore(context->event_lock, flags);
  if (pending || bypass)
   break;
  schedule();
 }
 finish_wait(context->queue, &entry);
}
