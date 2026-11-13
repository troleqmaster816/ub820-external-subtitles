/* SPDX-License-Identifier: (MIT OR GPL-2.0-only)
 * OFFLINE native CPU-wait candidate, not authorized for a device test.
 * Init is inert. LIVE-only ON/OFF uses the original worker queues and keeps
 * all contexts/owner references and a module lease until actual kernel reset.
 * OFF restores branches but cannot free parked module return addresses.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/wait.h>
#include <linux/timer.h>
#include <linux/string.h>
#include "full_code_guard_fixture_v7.h"
#include "native_preflight_bindings_v9.h"
#include <linux/mutex.h>
#include "resident_wait_controller_v15.h"
#include "native_cpu_wait_v15.h"
#include "native_cpu_wait_frontend_v15.h"
#include "native_cpu_wait_sync_v15.h"
extern void ub_fpd_wait_bridge_v15(void);
extern void ub_antena_wait_bridge_v15(void);

_Static_assert(sizeof(void *)==8,"AArch64 pointers required");
_Static_assert(sizeof(pid_t)==4,"native saved PID width");
_Static_assert(sizeof(atomic_t)==4,"native task usage atomic width");
_Static_assert(__builtin_offsetof(atomic_t,counter)==0,"native task usage counter");
_Static_assert(__builtin_offsetof(struct task_struct,usage)==16,"native task usage offset");
_Static_assert(__builtin_offsetof(struct task_struct,flags)==20,"native task flags offset");
_Static_assert(sizeof(spinlock_t)==4,"native event lock width");
_Static_assert(sizeof(wait_queue_head_t)==24,"native queue head width");
_Static_assert(__builtin_offsetof(wait_queue_head_t,task_list)==8,"native queue list offset");
_Static_assert(sizeof(struct timer_list)==48,"native timer metadata width");
_Static_assert(UB_PF_ROLE_COUNT==16,"all dynamic roles required");
_Static_assert(UB_PF_PAN_BIT==4,"verified native PAN capability bit");

struct ub_pf_worker_snapshot {
 struct task_struct *task; /* caller owns this reference on success OR failure */
 pid_t pid;
 unsigned flags;
 u8 initialized;
 u64 event;
 unsigned queue_self;
};

static int ub_pf_lookup(void *unused,const char *name,uint64_t *address)
{
 (void)unused;
 *address=kallsyms_lookup_name(name);
 return *address?0:-ENOENT;
}

static int ub_pf_read(void *unused,uint64_t address,void *output,size_t length)
{
 (void)unused;
 return probe_kernel_read(output,(const void *)(unsigned long)address,length);
}

static const struct guard_v7_ops ub_pf_guard_ops={ub_pf_lookup,ub_pf_read};

static const struct ub_preflight_build_binding *ub_pf_select_build(void)
{
 unsigned long address=kallsyms_lookup_name("linux_banner");
 u8 actual[256];
 unsigned i;
 if(!address)
  return NULL;
 for(i=0;i<ARRAY_SIZE(ub_preflight_bindings_v9);i++){
  const struct ub_preflight_build_binding *binding=&ub_preflight_bindings_v9[i];
  if(binding->banner_address!=address || !binding->banner_bytes ||
     !binding->banner_length_including_nul ||
     binding->banner_length_including_nul>sizeof actual)
   continue;
  if(!probe_kernel_read(actual,(const void *)address,binding->banner_length_including_nul) &&
     !memcmp(actual,binding->banner_bytes,binding->banner_length_including_nul))
   return binding;
 }
 return NULL;
}

