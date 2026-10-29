# UB820 External Subtitle Menu (firmware 1.82)

A RAM-only patch for the **Panasonic DP-UB820** Ultra HD Blu-ray player
(firmware **1.82**) that lets you choose and control external `.srt` subtitles
from a USB stick while a disc is playing.

It adds an **External Subtitles** entry to the player's own OPTIONS menu, lets
you browse the USB stick and switch subtitle files mid-film, and draws
subtitles with a disc-like look: white text, neutral black outline, no
background band.

> [!NOTE]
> Tested and working on the author's own DP-UB820 with firmware 1.82. Other
> units on the same firmware should behave the same, but that has not been
> checked.

## Features

- **External Subtitles menu** in OPTIONS during playback
  - browse folders on the USB stick (folders first, then `.srt` files)
  - pick any `.srt` file; it takes over at the current film position
  - **Off / On** without losing the selected file
  - **Reload current subtitle**: re-reads an edited file without restarting
  - **Refresh files**: rescans the current folder
- **Neutral black outline.** White text with an opaque black outline
  (`0xFF000000`) and neutral grey anti-aliased edges.
- **Stable bottom baseline.** When a cue goes from one line to two, the extra
  line grows upward instead of pushing the bottom line lower. Three- and
  four-line cues keep the stock behaviour.
- **Size and position calibration** in `sub/disc_style.cfg`.
- **Nothing is written to flash.** No firmware files, flash or EEPROM are
  modified. A full power cycle removes the patch completely.

## Requirements

- Panasonic DP-UB820 running firmware **1.82**. The loader checks exact
  SHA-256 hashes of the target executables and refuses to install hooks on
  anything else:
  - `apl_dispsrv`: `3ca762ac044acba426c45be240075f2644fc9f645a0fbbb1aabc4c8656dcec1d`
  - `apl_meda_bdplay`: `a07b82bc4a3a50594ee97ce9ad6937f8f63b129da0779c15576691e06da9789b`
- A USB stick formatted as **FAT32**. This is the only format the patch was
  tested with.

## Installation

1. Copy **the contents** of [`USB-CONTENTS/`](USB-CONTENTS) to the **root** of
   the USB stick, keeping the `sub` folder:

   ```
   USB root
   ├── UB820-SUBTITLES.MKV   loader (launch this one)
   └── sub/
       ├── engage            helper: menu + subtitle controller + RAM loader
       ├── disc_style.cfg    optional look calibration
       └── sub.srt           native default subtitle (fallback)
   ```

   Put your own `.srt` files anywhere on the stick. Subfolders work.

2. **Fully power-cycle the player** (unplug it, or turn off Quick Start), so
   the helper starts fresh. Standby keeps whatever is already running in RAM.
3. Insert the stick and launch `UB820-SUBTITLES.MKV` with your usual loader
   startup procedure. The player will show an error that the file cannot be
   opened. This is expected: the file only triggers the helper, so there is
   nothing to play. Dismiss the message and continue.
4. Insert the disc and start the film. Wait a few seconds, then press
   **OPTIONS**.
5. Select **External Subtitles** and enter it with **OK** or **Right**.

## Using the menu

| Key | Action |
| --- | --- |
| Up / Down | move between rows |
| OK / Right | open a folder, select a file, run an action |
| Return / Left | close the submenu and return to OPTIONS |
| OPTIONS | close everything as usual |

The header shows the current folder and the active state, for example
`On | /mnt/usb/SUB/menu_polish.srt`. Rows, top to bottom: **[..] Up one
folder**, **Off**, **On**, **Reload current subtitle**, **Refresh files**,
then the subfolders, then the `.srt` files.

## Configuration: `sub/disc_style.cfg`

The helper reads this file once at startup. Power-cycle the player after
editing it.

| Key | Default | Range | Meaning |
| --- | --- | --- | --- |
| `font_percent` | `122` | 90 to 150 | Font scale. `124` is slightly larger. |
| `position` | `-8` | -10 to 10 | Native vertical step. Lower numbers move text down. |
| `bottom_align` | `1` | 0 or 1 | `1` keeps one- and two-line cues on the same bottom line. `0` restores stock row selection. |

The helper ignores invalid or out-of-range values and uses the defaults
instead. The player's own subtitle position and size settings still work.

## How it works

