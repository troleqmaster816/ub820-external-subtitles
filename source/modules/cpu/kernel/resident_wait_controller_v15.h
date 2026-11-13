#ifndef RESIDENT_WAIT_CONTROLLER_V15_H
#define RESIDENT_WAIT_CONTROLLER_V15_H
/* Reusable resident ON/OFF: never unpins or resets a context after publication.
 * Every call requires a real independent framework ref through its return.
 */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <string.h>
/* end of platform includes */
#endif
enum rw15_site {RW15_ORIGINAL,RW15_REDIRECT,RW15_OTHER,RW15_UNREADABLE};
enum rw15_phase {RW15_INERT,RW15_ACTIVE,RW15_PARTIAL,RW15_RESTORED_RESIDENT,RW15_BLOCKED_RESIDENT};
enum rw15_result {RW15_OK=0,RW15_ALREADY=1,RW15_BUSY=-1,RW15_MODULE=-2,RW15_FRAMEWORK=-3,
 RW15_GUARD=-4,RW15_PIN=-5,RW15_READ=-6,RW15_PATCH=-7,RW15_UNKNOWN=-8,RW15_RECOVER=-9};
struct rw15_ops {
 int (*framework_reference_held)(void *);
 int (*enter_control)(void *);void (*leave_control)(void *);
 int (*module_state)(void *);
 int (*verify_original_manifest_and_owners)(void *);
 int (*pin_until_true_reset)(void *);
 int (*read_site)(void *,unsigned,uint32_t *);
 int (*compare_publish_synchronized)(void *,unsigned,uint32_t,uint32_t);
 void (*publish_bypass_locked)(void *,unsigned,unsigned);
 void (*wake_worker)(void *,unsigned);
};
struct rw15_controller {
 const struct rw15_ops *ops;void *context;
 uint32_t original[2],redirect[2],observed[2];
 enum rw15_site site[2];enum rw15_phase phase;
 unsigned pinned,enabled,bypass[2],patch_calls;
 int fault,last_result;
};
static inline int rw15_init(struct rw15_controller *c,const struct rw15_ops *o,void *p,
 const uint32_t original[2],const uint32_t redirect[2]){
 if(!c || !o || !original || !redirect || !o->framework_reference_held || !o->enter_control ||
    !o->leave_control || !o->module_state || !o->verify_original_manifest_and_owners ||
    !o->pin_until_true_reset || !o->read_site || !o->compare_publish_synchronized ||
    !o->publish_bypass_locked || !o->wake_worker || original[0]==redirect[0] || original[1]==redirect[1])return RW15_GUARD;
 uint32_t a[2]={original[0],original[1]},b[2]={redirect[0],redirect[1]};
 memset(c,0,sizeof *c);c->ops=o;c->context=p;
 for(unsigned i=0;i<2;i++){c->original[i]=a[i];c->redirect[i]=b[i];c->site[i]=RW15_UNREADABLE;c->bypass[i]=1;}
 return RW15_OK;
}
static inline int rw15_refresh(struct rw15_controller *c){
 int result=0;
 for(unsigned i=0;i<2;i++){
  uint32_t word;
  if(c->ops->read_site(c->context,i,&word)){c->site[i]=RW15_UNREADABLE;result=RW15_READ;continue;}
  c->observed[i]=word;
  c->site[i]=word==c->original[i]?RW15_ORIGINAL:word==c->redirect[i]?RW15_REDIRECT:RW15_OTHER;
  if(c->site[i]==RW15_OTHER && !result)result=RW15_UNKNOWN;
 }
 return result;
}
static inline void rw15_bypass_and_wake(struct rw15_controller *c){
 c->enabled=0;
 for(unsigned i=0;i<2;i++){
  c->ops->publish_bypass_locked(c->context,i,1);c->bypass[i]=1;
  c->ops->wake_worker(c->context,i);
 }
}
static inline int rw15_finish(struct rw15_controller *c,int result){
 if(result<0 && c->pinned)c->fault=result;
 if(!c->pinned)c->phase=RW15_INERT;
 else if(c->site[0]>=RW15_OTHER || c->site[1]>=RW15_OTHER)c->phase=RW15_BLOCKED_RESIDENT;
 else if(c->enabled && c->site[0]==RW15_REDIRECT && c->site[1]==RW15_REDIRECT)c->phase=RW15_ACTIVE;
 else if(c->site[0]==RW15_ORIGINAL && c->site[1]==RW15_ORIGINAL)c->phase=RW15_RESTORED_RESIDENT;
 else c->phase=RW15_PARTIAL;
 c->last_result=result;c->ops->leave_control(c->context);return result;
}
static inline int rw15_on(struct rw15_controller *c){
 const struct rw15_ops *o=c->ops;void *p=c->context;int rc;
 if(!o->framework_reference_held(p))return RW15_FRAMEWORK;
 if(o->enter_control(p))return RW15_BUSY;
 if(o->module_state(p)!=0)return rw15_finish(c,RW15_MODULE);
 rc=rw15_refresh(c);
 if(rc){if(c->pinned)rw15_bypass_and_wake(c);return rw15_finish(c,rc);}
 if(c->enabled){
  if(c->site[0]==RW15_REDIRECT && c->site[1]==RW15_REDIRECT && !c->fault)return rw15_finish(c,RW15_ALREADY);
  rw15_bypass_and_wake(c);return rw15_finish(c,RW15_RECOVER);
 }
 if(c->fault)return rw15_finish(c,RW15_RECOVER);
 if(c->site[0]!=RW15_ORIGINAL || c->site[1]!=RW15_ORIGINAL)return rw15_finish(c,RW15_RECOVER);
 if(o->verify_original_manifest_and_owners(p))return rw15_finish(c,RW15_GUARD);
 if(!c->pinned){if(o->pin_until_true_reset(p))return rw15_finish(c,RW15_PIN);c->pinned=1;}
 if(o->module_state(p)!=0)return rw15_finish(c,RW15_MODULE);
 /* Same contexts/owners/queues can have an old helper continuation. NEVER
  * reset them. Install while bypassed; enable ONLY after both sites verified. */
 rw15_bypass_and_wake(c);
 for(unsigned i=0;i<2;i++){
  rc=rw15_refresh(c);if(rc)return rw15_finish(c,rc);
  for(unsigned j=0;j<2;j++)if(c->site[j]!=(j<i?RW15_REDIRECT:RW15_ORIGINAL))return rw15_finish(c,RW15_PATCH);
  c->patch_calls++;
  int error=o->compare_publish_synchronized(p,i,c->original[i],c->redirect[i]);
  rc=rw15_refresh(c);if(rc)return rw15_finish(c,rc);
  if(error)return rw15_finish(c,RW15_PATCH);
  for(unsigned j=0;j<2;j++)if(c->site[j]!=(j<=i?RW15_REDIRECT:RW15_ORIGINAL))return rw15_finish(c,RW15_PATCH);
 }
 for(unsigned i=0;i<2;i++){o->publish_bypass_locked(p,i,0);c->bypass[i]=0;}
 c->enabled=1;c->fault=0;
 return rw15_finish(c,RW15_OK);
}
static inline int rw15_off(struct rw15_controller *c){
 const struct rw15_ops *o=c->ops;void *p=c->context;int rc;
 if(!o->framework_reference_held(p))return RW15_FRAMEWORK;
 if(o->enter_control(p))return RW15_BUSY;
 if(o->module_state(p)!=0)return rw15_finish(c,RW15_MODULE);
 if(!c->pinned)return rw15_finish(c,RW15_ALREADY);
 rw15_bypass_and_wake(c);
 rc=rw15_refresh(c);if(rc)return rw15_finish(c,rc);
 for(unsigned i=0;i<2;i++){
  rc=rw15_refresh(c);if(rc)return rw15_finish(c,rc);
  if(c->site[i]==RW15_ORIGINAL)continue;
  c->patch_calls++;
  int error=o->compare_publish_synchronized(p,i,c->redirect[i],c->original[i]);
  rc=rw15_refresh(c);if(rc)return rw15_finish(c,rc);
  if(error || c->site[i]!=RW15_ORIGINAL)return rw15_finish(c,RW15_PATCH);
 }
 if(c->site[0]!=RW15_ORIGINAL || c->site[1]!=RW15_ORIGINAL)return rw15_finish(c,RW15_PATCH);
 c->fault=0;
 return rw15_finish(c,RW15_OK); /* keeps permanent pin */
}
#endif