static int ub_pf_bind_roles(const struct ub_preflight_build_binding *binding)
{
 static const unsigned widths[UB_PF_ROLE_COUNT]={4,1,4,8,24,16,48,4,4,1,4,8,24,48,4,24};
 unsigned i,j;
 if(binding->build>1 || binding->bss_start>=binding->bss_end ||
    kallsyms_lookup_name("__bss_start")!=binding->bss_start ||
    kallsyms_lookup_name("__bss_stop")!=binding->bss_end ||
    binding->caps_width!=sizeof(u64) ||
    kallsyms_lookup_name("cpu_hwcaps")!=binding->caps_address ||
    binding->caps_address<binding->bss_start ||
    binding->caps_address>=binding->bss_end ||
    binding->caps_width>binding->bss_end-binding->caps_address ||
    (binding->caps_address&7))
  return -ESTALE;
 for(i=0;i<UB_PF_ROLE_COUNT;i++){
  const struct ub_preflight_role_binding *role=&binding->roles[i];
  unsigned alignment=widths[i]>=8?8:widths[i];
  if(!role->role || !role->symbol || role->width!=widths[i] ||
     role->address<binding->bss_start || role->address>=binding->bss_end ||
     role->width>binding->bss_end-role->address ||
     (role->address&(alignment-1)) || kallsyms_lookup_name(role->symbol)!=role->address)
   return -ESTALE;
 }
 /* Independent roles must not alias. All interval ends were checked above. */
 for(i=0;i<UB_PF_ROLE_COUNT;i++)for(j=i+1;j<UB_PF_ROLE_COUNT;j++){
  const struct ub_preflight_role_binding *a=&binding->roles[i],*b=&binding->roles[j];
  if(a->address<b->address+b->width && b->address<a->address+a->width)
   return -ESTALE;
 }
 return 0;
}

static int ub_pf_capture_worker(const struct ub_preflight_build_binding *binding,
 unsigned pid_role,unsigned init_role,unsigned lock_role,unsigned event_role,unsigned queue_role,
 struct ub_pf_worker_snapshot *snapshot)
{
 const pid_t *pid_word=(const pid_t *)binding->roles[pid_role].address;
 const u8 *init_word=(const u8 *)binding->roles[init_role].address;
 const u64 *event_word=(const u64 *)binding->roles[event_role].address;
 spinlock_t *event_lock=(spinlock_t *)binding->roles[lock_role].address;
 wait_queue_head_t *queue=(wait_queue_head_t *)binding->roles[queue_role].address;
 struct pid *pid_reference;
 struct list_head *next,*previous,*self=&queue->task_list;
 unsigned long flags;
 pid_t observed_pid;
 u8 observed_init;

 /* Observe initialization before trying either native lock. This is a
  * steady-state diagnostic prerequisite, not proof of a global lifecycle
  * freeze or of publication ordering during the original driver's startup. */
 if(probe_kernel_read(&snapshot->pid,pid_word,sizeof snapshot->pid) ||
    probe_kernel_read(&snapshot->initialized,init_word,sizeof snapshot->initialized))
  return -EFAULT;
 if(snapshot->pid<=0 || snapshot->initialized!=1)
  return -EAGAIN;
 pid_reference=find_get_pid(snapshot->pid);
 if(!pid_reference)
  return -ESRCH;
 snapshot->task=get_pid_task(pid_reference,PIDTYPE_PID);
 put_pid(pid_reference); /* no pid reference survives this point */
 if(!snapshot->task)
  return -ESRCH;
 /* Native get_pid_task already acquired task+16 usage. Do not get twice or
  * call __put_task_struct directly; the caller uses standard put_task_struct.
  * Namespace admission is checked before this helper, then identity is
  * checked explicitly in init_pid_ns too. Ref ownership is lifetime only. */
 if(__task_pid_nr_ns(snapshot->task,PIDTYPE_PID,&init_pid_ns)!=snapshot->pid)
  return -ESTALE;
 snapshot->flags=READ_ONCE(snapshot->task->flags);
 if(!(snapshot->flags&PF_KTHREAD) || (snapshot->flags&PF_EXITING))
  return -ESRCH;

 spin_lock_irqsave(event_lock,flags);
 observed_pid=READ_ONCE(*pid_word);
 observed_init=READ_ONCE(*init_word);
 snapshot->event=READ_ONCE(*event_word); /* full word; never clear or mask */
 spin_unlock_irqrestore(event_lock,flags);
 if(observed_pid!=snapshot->pid || observed_init!=1)
  return -EAGAIN;

 /* Separate acquisition, never nested with event lock. Do not inspect a
  * ticket word, require zero tickets, walk links or dereference neighbors. */
 spin_lock_irqsave(&queue->lock,flags);
 next=READ_ONCE(queue->task_list.next);
 previous=READ_ONCE(queue->task_list.prev);
 snapshot->queue_self=next==self && previous==self;
 spin_unlock_irqrestore(&queue->lock,flags);
 /* Preserve the existing queue: occupancy is a separate snapshot, not an
  * excuse to reinitialize a native head or discard other native waiters. */

