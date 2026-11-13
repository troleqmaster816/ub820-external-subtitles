#ifndef FULL_CODE_GUARD_V7_H
#define FULL_CODE_GUARD_V7_H
/* Read-only host prototype, not a module or activation API. Expected manifest,
 * names and complete byte arrays must be trusted, immutable and alive. Only
 * symbol lookup and byte reads are exposed; no mutation callback exists. */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif
#ifndef UINT64_MAX
#define UINT64_MAX (~UINT64_C(0))
#endif
#else
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#endif

#define GUARD_V7_REQUIRED_COUNT 49u
#define GUARD_V7_MAX_WINDOW UINT64_C(65536)
enum guard_v7_build { GUARD_V7_V182=0,GUARD_V7_V185=1 };
enum guard_v7_result { GUARD_V7_OK=0,GUARD_V7_METADATA=-1,GUARD_V7_PROFILE=-2,GUARD_V7_LOOKUP=-3,GUARD_V7_READ=-4,GUARD_V7_BYTES=-5 };
struct guard_v7_window { const char *name;uint64_t address,length;const unsigned char *base,*pan_on; };
struct guard_v7_manifest { enum guard_v7_build build;size_t count;const struct guard_v7_window *windows; };
struct guard_v7_shape { uint64_t address,length; };
struct guard_v7_ops {
 int (*lookup)(void *,const char *,uint64_t *);
 int (*read)(void *,uint64_t,void *,size_t);
};
struct guard_v7_report { int result;size_t window_index;uint64_t byte_offset,checked_bytes;size_t checked_windows,lookups,reads; };
#include "full_code_guard_shape_v7.h"

static inline int guard_v7_finish(struct guard_v7_report *r,int result){r->result=result;return result;}
static inline int guard_v7_validate(const struct guard_v7_manifest *manifest,int reported_pan,
 const struct guard_v7_ops *ops,void *context,struct guard_v7_report *report){
 if(!report)return GUARD_V7_METADATA;
 memset(report,0,sizeof *report);report->window_index=SIZE_MAX;
 if(reported_pan!=0 && reported_pan!=1)return guard_v7_finish(report,GUARD_V7_PROFILE);
 if(!manifest || !ops || !ops->lookup || !ops->read || !manifest->windows ||
    manifest->count!=GUARD_V7_REQUIRED_COUNT || (manifest->build!=GUARD_V7_V182 && manifest->build!=GUARD_V7_V185))
  return guard_v7_finish(report,GUARD_V7_METADATA);
 /* Validate the ENTIRE canonical shape before any lookup/read callback. A
  * duplicate/reordered/shortened window cannot masquerade as the full set. */
 for(size_t i=0;i<GUARD_V7_REQUIRED_COUNT;i++){
  const struct guard_v7_window *g=&manifest->windows[i];
  const struct guard_v7_shape *shape=&guard_v7_shapes[manifest->build][i];
  report->window_index=i;
  if(!g->name || !g->base || !g->pan_on || !g->length || g->length>GUARD_V7_MAX_WINDOW ||
     g->length>UINT64_MAX-g->address || strcmp(g->name,guard_v7_required_names[i]) ||
     g->address!=shape->address || g->length!=shape->length)
   return guard_v7_finish(report,GUARD_V7_METADATA);
 }
 for(size_t i=0;i<GUARD_V7_REQUIRED_COUNT;i++){
  const struct guard_v7_window *g=&manifest->windows[i];
  const unsigned char *expected=reported_pan?g->pan_on:g->base;
  uint64_t actual_address=0;unsigned char bytes[32];
  report->window_index=i;report->byte_offset=0;report->lookups++;
  if(ops->lookup(context,g->name,&actual_address) || !actual_address || actual_address!=g->address)
   return guard_v7_finish(report,GUARD_V7_LOOKUP);
  for(uint64_t offset=0;offset<g->length;){
   size_t count=(size_t)(g->length-offset);if(count>sizeof bytes)count=sizeof bytes;
   report->byte_offset=offset;report->reads++;
   if(ops->read(context,g->address+offset,bytes,count))return guard_v7_finish(report,GUARD_V7_READ);
   for(size_t j=0;j<count;j++){
    if(bytes[j]!=expected[(size_t)offset+j]){report->byte_offset=offset+j;return guard_v7_finish(report,GUARD_V7_BYTES);}
    report->checked_bytes++;
   }
   offset+=count;
  }
  report->checked_windows++;
 }
 report->window_index=SIZE_MAX;report->byte_offset=0;
 return guard_v7_finish(report,GUARD_V7_OK);
}
#endif
