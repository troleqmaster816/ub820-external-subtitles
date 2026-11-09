# SSH runtime for the UB820

The optional SSH module provides an ordinary root SSH session in RAM. It bundles
static ARM tools: Dropbear server/key generator/client, BusyBox ash and utilities,
legacy SCP, and a standalone OpenSSH SFTP server. The helper remains the single
file installed on the stick: these components are embedded by `source/build.py`.

The environment belongs to SSH sessions. No firmware passwd entry, system shell,
system profile or firmware filesystem is changed. There is no forced command,
command whitelist or production chroot. The optional diagnostic session guard
remains off by default.

## Session environment

- HOME and initial working directory: `/tmp/.ub820_ssh/home`.
- Shell: `/tmp/.ub820_ssh/bin/sh` (BusyBox ash).
- PATH starts with `/tmp/.ub820_ssh/bin`, then includes the normal firmware paths.
  Custom PATH entries retain normal precedence; ash does not force internal
  applets ahead of PATH.
- The private system login profile is `/tmp/.ub820_ssh/profile`; the usual
  `$HOME/.profile` and ENV handling remain available. Profiles and home contents
  survive Stop/Start until a power cycle.
- Incoming key authentication still reads the configured stick authorized_keys;
  outgoing SSH uses `$HOME/.ssh`, including keys and known_hosts.
- SFTP and both modern and legacy SCP work, including the managed root `~/` home.
  The bundled `ssh` command is Dropbear dbclient; host-key verification stays on.
- Standard session umask is 022. Signals and inherited descriptors are reset
  before launching tools; descriptor cleanup has no arbitrary 256-FD cap.

The firmware's absolute `/bin/sh` still means its original sash. Use `sh file.sh`
from the SSH PATH, or a `#!/tmp/.ub820_ssh/bin/sh` shebang for executable POSIX
scripts. This preserves factory scripts that intentionally use sash. Bundled
utilities' implicit shell helpers use the managed POSIX shell.

All managed directories are private (0700), secrets are 0600, and tools are
published as complete files by rename. The RAM host key is reused across
Stop/Start; failed key generation never publishes a partial final key. A full
power cycle removes the environment and host identity, so the client must
verify a new host key then. Authentication is not weakened to hide that change.

## Linux 4.4 compatibility

The target has an ARM64 Linux 4.4.8 kernel with ARM32 compat applications. Tools
are static musl ARMv7 hardfloat binaries. `linux44_compat.c` preserves musl's
public 64-bit time ABI while making the older ARM clock/stat/timestamp syscalls
directly, avoiding repeated unsupported time64/statx probes on the vendor kernel.
The time32 kernel interface reports overflow rather than truncating times beyond
its representable range.

Only the standalone SFTP filesystem subsystem is built from OpenSSH; it does not
implement SSH transport or authentication. Those remain in Dropbear. SFTP uses the
same managed root home for its tilde expansion, without editing `/etc/passwd`.

## Rebuilding

Exact upstream sources/toolchain checksums are in `sources.json`; packaged
binary hashes and sizes are in `binaries.json`. Verify downloaded archives against
those values before unpacking. The BusyBox source archive is included under
`src/`, alongside this repository's complete config, patches and build recipe.
Corresponding license texts are in `licenses/`.

Use a Linux/WSL build environment with make, Python 3 and qemu-arm. The extracted
musl cross toolchain directory contains `bin/arm-linux-musleabihf-gcc`.

1. Apply `python3 patch_dropbear.py <clean-dropbear-source>` and run
   `UB820_MUSL_TC=<toolchain>/bin bash build_dropbear.sh <patched-source>`.
   Copy its dropbear, dropbearkey, scp and dbclient into `bin/` after validation.
2. Run `bash build_busybox.sh <busybox-source> <toolchain>`. It applies the patch,
   installs `busybox.config`, builds and writes `bin/busybox` by default.
3. Run `UB820_MUSL_TOOLCHAIN=<toolchain> bash build_sftp.sh <openssh-source>`.
   It applies `patch_sftp.py`, links the shared compatibility code and builds
   sftp-server. Copy the validated result to `bin/sftp-server`.
4. Run the repository's normal full build with both firmware inputs to regenerate
   `generated/tools.c` and the helper. The build hash self-test uses a generated
   text fixture and does not read a media file.

## Tests

`source/tests/run_tests.py` includes the SSH module and RAM-preparation unit tests:
real descriptor cleanup, inherited signal/umask state, atomic publication failure,
private directories, profiles, nonblocking rejection of FIFOs, and key-generation
failure/retry/reuse.

`test_linux44_compat.c` exercises the tool ABI under the musl cross compiler and
qemu-arm: stat fields, symlinks, large files, timestamp precision, sleep EINTR and
range errors. Link it together with `linux44_compat.c` as a static ARM executable.

`python source/tools/ssh/test_ssh_runtime.py` runs real ARM SSH binaries under
QEMU in a disposable local mount/PID namespace and chroot. It tests key/password
login, failure paths, HOME/cwd/SHELL/PATH, normal commands, quoting, binary I/O,
PTY, scripts, outgoing SSH, TCP forwarding, SFTP, both SCP protocols, ordinary
PATH overrides, EOF during host-key confirmation, and awk shell helpers. The
chroot is a test fixture, not a restriction imposed on player sessions.

These tests do not replace the combined package test on the physical player.