 /* Best-effort end check narrows obvious changes; it does not turn separate
  * observations into one atomic all-object snapshot or freeze the task. */
 snapshot->flags=READ_ONCE(snapshot->task->flags);
 if(!(snapshot->flags&PF_KTHREAD) || (snapshot->flags&PF_EXITING) ||
    __task_pid_nr_ns(snapshot->task,PIDTYPE_PID,&init_pid_ns)!=snapshot->pid)
  return -ESRCH;
 if(probe_kernel_read(&observed_pid,pid_word,sizeof observed_pid) ||
    probe_kernel_read(&observed_init,init_word,sizeof observed_init))
  return -EFAULT;
 return observed_pid==snapshot->pid && observed_init==1?0:-EAGAIN;
}

static DEFINE_MUTEX(ub_cpu_wait_control_mutex_v15);
static const unsigned long ub_cpu_wait_sites_v15[2]={
 0xffffffc000626ab0UL,0xffffffc00062aa60UL
};
static const u32 ub_cpu_wait_original_v15[2]={0xb4ffff53u,0xb4ffff53u};
static struct {
 const struct ub_preflight_build_binding *binding;
 struct ub_pf_worker_snapshot workers[2]; /* real leases, retained until exit */
 struct ub_cpu_wait_sync_v15 sync;
 struct rw15_controller controller;
 struct guard_v7_report guard;
 u64 caps;
 int pan,begin_error;
} ub_cpu_wait_root_v15;

static struct ub_cpu_wait_context_v15 *ub_cpu_wait_context_v15(unsigned index)
{
 return index?&ub_antena_wait_context_v15:&ub_fpd_wait_context_v15;
}

/* Encode only an aligned direct B within the architectural signed imm26 range.
 * Avoid signed pointer subtraction/overflow, including negative-limit cases. */
static int ub_cpu_wait_branch_v15(unsigned long source,unsigned long destination,u32 *word)
{
 unsigned long distance;
 u32 immediate;
 if(!word || ((source|destination)&3))
  return -EINVAL;
 if(destination>=source){
  distance=destination-source;
  if(distance>=(1UL<<27))return -ERANGE;
  immediate=(u32)(distance>>2);
 }else{
  distance=source-destination;
  if(distance>(1UL<<27))return -ERANGE;
  immediate=0u-(u32)(distance>>2);
 }
 *word=0x14000000u|(immediate&0x03ffffffu);
 return 0;
}

