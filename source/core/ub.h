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
 /* Install refused because the hook sites already held changed code (for
  * example a previous helper run's hooks, still resident this boot). */
 int changed_code;
 unsigned retry;
 unsigned changed;            /* order of this part's first change, 0 = none */
 int state[8];                /* part-private, zeroed for every new process */
};

struct ub_part {
 const char *name;                 /* log label inside the module */
 const char *process,*sha256;      /* NULL process: step() runs every second */
 const struct ub_payload *payload; /* optional, installed before step() */
 int guarded;                      /* may be switched off by the circuit breaker */
 /* Before install: setting target->retry defers without consuming an install
  * attempt; setting target->failed refuses installation for this instance. */
 void (*prepare)(struct ub_part *,struct ub_target *);
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
 /* Optional disabled-module housekeeping, once after config and on existing
  * core ticks. May reconcile/finish an already resident operation; must not
  * activate the disabled feature. NULL leaves disabled modules untouched. */
 void (*disabled_step)(struct ub_module *);
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
/* The menu configuration was read completely or confirmed absent at startup. */
int ub_menu_known_at_start(void);
/* Apply a menu choice and save it to sub/ub820_menu.cfg on the stick:
 *   0   applied and saved;
 *   -1  invalid module, key or value: nothing applied (the only negative);
 *   >0  applied in RAM but not saved; ub_stick_error(code) says why. This
 *       includes the storage tool not starting (UB_STICK_NO_TOOL) or timing
 *       out (UB_STICK_TIMEOUT), and UB_STICK_UNREAD: the stick's file could
 *       not be read at start, so it is left untouched and the choice is saved
 *       with it, merged, by a later call once it reads (see core/storage.h).
 * A caller must treat >0 as "applied", e.g. publish the new value. */
int ub_setting_set(const char *module,const char *key,const char *value);
/* ub_setting_set in two steps: stage several choices, then save them with one
 * USB write. stage returns 0 or ub_setting_set's -1/UB_STICK_UNREAD; flush
 * returns ub_setting_set's save result. */
int ub_setting_stage(const char *module,const char *key,const char *value);
int ub_setting_flush(void);
/* Prerequisite setting: failure restores RAM and the later-save set. A USB
 * timeout remains an unknown disk outcome; no disk rollback is promised. */
int ub_setting_set_saved(const char *module,const char *key,const char *value);

/* Core loop (core/core.c); split out so host tests can drive it. */
void ub_core_start(void);
void ub_core_tick(void);
#endif
