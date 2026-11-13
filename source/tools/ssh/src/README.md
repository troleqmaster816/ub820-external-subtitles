# Corresponding BusyBox source

`busybox-1.37.0.tar.bz2` is the complete upstream source archive. Its SHA-256
is recorded in `../sources.json`. The full configuration is `../busybox.config`;
all modifications and additional source are in `../patch_busybox.py`,
`../linux44_compat.c` and `../ssh_shell_spawn.c`. Use `../build_busybox.sh` with
the musl toolchain recorded in the source manifest to rebuild the bundled binary.
The GPL license is in `../licenses/busybox-LICENSE.txt` and in the source archive.