static int ub_cpu_wait_framework_v15(void *unused)
{
 (void)unused;
 /* Closed call graph: controller is invoked ONLY by our misc fops callback.
  * misc_open acquired THIS_MODULE through fops_get BEFORE calling open, and
  * VFS keeps that independent lease through write/release. This predicate
  * does not acquire a reference or infer it from module_refcount(). */
 return 1;
}
static int ub_cpu_wait_enter_v15(void *unused)
{
 int error;
 (void)unused;
 if(!mutex_trylock(&ub_cpu_wait_control_mutex_v15))
  return -EBUSY;
 error=ub_cpu_wait_hotplug_begin_v15(&ub_cpu_wait_root_v15.sync);
 ub_cpu_wait_root_v15.begin_error=error;
 if(error)
  mutex_unlock(&ub_cpu_wait_control_mutex_v15);
 return error;
}
static void ub_cpu_wait_leave_v15(void *unused)
{
 (void)unused;
 ub_cpu_wait_hotplug_end_v15(&ub_cpu_wait_root_v15.sync);
 mutex_unlock(&ub_cpu_wait_control_mutex_v15);
}
static int ub_cpu_wait_state_v15(void *unused)
{
 (void)unused;
 return READ_ONCE(THIS_MODULE->state);
}
static int ub_cpu_wait_guard_v15(void *unused)
{
 const struct ub_preflight_build_binding *binding=ub_cpu_wait_root_v15.binding;
 u64 caps;
 unsigned i;
 int error;
 (void)unused;
 if(READ_ONCE(ub_fpd_wait_context_v15.rejected) || READ_ONCE(ub_antena_wait_context_v15.rejected))
  return -ESTALE;
 if(ub_pf_select_build()!=binding || ub_pf_bind_roles(binding))
  return -ESTALE;
 if(probe_kernel_read(&caps,(const void *)binding->caps_address,sizeof caps) ||
    caps!=ub_cpu_wait_root_v15.caps)
  return -ESTALE;
 error=guard_v7_validate(&guard_v7_fixtures[binding->build],ub_cpu_wait_root_v15.pan,
                         &ub_pf_guard_ops,NULL,&ub_cpu_wait_root_v15.guard);
 if(error)
  return -ESTALE;
 for(i=0;i<2;i++){
  const unsigned pid_role=i?UB_PF_ANTENA_PID:UB_PF_FPD_PID;
  const unsigned init_role=i?UB_PF_ANTENA_INIT:UB_PF_FPD_INIT;
  const unsigned lock_role=i?UB_PF_ANTENA_LOCK:UB_PF_FPD_LOCK;
  const struct ub_pf_worker_snapshot *worker=&ub_cpu_wait_root_v15.workers[i];
  unsigned long irq_flags;
  unsigned task_flags;
  pid_t pid;
  u8 initialized;
  if(!worker->task || __task_pid_nr_ns(worker->task,PIDTYPE_PID,&init_pid_ns)!=worker->pid)
   return -ESTALE;
  task_flags=READ_ONCE(worker->task->flags);
  if(!(task_flags&PF_KTHREAD) || (task_flags&PF_EXITING))
   return -ESRCH;
  spin_lock_irqsave((spinlock_t *)binding->roles[lock_role].address,irq_flags);
  pid=READ_ONCE(*(const pid_t *)binding->roles[pid_role].address);
  initialized=READ_ONCE(*(const u8 *)binding->roles[init_role].address);
  spin_unlock_irqrestore((spinlock_t *)binding->roles[lock_role].address,irq_flags);
  if(pid!=worker->pid || initialized!=1)
   return -EAGAIN;
 }
 return 0;
}
static int ub_cpu_wait_pin_v15(void *unused)
{
 (void)unused;
 return try_module_get(THIS_MODULE)?0:-ENODEV;
}


static int ub_cpu_wait_read_site_v15(void *unused,unsigned index,u32 *word)
{
 (void)unused;
 if(index>=2 || !word)return -EINVAL;
 return probe_kernel_read(word,(const void *)ub_cpu_wait_sites_v15[index],sizeof *word);
}
static int ub_cpu_wait_publish_v15(void *unused,unsigned index,u32 expected,u32 replacement)
{
 (void)unused;
 return ub_cpu_wait_patch_one_v15(&ub_cpu_wait_root_v15.sync,index,expected,replacement);
}
static void ub_cpu_wait_publish_bypass_v15(void *unused,unsigned index,unsigned bypass)
{
 struct ub_cpu_wait_context_v15 *context;
 unsigned long flags;
 (void)unused;
 if(index>=2 || bypass>1)return;
 context=ub_cpu_wait_context_v15(index);
 spin_lock_irqsave(context->event_lock,flags);
 WRITE_ONCE(context->bypass,bypass);
 spin_unlock_irqrestore(context->event_lock,flags);
}
static void ub_cpu_wait_wake_v15(void *unused,unsigned index)
{
 (void)unused;
 if(index<2)__wake_up(ub_cpu_wait_context_v15(index)->queue,TASK_NORMAL,0,NULL);
}



static const struct rw15_ops ub_cpu_wait_controller_ops_v15={
 .framework_reference_held=ub_cpu_wait_framework_v15,
 .enter_control=ub_cpu_wait_enter_v15,.leave_control=ub_cpu_wait_leave_v15,
 .module_state=ub_cpu_wait_state_v15,
 .verify_original_manifest_and_owners=ub_cpu_wait_guard_v15,
 .pin_until_true_reset=ub_cpu_wait_pin_v15,
 .read_site=ub_cpu_wait_read_site_v15,.compare_publish_synchronized=ub_cpu_wait_publish_v15,
 .publish_bypass_locked=ub_cpu_wait_publish_bypass_v15,.wake_worker=ub_cpu_wait_wake_v15,
};