```
UB820-SUBTITLES.MKV ──► starts sub/engage (supervisor, static ARM binary)
                            │
                            ├─ injects menu payload    ──► apl_dispsrv      (OPTIONS UI)
                            └─ injects backend payload ──► apl_meda_bdplay  (SRT engine)
                                        ▲                         │
                                        └── /tmp request/status ──┘
```

- **Supervisor** ([`source/src/supervisor.c`](source/src/supervisor.c),
  [`engage_core.c`](source/src/engage_core.c),
  [`disc_style.h`](source/src/disc_style.h)): finds the two player processes,
  checks the firmware hashes, installs both payloads with
  [`injector.c`](source/src/injector.c), applies the subtitle style (font,
  outline colour, row layout) and turns on the native external-SRT engine.
- **Menu payload** ([`menu.c`](source/src/menu.c)): runs inside
  `apl_dispsrv`. It adds the OPTIONS entry and the file browser using the
  player's own CTK/GTK widgets.
- **Backend payload** ([`backend.c`](source/src/backend.c)): runs inside
  `apl_meda_bdplay`. It hooks the native ESL Open/Draw/Close functions, lists
  the USB folders, and restarts the stock SRT parser on the selected file.
- **IPC** ([`protocol.h`](source/src/protocol.h)): fixed-size, versioned
  request and status records in `/tmp`. Status is written atomically
  (temporary file, then rename). Each record carries an epoch so stale
  requests from a previous playback session are rejected.
- **Hook definitions:** [`source/ui_hooks.json`](source/ui_hooks.json) and
  [`source/src/backend-hooks.json`](source/src/backend-hooks.json). Each hook
  checks that the original instruction bytes match before patching. If they
  do not match, nothing is installed.

The runtime log is written to `/tmp/ub820_submenu.log` on the player. The
patch starts no network service.

## Building from source

The prebuilt `USB-CONTENTS/sub/engage` is reproducible: building from this
repository gives a byte-identical file with SHA-256
`9570a94f4617a3980350dca64a74c3744a01c76c471bef4159c11f30bf0f5e9d`.

### Helper only (uses the committed `embedded_payloads.h`)

On Linux or WSL with an ARM hard-float cross compiler:

```bash
cd source
arm-linux-gnueabihf-gcc -std=gnu11 -static -Os -s -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard -fno-strict-aliasing -ffunction-sections -fdata-sections -Wl,--gc-sections -Wall -Wextra -Werror -Wno-unused-function -Isrc src/supervisor.c src/injector.c -o engage
```

```bash
qemu-arm ./engage --selftest
```

### Full rebuild (payloads + hook trampolines)

[`source/build.py`](source/build.py) recompiles `menu.c` and `backend.c`,
generates the hook trampolines from the stock firmware binaries, writes
`src/embedded_payloads.h`, and builds and self-tests `engage`. It needs:

- Windows with WSL (`Ubuntu`) providing `arm-linux-gnueabihf-gcc` and
  `qemu-arm`
- Python packages `pyelftools`, `capstone` and `keystone-engine`
- the stock 1.82 `apl_dispsrv` and `apl_meda_bdplay` executables. They are
  not included in this repository.

```bash
UB820_FIRMWARE_DIR=/path/to/v182/dtvrec/bin python source/build.py
```

The output goes to `source/build/`.

## Known limitations

- Works only with firmware 1.82 and the exact hashes listed above.
- At most 64 entries per folder. Split large libraries into subfolders. Full
  paths are limited to 250 bytes.
- The selected file is remembered for the current playback session only, not
  across a power cycle.
- A media or state change may close the submenu. Open it again.
- Not covered by testing so far: unusual filename encodings, very large or
  corrupt `.srt` files, and removing the stick while a file is being read.

## Troubleshooting

- **The menu does not appear.** Power-cycle the player fully, then launch the
  MKV again before starting the disc.
- **The UI or playback stalls.** Remove the stick and power-cycle the player.
  All changes live in RAM, so a power cycle clears them.
- **A `disc_style.cfg` change has no effect.** The file is read only at
  startup. Power-cycle the player and launch the MKV again.

## License

Released under the MIT License. See [`LICENSE`](LICENSE).

## Disclaimer

This is an unofficial project with no affiliation to Panasonic. It modifies
running processes on your own device and is provided as is, with no warranty.
