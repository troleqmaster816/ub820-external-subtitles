/* SPDX-License-Identifier: (MIT OR GPL-2.0-only)
 * OFFLINE resident sleeping-worker text synchronization candidate.
 * No module entry point; no authority to load/execute on a real device.
 * No drain/unpin operation: post-publication lifetime lasts until true reset.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/stop_machine.h>
#include <linux/sched.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include <linux/uaccess.h>
#include <linux/kallsyms.h>
#include <linux/string.h>
#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include "native_cpu_wait_sync_v15.h"

/* Reuse immutable V12 exact native HELPER guards, never its MOV hook sites or
 * observer drain policy. Boot/offline tables in that data file are unused here.
 */
struct ub_obs12_guard {const char *name;unsigned long address;unsigned length;const u8 *expected;};
struct ub_obs12_build_data {
 unsigned long banner_address;const u8 *banner;unsigned banner_length;
 unsigned long sites[2],writer,cpu_ops;
 unsigned long known_ops[2],known_boot[2];
 const struct ub_obs12_guard *core;unsigned core_count;
 const struct ub_obs12_guard *boot;unsigned boot_count;
};
#include "native_observer_sync_guard_data_v12.h"
#include "native_cpu_wait_bindings_v15.h"

_Static_assert(sizeof(void *)==8,"exact AArch64 pointer ABI");
_Static_assert(ARRAY_SIZE(ub_wait15_sites)==ARRAY_SIZE(ub_obs12_builds),"per-build binding count");

static int ub_wait15_guard_one(const struct ub_obs12_guard *guard)
{
 u8 bytes[32];unsigned offset,count,i;
 if(!guard || !guard->name || !guard->expected || !guard->length || guard->length>65536 ||
    guard->length>ULONG_MAX-guard->address || kallsyms_lookup_name(guard->name)!=guard->address)
  return -ESTALE;
 for(offset=0;offset<guard->length;offset+=count){
  count=guard->length-offset;if(count>sizeof bytes)count=sizeof bytes;
  if(probe_kernel_read(bytes,(const void *)(guard->address+offset),count))return -EFAULT;
  for(i=0;i<count;i++)if(bytes[i]!=guard->expected[offset+i])return -ESTALE;
 }
 return 0;
}

static int ub_wait15_identity_and_core(unsigned build)
{
 const struct ub_obs12_build_data *data;u8 banner[256];unsigned i;int error;
 if(build>=ARRAY_SIZE(ub_obs12_builds))return -EINVAL;
 data=&ub_obs12_builds[build];
 if(!data->banner_length || data->banner_length>sizeof banner || data->core_count!=10 ||
    kallsyms_lookup_name("linux_banner")!=data->banner_address)return -ESTALE;
 if(probe_kernel_read(banner,(const void *)data->banner_address,data->banner_length))return -EFAULT;
 if(memcmp(banner,data->banner,data->banner_length))return -ESTALE;
 for(i=0;i<data->core_count;i++){
  error=ub_wait15_guard_one(&data->core[i]);if(error)return error;
 }
 return 0;
}

static int ub_wait15_context_valid(const struct ub_cpu_wait_sync_v15 *context)
{
 const struct ub_obs12_build_data *data;unsigned build;
 if(!context || context->build>=ARRAY_SIZE(ub_obs12_builds) || !context->write_word)return 0;
 build=context->build;data=&ub_obs12_builds[build];
 return (unsigned long)context->write_word==data->writer &&
        (unsigned long)context->sites[0]==ub_wait15_sites[build][0] &&
        (unsigned long)context->sites[1]==ub_wait15_sites[build][1] &&
        context->original[0]==ub_wait15_original[build][0] &&
        context->original[1]==ub_wait15_original[build][1];
}

static int ub_wait15_held(const struct ub_cpu_wait_sync_v15 *context)
{
 return ub_wait15_context_valid(context) && context->guard_checked &&
        context->hotplug_held && context->holder==current;
}

int ub_cpu_wait_sync_init_v15(struct ub_cpu_wait_sync_v15 *context,unsigned build,
 ub_verified_patch_word_fn writer,void *const sites[2],const u32 original[2])
{
 struct ub_cpu_wait_sync_v15 candidate;int error;
 if(!context || !sites || !original)return -EINVAL;
 memset(&candidate,0,sizeof candidate);candidate.build=build;candidate.write_word=writer;
 candidate.sites[0]=sites[0];candidate.sites[1]=sites[1];
 candidate.original[0]=original[0];candidate.original[1]=original[1];
 if(!ub_wait15_context_valid(&candidate))return -EINVAL;
 error=ub_wait15_identity_and_core(build);if(error)return error;
 *context=candidate;context->guard_checked=1;
 return 0;
}

int ub_cpu_wait_hotplug_begin_v15(struct ub_cpu_wait_sync_v15 *context)
{
 int error;
 if(!ub_wait15_context_valid(context))return -EINVAL;
 if(context->hotplug_held)return -EDEADLK;
 if(irqs_disabled() || preempt_count())return -EINVAL;
 context->guard_checked=0;
 error=ub_wait15_identity_and_core(context->build);if(error)return error;
 /* Exact byte-guarded 4.4 counted-reader implementation. Pending writers
  * release their brief mutex before schedule, so public stop_machine's
  * nested get_online_cpus does not deadlock this outer reader lease.
  */
 get_online_cpus();
 context->holder=current;context->hotplug_held=1;context->guard_checked=1;
 return 0;
}

void ub_cpu_wait_hotplug_end_v15(struct ub_cpu_wait_sync_v15 *context)
{
 if(!context || !context->hotplug_held || context->holder!=current)return;
 put_online_cpus();context->hotplug_held=0;context->holder=NULL;
}

struct ub_wait15_request {
 struct ub_cpu_wait_sync_v15 *context;
 atomic_t arrivals;int done,result;
 unsigned site;u32 expected,replacement;
};

static int ub_wait15_stopped(void *opaque)
{
 struct ub_wait15_request *request=opaque;
 struct ub_cpu_wait_sync_v15 *context=request->context;
 if(atomic_inc_return(&request->arrivals)==1){
  u32 observed;int result;
  result=probe_kernel_read(&observed,context->sites[request->site],sizeof observed);
  if(!result && observed!=request->expected)result=-ESTALE;
  if(!result){
   result=context->write_word(context->sites[request->site],request->replacement);
   /* Include any error-after-write case: native writer flushes only on0. */
   flush_icache_range((unsigned long)context->sites[request->site],(unsigned long)context->sites[request->site]+4);
  }
  request->result=result;smp_store_release(&request->done,1);
 }else while(!smp_load_acquire(&request->done))cpu_relax();
 isb();
 return READ_ONCE(request->result);
}

int ub_cpu_wait_patch_one_v15(struct ub_cpu_wait_sync_v15 *context,unsigned site,u32 expected,u32 replacement)
{
 struct ub_wait15_request request;int result;
 if(!ub_wait15_held(context) || site>=2 || irqs_disabled() || preempt_count())return -EINVAL;
 memset(&request,0,sizeof request);request.context=context;request.site=site;
 request.expected=expected;request.replacement=replacement;
 atomic_set(&request.arrivals,0);request.result=-EINPROGRESS;
 /* Every online CPU executes the callback and ISB. Outer lease spans the
  * whole command, not just this public stop_machine's nested reader.
  */
 result=stop_machine(ub_wait15_stopped,&request,cpu_online_mask);
 if(result)return result;
 if(!smp_load_acquire(&request.done))return -EIO;
 return request.result;
}