static int ub_cpu_wait_live_v15(void *unused)
{
 (void)unused;
 return READ_ONCE(THIS_MODULE->state)==MODULE_STATE_LIVE?0:-ENODEV;
}
static int ub_cpu_wait_command_v15(void *unused,enum ub_cpu_wait_frontend_v15_command command)
{
 int result;
 (void)unused;
 if(ub_cpu_wait_live_v15(NULL))return -ENODEV;
 if(command==UB_CPU_WAIT_FRONTEND_V15_ON)result=rw15_on(&ub_cpu_wait_root_v15.controller);
 else if(command==UB_CPU_WAIT_FRONTEND_V15_OFF)result=rw15_off(&ub_cpu_wait_root_v15.controller);
 else return -EINVAL;
 switch(result){
 case RW15_OK:case RW15_ALREADY:return 0;
 case RW15_BUSY:case RW15_RECOVER:return -EBUSY;
 case RW15_MODULE:case RW15_FRAMEWORK:case RW15_PIN:return -ENODEV;
 case RW15_GUARD:return -ESTALE;
 case RW15_READ:return -EFAULT;
 default:return -EIO;
 }
}

static int ub_cpu_wait_snapshot_v15(void *unused,char *output,size_t capacity,size_t *length)
{
 const struct rw15_controller *controller=&ub_cpu_wait_root_v15.controller;
 size_t used=0;unsigned i;
 (void)unused;
 if(!output || !length || capacity<UB_CPU_WAIT_FRONTEND_V15_STATUS_CAPACITY)return -EINVAL;
 if(!mutex_trylock(&ub_cpu_wait_control_mutex_v15))return -EBUSY;
 used+=scnprintf(output+used,capacity-used,
  "cpu_wait=1 build=%s phase=%u pinned_until_reset=%u intended_enabled=%u rc=%d fault=%d begin_rc=%d\n",
  ub_cpu_wait_root_v15.binding->label,(unsigned)controller->phase,controller->pinned,
  controller->enabled,controller->last_result,controller->fault,ub_cpu_wait_root_v15.begin_error);
 for(i=0;i<2;i++){
  const struct ub_cpu_wait_context_v15 *context=ub_cpu_wait_context_v15(i);
  unsigned long irq_flags;unsigned bypass,rejected;
  spin_lock_irqsave(context->event_lock,irq_flags);
  bypass=READ_ONCE(context->bypass);rejected=READ_ONCE(context->rejected);
  spin_unlock_irqrestore(context->event_lock,irq_flags);
  used+=scnprintf(output+used,capacity-used,"%s pid=%d bypass=%u rejected=%u observed_task_state=%ld\n",
   i?"antena":"fpd",ub_cpu_wait_root_v15.workers[i].pid,bypass,rejected,
   READ_ONCE(ub_cpu_wait_root_v15.workers[i].task->state));
 }
 *length=used;mutex_unlock(&ub_cpu_wait_control_mutex_v15);return 0;
}

