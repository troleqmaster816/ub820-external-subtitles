# Writing a module

Every feature of the patch is a module in `source/modules/<id>/`. The core
(`source/core/`) does the risky plumbing once for everyone: finding player
processes, checking their exact firmware hash, installing payloads, noticing
when a process ends, switching off a part that coincides with a crash, logging
and reading `sub/modules.cfg`. A module only describes what it changes.

## Layout

```
source/modules/<id>/
├── module.json       id, build order, optional payloads
├── module.c          helper side: options, parts, step() functions
└── (optional)        payload source + hook manifest, local headers
```

`build.py` discovers the directory by itself. The module's `[id]` section in
`sub/modules.cfg` switches it on or off and sets its options.

`module.json`:

```json
{
  "id": "my_feature",
  "order": 60,
  "payloads": []
}
```

`order` decides the order in which modules run each second (lower first). A
module that has to wait for another one's payload uses `ub_settled()` rather
than relying on order alone.

## The interface (`source/core/ub.h`)

A module (`struct ub_module`) has an id, a one-line summary, a default
`enabled`, optional options and one or more **parts**. A part
(`struct ub_part`) works on one player process, or on none:

| Field | Meaning |
| --- | --- |
| `name` | label in the log |
| `process`, `sha256` | target program and its exact 1.82 hash; `NULL` process = runs every second without a target |
| `payload` | optional in-process code (see below), installed before `step()` runs |
| `guarded` | 1 if this part changes the process in a way that could crash it; the circuit breaker may then switch it off |
| `prepare` | optional, called before each payload install attempt |
| `step` | called once per second while the process runs **and its hash matched** |

`step(part, target)` gets `target->pid` and `target->state[8]`, which is
zeroed for every new process instance. Call `ub_changed(part)` the first
time the part changes the process, so the breaker knows who changed it last.

Helpers: `ub_mem_read`/`ub_mem_write` (process memory), `ub_map_base` (where
a library or file is mapped), `ub_mount_for`, `ub_overlay_pages` (swap whole
pages of a mapping for a private copy), `ub_hash_file`,
`ub_stick_read`/`ub_stick_write` (`core/storage.h`: copy a file between the
USB stick and `/tmp`; the stick is not mounted in Linux, so plain `open()` on
`/mnt/usb` does not work), and
`ub_log(id, level, ...)`. `UB_ALWAYS` lines are always kept; `UB_INFO` and
`UB_DEBUG` follow the `[log] level` setting.

## Template

```c
/* What the module does, and the firmware facts it relies on
 * (function/address names from the 1.82 analysis). */
#include "ub.h"
#include "payloads.h"   /* UB_SHA256_<PROGRAM> for programs with payloads */

#define ID "my_feature"
#define TARGET_ADDR 0x00123456u

static int strength=1;
static const struct ub_option options[]={
 {"strength",0,3,&strength,0},
 {0,0,0,0,0}
};

/* state[0]: 1 once applied in this process instance */
static void step(struct ub_part *p,struct ub_target *t){
 uint32_t value;
 if(t->state[0])return;
 if(ub_mem_read(t->pid,TARGET_ADDR,&value,4))return;
 if(value!=EXPECTED_STOCK_VALUE){t->state[0]=1;ub_log(ID,UB_ALWAYS,"unexpected value 0x%x; skipped",value);return;}
 /* ... change it ... */
 ub_changed(p);t->state[0]=1;
 ub_log(ID,UB_INFO,"applied in PID%d",t->pid);
}

static struct ub_part parts[]={
 {"main","apl_dispsrv",UB_SHA256_APL_DISPSRV,0,1,0,step,{0},0,0},
};
struct ub_module ub_module_my_feature={
 ID,"One line that says what the user gets",
 1,options,0,parts,sizeof parts/sizeof *parts
};
```

The exported name must be `ub_module_<id>`.

## Payloads (code inside a player process)

A payload is a small position-independent shared object with no
constructors. List it in `module.json`:

```json
"payloads": [{"name": "ui", "source": "ui.c", "manifest": "ui_hooks.json"}]
```

The manifest names the `program`, its `sha256`, the `hooks` (target address,
replacement function, original-pointer variable) and `bindings` (fixed
addresses handed to the payload), all for firmware 1.82. The full build turns
each hook into a trampoline from the stock executable and refuses to build if
two modules hook overlapping bytes. At run time the injector checks the
original bytes again before patching, and installs nothing if they differ. The
payload is then available to `module.c` as `ub_payload_<id>_<name>`.

Another firmware whose program changed gets a `variants` entry with its own
hash and the address of **every** hook (by replacement) and **every** binding
(by symbol); the build refuses a variant that leaves one out, and builds its
trampolines from that firmware's executable (`UB820_FIRMWARE_DIR_185` for
1.85). The payload's code is the same for all variants:

```json
"variants": [{"firmware": "1.85", "sha256": "…",
              "hooks": {"ub_hook_key_filter": "0x2e568"}, "bindings": {}}]
```

At run time the injector takes the variant whose hash is the process's; a
1.85 program that is byte for byte the 1.82 one (listed in `core/injector.c`)
takes the 1.82 variant. Give a variant only for code you compared with the
1.82 code instruction by instruction, and say how in its `note`.

Several payloads may live in the same process, one per module part.

## Rules

1. **Exact hash first.** Never touch a process (or a file you patch in
   memory) whose SHA-256 is not the one you analysed. The core does this for
   processes; check files yourself, as `external_subtitles/english_text.h` does.
2. **Verify before you write.** Read the bytes or value you are about to
   change and compare them with the stock value; if they differ, log it and
   do nothing.
3. **RAM only.** No writes to flash, EEPROM, firmware files or the drive.
4. **Prefer data over code.** A data write or page overlay is safer than a
   hook. If you need a hook, keep the payload small and on the thread that
   already calls the hooked function.
5. **Fail closed and say why.** If something is unexpected, leave the player
   as it is and write one `UB_ALWAYS` line explaining it.
6. **Mark risky parts `guarded`** and call `ub_changed()`.
7. **Document the firmware facts** in the module's header comment: which
   function or address, what it does, how you found it.

## Testing

- `python source/tests/run_tests.py` runs the host tests (x86, WSL). Add a
  `tests/<id>_test.c` for any parser or decision logic, like
  `tests/region_test.c`. `config_test` builds the real module set and checks
  that the shipped `modules.cfg` parses without warnings, so add your
  section there.
- `python source/build.py` (full) or `--helper` must build with
  `-Wall -Wextra -Werror`.
- On the player: test with `[log] level=debug`, read the log in OPTIONS >
  External Subtitles > Helper log, and say in your pull request which disc
  types and menu languages you tried.
