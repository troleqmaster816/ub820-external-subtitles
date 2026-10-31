# ssh tool: Dropbear for the UB820

The `ssh` module runs a small [Dropbear](https://matt.ucc.asn.au/dropbear/)
SSH server on the player, on demand. The two binaries in [`bin/`](bin) —
`dropbear` (server) and `dropbearkey` (host-key generator) — are **prebuilt,
static musl ARM binaries**: self-contained, with no dependency on the player's
libraries, their own `getpwnam` and `crypt()`. They run on the player's 4.4
kernel. `build.py` embeds them into the helper (as `ub_tool_dropbear` /
`ub_tool_dropbearkey`), like the `ufs` tool.

A glibc build is not used on purpose: compiling against a modern cross
toolchain (glibc 2.39) but running on the player's glibc 2.20 pulls in symbols
the old libc lacks (time64, `fcntl64`, `__isoc23_*`). A static musl binary
sidesteps all of that.

## Reproducing the binaries

Needs WSL (Ubuntu) with `curl`, and `qemu-arm` for the smoke test.

```bash
# 1. a musl ARM hardfloat cross toolchain
curl -fsSLO https://musl.cc/arm-linux-musleabihf-cross.tgz
tar xzf arm-linux-musleabihf-cross.tgz          # -> ./arm-linux-musleabihf-cross

# 2. the exact Dropbear release
curl -fsSLO https://matt.ucc.asn.au/dropbear/releases/dropbear-2022.83.tar.bz2
# sha256: bc5a121ffbc94b5171ad5ebe01be42746d50aa797c9549a4639894a16749443b
tar xjf dropbear-2022.83.tar.bz2

# 3. apply the UB820 patches + localoptions.h
python3 patch_dropbear.py dropbear-2022.83

# 4. build (edit the toolchain path in build_dropbear.sh if needed)
bash build_dropbear.sh dropbear-2022.83
```

Expected SHA-256 of the result (the same binaries are also embedded in
`USB-CONTENTS/sub/engage`):

- `dropbear`    `8fa0a1be7401dff7583b644b49380c4272f3a7dcdee02c84b6cc3ee10b58f0d5`
- `dropbearkey` `4e79cf5ff749d09c2b37b422f11c6985a4bd6e651c9b7c339a0b005fe8fd67dc`

## The patches ([`patch_dropbear.py`](patch_dropbear.py))

Three small source changes: the device has no writable home, no
`/etc/shadow`, and runs Linux 4.4:

1. **`svr-authpubkey.c`** — read `authorized_keys` from the fixed path
   `/tmp/.ub820_ssh/authorized_keys` instead of `~/.ssh/authorized_keys`
   (`/root` is read-only), and skip the home-directory permission checks.
2. **`svr-authpasswd.c`** — take the password hash from
   `/tmp/.ub820_ssh/pwhash` (written by the helper) instead of the shadow
   password, which does not exist on this device.
3. **`ub820_time32.c`** (included at the end of `dbutil.c`) — `clock_gettime`
   through the 32-bit syscall 263. musl 1.2 tries `clock_gettime64`
   (syscall 403, Linux 5.1+) first on every call; on the player each failure
   made the kernel log a register dump, about 240 per ssh session. It
   replaces musl's `__clock_gettime` and `__clock_gettime64`, so musl's own
   version is not linked. Checked under `qemu-arm -strace` with a password
   login and `exec_command`: no `clock_gettime64`, `statx` or other `*_time64`
   call is left.

`localoptions.h` builds the server only, with password and public-key auth,
ed25519/ecdsa/rsa host keys, no zlib, no syslog/utmp, and `DROPBEAR_REEXEC 0`
(no per-connection re-exec). The helper generates an ephemeral ed25519 host
key with `dropbearkey` at startup.

## How the module uses them

See [`../../modules/ssh/module.c`](../../modules/ssh/module.c). On "Start SSH"
it writes the binaries to `/tmp`, copies the chosen secret from the stick
through the player's file layer, prepares `/tmp/.ub820_ssh/` (host key, and
`authorized_keys` or a `$6$` password hash from
[`sha512crypt.h`](../../modules/ssh/sha512crypt.h)), mounts devpts, and
launches `dropbear`. Everything is in RAM; a power cycle removes it.

## Licenses

Dropbear is distributed under its own MIT-style terms and the binaries are
statically linked with musl (MIT). Their license texts are in
[`licenses/`](licenses): [`dropbear-LICENSE.txt`](licenses/dropbear-LICENSE.txt)
(from the 2022.83 release) and [`musl-COPYRIGHT.txt`](licenses/musl-COPYRIGHT.txt).
