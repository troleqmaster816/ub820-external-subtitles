#!/bin/bash
# Cross-build an already-patched Dropbear 2022.83 source tree. All output stays
# in that tree; copying validated binaries into bin/ is a separate packaging step.
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
WORKSPACE=$(cd -- "$SCRIPT_DIR/../../.." && pwd)
D=${1:?Usage: build_dropbear.sh patched-source-directory}
D=$(cd -- "$D" && pwd)
test -f "$D/localoptions.h" && grep -q UB820_HOME_PATH "$D/localoptions.h" || {
  echo "Apply patch_dropbear.py to a clean Dropbear 2022.83 tree first." >&2
  exit 1
}
TC=${UB820_MUSL_TC:-$WORKSPACE/analysis/ssh-runtime/arm-linux-musleabihf-cross/bin}
TC=$(cd -- "$TC" && pwd)
LOG_DIR="$D/ub820-build-logs"
mkdir -p -- "$LOG_DIR"

# Keep compiler command names free of spaces while preserving the complete
# toolchain path as one PATH entry. Autoconf and recursive make both resolve it.
export PATH="$TC:$PATH"
CC=arm-linux-musleabihf-gcc
cd -- "$D"
make distclean > "$LOG_DIR/distclean.log" 2>&1 || true

./configure --host=arm-linux-musleabihf --build=x86_64-pc-linux-gnu \
  CC="$CC" \
  CFLAGS="-Os -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard" \
  LDFLAGS="-static -Wl,--gc-sections" \
  --disable-harden --disable-zlib --disable-syslog --disable-utmp --disable-wtmp --disable-lastlog \
  --disable-loginfunc --disable-pututline --disable-pututxline \
  > "$LOG_DIR/configure.log" 2>&1 || { tail -40 "$LOG_DIR/configure.log"; exit 1; }

make dropbear dropbearkey scp dbclient MULTI=0 -j"${UB820_BUILD_JOBS:-$(nproc)}" \
  > "$LOG_DIR/make.log" 2>&1 || { tail -60 "$LOG_DIR/make.log"; exit 1; }
"$TC/arm-linux-musleabihf-strip" dropbear dropbearkey scp dbclient
for binary in dropbear dropbearkey scp dbclient; do
  "$TC/arm-linux-musleabihf-readelf" -d "$binary" > "$LOG_DIR/$binary-dynamic.txt"
  if grep -q 'NEEDED' "$LOG_DIR/$binary-dynamic.txt"; then
    echo "$binary unexpectedly needs shared libraries" >&2
    exit 1
  fi
done

KEY_DIR=$(mktemp -d)
trap 'rm -f -- "$KEY_DIR/hostkey"; rmdir -- "$KEY_DIR"' EXIT
qemu-arm ./dropbearkey -t ed25519 -f "$KEY_DIR/hostkey" > "$LOG_DIR/keygen.log" 2>&1
test -s "$KEY_DIR/hostkey"
sha256sum dropbear dropbearkey scp dbclient
echo "Static ARM Dropbear server/client, key generator and SCP built; keygen smoke test passed."
