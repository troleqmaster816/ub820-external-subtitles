/* SPDX-License-Identifier: (MIT OR GPL-2.0-only)
 * OFFLINE source candidate: resident CPU-wait control/status VFS frontend only.
 * No module entry, hook/writer, worker, timer, poll loop, ioctl or async work.
 * Standard fops owner lease is the independent control-call lifetime reference.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/errno.h>
#include "native_cpu_wait_frontend_v15.h"

struct ub_frontend_file_v15 {
 struct mutex mutex; /* per-open snapshot/position, never held by observer */
 size_t length;
 unsigned valid;
 char status[UB_CPU_WAIT_FRONTEND_V15_STATUS_CAPACITY];
};
static const struct ub_cpu_wait_frontend_v15_ops *ub_frontend_ops_v15;
static void *ub_frontend_context_v15;
static unsigned ub_frontend_registered_v15;
static struct miscdevice ub_frontend_misc_v15;

static int ub_frontend_live_v15(void)
{
 int error=ub_frontend_ops_v15->is_live(ub_frontend_context_v15);
 return error>0?-ENODEV:error;
}
static int ub_frontend_open_v15(struct inode *inode,struct file *file)
{
 struct ub_frontend_file_v15 *state;
 int error;
 /* misc_open has already acquired fops.owner and set private_data. */
 if(file->private_data!=&ub_frontend_misc_v15 || !ub_frontend_ops_v15)
  return -ENODEV;
 error=ub_frontend_live_v15();
 if(error)
  return error;
 state=kzalloc(sizeof *state,GFP_KERNEL);
 if(!state)
  return -ENOMEM;
 mutex_init(&state->mutex);
 error=nonseekable_open(inode,file);
 if(error){
  kfree(state);
  return error;
 }
 /* VFS otherwise enables atomic f_pos only for regular files. Shared misc FDs
  * also need it: read/write receive a local position copied before our mutex. */
 file->f_mode|=FMODE_ATOMIC_POS;
 file->private_data=state;
 return 0;
}
static int ub_frontend_release_v15(struct inode *inode,struct file *file)
{
 (void)inode;
 /* release runs BEFORE VFS fops_put; no LIVE gate may skip this cleanup. */
 kfree(file->private_data);
 file->private_data=NULL;
 return 0;
}
static ssize_t ub_frontend_read_v15(struct file *file,char __user *output,
 size_t count,loff_t *position)
{
 struct ub_frontend_file_v15 *state=file->private_data;
 ssize_t result;
 int error=mutex_lock_interruptible(&state->mutex);
 if(error)
  return error;
 error=ub_frontend_live_v15();
 if(error){result=error;goto out;}
 if(!count){result=0;goto out;}
 if(!state->valid){
  size_t length=0;
  memset(state->status,0,sizeof state->status);
  error=ub_frontend_ops_v15->snapshot(ub_frontend_context_v15,state->status,
                                     sizeof state->status,&length);
  if(error){result=error<0?error:-EIO;goto out;}
  if(length>sizeof state->status){result=-EOVERFLOW;goto out;}
  state->length=length;
  /* VFS commits write's local position only for nonnegative returns. A failed
   * command can invalidate this snapshot without committing its *position=0. */
  *position=0;
  state->valid=1;
 }
 result=simple_read_from_buffer(output,count,position,state->status,state->length);
out:
 mutex_unlock(&state->mutex);
 return result;
}
static ssize_t ub_frontend_write_v15(struct file *file,const char __user *input,
 size_t count,loff_t *position)
{
 struct ub_frontend_file_v15 *state=file->private_data;
 enum ub_cpu_wait_frontend_v15_command command;
 char word[4];
 size_t length=count;
 ssize_t result;
 int error;
 if(!count)
  return 0;
 if(count>sizeof word)
  return -EMSGSIZE;
 if(copy_from_user(word,input,count))
  return -EFAULT;
 if(word[length-1]=='\n')
  length--;
 if(length==2 && !memcmp(word,"ON",2))
  command=UB_CPU_WAIT_FRONTEND_V15_ON;
 else if(length==3 && !memcmp(word,"OFF",3))
  command=UB_CPU_WAIT_FRONTEND_V15_OFF;
 else
  return -EINVAL;
 error=mutex_lock_interruptible(&state->mutex);
 if(error)
  return error;
 error=ub_frontend_live_v15();
 if(error){result=error;goto out;}
 /* A failed command may leave a safely pinned partial controller state.
  * Invalidate on EVERY attempted callback, not only successful commands. */
 state->valid=0;
 state->length=0;
 *position=0;
 error=ub_frontend_ops_v15->command(ub_frontend_context_v15,command);
 result=error?(error<0?error:-EIO):(ssize_t)count;
out:
 mutex_unlock(&state->mutex);
 return result;
}
static const struct file_operations ub_frontend_fops_v15={
 .owner=THIS_MODULE,
 .open=ub_frontend_open_v15,
 .release=ub_frontend_release_v15,
 .read=ub_frontend_read_v15,
 .write=ub_frontend_write_v15,
 .llseek=no_llseek,
};
static struct miscdevice ub_frontend_misc_v15={
 .minor=MISC_DYNAMIC_MINOR,
 .name="ub820_cpu_wait",
 .fops=&ub_frontend_fops_v15,
 .mode=0600,
};
int ub_cpu_wait_frontend_v15_register(const struct ub_cpu_wait_frontend_v15_ops *ops,
 void *context)
{
 int error;
 if(!ops || !ops->is_live || !ops->command || !ops->snapshot)
  return -EINVAL;
 if(ub_frontend_ops_v15)
  return -EBUSY;
 /* module init only. misc registration publishes these fully initialized
  * immutable callback bindings; COMING open rejects before command/status. */
 ub_frontend_ops_v15=ops;
 ub_frontend_context_v15=context;
 error=misc_register(&ub_frontend_misc_v15);
 if(error){
  ub_frontend_ops_v15=NULL;
  ub_frontend_context_v15=NULL;
  return error;
 }
 ub_frontend_registered_v15=1;
 return 0;
}
void ub_cpu_wait_frontend_v15_unregister(void)
{
 /* Normal module-exit only: owner refs preclude live open files. Keep callback
  * binding immutable until reclamation; do not permit rebind/re-registration. */
 if(ub_frontend_registered_v15){
  misc_deregister(&ub_frontend_misc_v15);
  ub_frontend_registered_v15=0;
 }
}
