#!/bin/bash
# Standalone SFTP subsystem; SSH transport/authentication remain in Dropbear.
# Usage: UB820_MUSL_TOOLCHAIN=/path/to/cross ./build_sftp.sh /path/to/openssh-10.6p2
set -euo pipefail
toolchain=${UB820_MUSL_TOOLCHAIN:?Set UB820_MUSL_TOOLCHAIN to the extracted musl cross directory}
source_dir=${1:?Pass the verified openssh-10.6p2 source directory}
recipe_dir=$(cd -- "$(dirname -- "$0")" && pwd)
export PATH="$toolchain/bin:$PATH"
python3 "$recipe_dir/patch_sftp.py" "$source_dir"
cd "$source_dir"
arm-linux-musleabihf-gcc -Os -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard \
  -c "$recipe_dir/linux44_compat.c" -o ub820_linux44_compat.o
arm-linux-musleabihf-gcc -Os -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard \
  -c "$recipe_dir/ssh_shell_spawn.c" -o ub820_ssh_shell_spawn.o
./configure --host=arm-linux-musleabihf --build=x86_64-pc-linux-gnu \
  CC=arm-linux-musleabihf-gcc AR=arm-linux-musleabihf-ar RANLIB=arm-linux-musleabihf-ranlib \
  CFLAGS='-Os -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard -ffunction-sections -fdata-sections' \
  LDFLAGS='-static -Wl,--gc-sections -Wl,--wrap=posix_spawn' \
  LIBS='./ub820_linux44_compat.o ./ub820_ssh_shell_spawn.o' \
  --without-openssl --without-zlib --without-pam --without-libedit \
  --without-kerberos5 --without-selinux \
  --without-security-key-builtin --without-stackprotect
make -j"${JOBS:-4}" sftp-server
arm-linux-musleabihf-strip sftp-server
if arm-linux-musleabihf-readelf -d sftp-server | grep -q NEEDED; then
  echo 'sftp-server unexpectedly requires dynamic libraries' >&2
  exit 1
fi
