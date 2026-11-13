#ifndef NATIVE_CPU_WAIT_FRONTEND_V15_H
#define NATIVE_CPU_WAIT_FRONTEND_V15_H
/* OFFLINE candidate, not a loaded module. Root owns module init/exit/controller.
 * Callbacks/context remain alive from registration through module reclamation.
 * register is the LAST fallible init action: return 0 immediately on success.
 * Initialize all callback state BEFORE register. Never runtime-unregister/rebind.
 */
#include <linux/types.h>
#define UB_CPU_WAIT_FRONTEND_V15_STATUS_CAPACITY 1024u
enum ub_cpu_wait_frontend_v15_command {
 UB_CPU_WAIT_FRONTEND_V15_ON,
 UB_CPU_WAIT_FRONTEND_V15_OFF
};
struct ub_cpu_wait_frontend_v15_ops {
 /* 0 ONLY for exact MODULE_STATE_LIVE; otherwise negative kernel errno.
  * Must be safe in COMING before any command/status callback can run. */
 int (*is_live)(void *context);
 /* 0 success, negative kernel errno failure (map controller-specific codes).
  * command and snapshot serialize the SAME root controller mutex. The VFS
  * fops.owner lease protects the complete call, including activation-ref drop. */
 int (*command)(void *context,enum ub_cpu_wait_frontend_v15_command command);
 /* Produce one complete bounded snapshot; set length to actual initialized
  * bytes <=capacity. No user pointer. 0 success or negative kernel errno.
  * Does not retain output/length pointers or perform observer ON/OFF itself. */
 int (*snapshot)(void *context,char *output,size_t capacity,size_t *length);
};
int ub_cpu_wait_frontend_v15_register(const struct ub_cpu_wait_frontend_v15_ops *,void *);
/* Normal module-exit ONLY, after controller is inactive/drained. FDs pin owner;
 * never force-unload, free callbacks early, or deregister/rebind at runtime. */
void ub_cpu_wait_frontend_v15_unregister(void);
#endif