static const struct ub_cpu_wait_frontend_v15_ops ub_cpu_wait_frontend_ops_v15={
 .is_live=ub_cpu_wait_live_v15,.command=ub_cpu_wait_command_v15,.snapshot=ub_cpu_wait_snapshot_v15,
};
static void ub_cpu_wait_release_workers_v15(void)
{
 unsigned i;
 for(i=0;i<2;i++)if(ub_cpu_wait_root_v15.workers[i].task){
  put_task_struct(ub_cpu_wait_root_v15.workers[i].task);
  ub_cpu_wait_root_v15.workers[i].task=NULL;
 }
}
static int __init ub_cpu_wait_init_v15(void)
{
 const struct ub_preflight_build_binding *binding;
 const struct guard_v7_window *writer_guard;
 u32 redirects[2];
 void *sites[2]={(void *)ub_cpu_wait_sites_v15[0],(void *)ub_cpu_wait_sites_v15[1]};
 int error=-ESTALE;
 binding=ub_pf_select_build();
 if(!binding)goto fail;
 ub_cpu_wait_root_v15.binding=binding;
 error=ub_pf_bind_roles(binding);if(error)goto fail;
 error=probe_kernel_read(&ub_cpu_wait_root_v15.caps,(void *)binding->caps_address,sizeof(u64));
 if(error)goto fail;
 ub_cpu_wait_root_v15.pan=!!(ub_cpu_wait_root_v15.caps&(1ULL<<UB_PF_PAN_BIT));
 error=guard_v7_validate(&guard_v7_fixtures[binding->build],ub_cpu_wait_root_v15.pan,
                        &ub_pf_guard_ops,NULL,&ub_cpu_wait_root_v15.guard);
 if(error){error=-ESTALE;goto fail;}
 if(task_active_pid_ns(current)!=&init_pid_ns){error=-EXDEV;goto fail;}
 error=ub_pf_capture_worker(binding,UB_PF_FPD_PID,UB_PF_FPD_INIT,UB_PF_FPD_LOCK,
                            UB_PF_FPD_EVENT,UB_PF_FPD_QUEUE,&ub_cpu_wait_root_v15.workers[0]);
 if(error)goto fail;
 error=ub_pf_capture_worker(binding,UB_PF_ANTENA_PID,UB_PF_ANTENA_INIT,UB_PF_ANTENA_LOCK,
                            UB_PF_ANTENA_EVENT,UB_PF_ANTENA_QUEUE,&ub_cpu_wait_root_v15.workers[1]);
 if(error)goto fail;
 if(ub_cpu_wait_root_v15.workers[0].pid==ub_cpu_wait_root_v15.workers[1].pid ||
    ub_cpu_wait_root_v15.workers[0].task==ub_cpu_wait_root_v15.workers[1].task){error=-ESTALE;goto fail;}
 /* Fresh initialization ONLY, before registration/publication. Never reset
  * these contexts on later ON/OFF; parked old helpers may still use them. */
 {
  unsigned i;
  for(i=0;i<2;i++){
   struct ub_cpu_wait_context_v15 *context=ub_cpu_wait_context_v15(i);
   context->queue=(wait_queue_head_t *)binding->roles[i?UB_PF_ANTENA_QUEUE:UB_PF_FPD_QUEUE].address;
   context->event_lock=(spinlock_t *)binding->roles[i?UB_PF_ANTENA_LOCK:UB_PF_FPD_LOCK].address;
   context->event_word=(const u64 *)binding->roles[i?UB_PF_ANTENA_EVENT:UB_PF_FPD_EVENT].address;
   context->expected_task=ub_cpu_wait_root_v15.workers[i].task;
   context->bypass=1;context->rejected=0;
  }
 }
 writer_guard=&guard_v7_fixtures[binding->build].windows[13];
 if(strcmp(writer_guard->name,"aarch64_insn_patch_text_nosync")){error=-ESTALE;goto fail;}
 error=ub_cpu_wait_branch_v15(ub_cpu_wait_sites_v15[0],(unsigned long)ub_fpd_wait_bridge_v15,&redirects[0]);
 if(error)goto fail;
 error=ub_cpu_wait_branch_v15(ub_cpu_wait_sites_v15[1],(unsigned long)ub_antena_wait_bridge_v15,&redirects[1]);
 if(error)goto fail;
 error=ub_cpu_wait_sync_init_v15(&ub_cpu_wait_root_v15.sync,binding->build,
                (ub_verified_patch_word_fn)(unsigned long)writer_guard->address,sites,ub_cpu_wait_original_v15);
 if(error)goto fail;
 error=rw15_init(&ub_cpu_wait_root_v15.controller,&ub_cpu_wait_controller_ops_v15,NULL,
                  ub_cpu_wait_original_v15,redirects);
 if(error){error=-EINVAL;goto fail;}
 /* This is the LAST fallible init operation. Success returns immediately:
  * no post-registration failure may strand a COMING-state open/fops lease. */
 error=ub_cpu_wait_frontend_v15_register(&ub_cpu_wait_frontend_ops_v15,NULL);
 if(!error)return 0;
fail:
 ub_cpu_wait_release_workers_v15();
 pr_err("ub820_cpu_wait: inert init failed rc=%d; no hooks published\n",error);
 return error;
}
static void __exit ub_cpu_wait_exit_v15(void)
{
 /* Normal unload is possible only with no VFS/activation lease. No text or
  * driver cleanup is attempted from exit. Once activated, permanent residency
 * makes normal unload refuse; force unload is unsupported. */
 ub_cpu_wait_frontend_v15_unregister();
 ub_cpu_wait_release_workers_v15();
 pr_info("ub820_cpu_wait: unloaded; CPU wait inactive\n");
}
module_init(ub_cpu_wait_init_v15);
module_exit(ub_cpu_wait_exit_v15);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("UB820 native worker waiting; resident code and reusable ON/OFF");
MODULE_VERSION("0.0-offline-v15");

