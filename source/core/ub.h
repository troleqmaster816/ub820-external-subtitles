#ifndef UB_CORE_H
#define UB_CORE_H
/* UB820 QoL module interface.
 *
 * A module is one feature that can be switched on or off in sub/modules.cfg.
 * It consists of parts; each part works on one player process (or on none),
 * may carry an in-process payload, and gets a step() call once per second.
 * The core finds the process, checks its exact executable hash, installs the
 * payload, notices when the process ends and runs the circuit breaker. A part
 * must not touch a process whose hash was not verified (target->verified==1,
 * which is the only state in which step() is called for a process part).
 */
#include "injector.h"
#include "log.h"
#include "sys.h"
#include <stddef.h>
#include <stdint.h>

/* Integer option "key=value" in the module's [section]. names (optional,
 * NULL-terminated) lets "key=name" select min+index. */
struct ub_option {
 const char *key;
 int min,max;
 int *value;
 const char *const *names;
};

struct ub_target {
 int pid;
 unsigned long long born;     /* start time, tells a reused PID apart */
 int verified;                /* 1 exact hash, -1 mismatch, 0 not checked */
 int installed,failed,attempts;
 unsigned retry;
 unsigned changed;            /* order of this part's first change, 0 = none */
 int state[8];                /* part-private, zeroed for every new process */
};

struct ub_part {
 const char *name;                 /* log label inside the module */
 const char *process,*sha256;      /* NULL process: step() runs every second */
 const struct ub_payload *payload; /* optional, installed before step() */
 int guarded;                      /* may be switched off by the circuit breaker */
 void (*prepare)(struct ub_part *,struct ub_target *); /* before each install attempt */
 void (*step)(struct ub_part *,struct ub_target *);
 /* core-owned */
 struct ub_target target;
 const char *module;
 int tripped;
};

struct ub_module {
 const char *id,*summary;
 int enabled;                       /* default; the config may change it */
 const struct ub_option *options;   /* terminated by {0} */
 void (*start)(struct ub_module *); /* once, after the config is read */
 struct ub_part *parts;
 unsigned part_count;
 /* Optional command line handled before the helper starts (another process,
  * e.g. one the kernel runs); returns the exit code, or -1 if not its own. */
 int (*command)(int argc,char **argv);
};

extern struct ub_module *const ub_modules[]; /* generated/registry.c */

/* Record that a part changed its process; the newest guarded change is the
 * first one switched off if that process ends. */
void ub_changed(struct ub_part *part);
/* 1 when every enabled payload part for this process has finished installing
 * (installed, failed or switched off): one ptrace session at a time. */
int ub_settled(const char *process);
int ub_module_enabled(const char *id);

/* Process memory and files (core/procmem.c). */
int ub_mem_read(int pid,uint32_t address,void *buffer,size_t length);
int ub_mem_write(int pid,uint32_t address,const void *buffer,size_t length);
/* Start of the first mapping (file offset 0) whose path ends with suffix. */
uint32_t ub_map_base(int pid,const char *suffix,char *path,size_t path_size);
/* The /proc/mounts line whose mount point is the longest prefix of path. */
int ub_mount_for(const char *path,char *line,size_t line_size);

/* Config (core/config.c). */
void ub_config_load(void);
/* Apply a menu choice and save it to sub/ub820_menu.cfg on the stick:
 * 0 saved, -1 invalid value, >0 applied but not saved (ub_stick_error). */
int ub_setting_set(const char *module,const char *key,const char *value);

/* Core loop (core/core.c); split out so host tests can drive it. */
void ub_core_start(void);
void ub_core_tick(void);
#endif
