#!/bin/bash
# Rebuild the bundled BusyBox from the matching source archive, config and patches.
# Usage: build_busybox.sh <busybox-1.37.0 source> <musl toolchain root> [output]
set -euo pipefail
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
src=$(realpath "${1:?BusyBox source directory required}")
tc=$(realpath "${2:?musl toolchain root required}")
out=$(realpath -m "${3:-"$script_dir/bin/busybox"}")
# Old Kbuild's fixdep cannot parse escaped spaces. Logical aliases plus GCC's
# no-canonical-prefixes keep all dependency names usable, even in this repo path.
aliases=$(mktemp -d /tmp/ub820-busybox-build.XXXXXX)
trap 'rm -f "$aliases/src" "$aliases/tc"; rmdir "$aliases"' EXIT
ln -s "$src" "$aliases/src"
ln -s "$tc" "$aliases/tc"
python3 "$script_dir/patch_busybox.py" "$src"
cp "$script_dir/busybox.config" "$src/.config"
export SOURCE_DATE_EPOCH=1727395200 KCONFIG_NOTIMESTAMP=1
cd "$aliases/src"
make CURDIR="$aliases/src" CROSS_COMPILE="$aliases/tc/bin/arm-linux-musleabihf-" oldconfig </dev/null
make CURDIR="$aliases/src" CROSS_COMPILE="$aliases/tc/bin/arm-linux-musleabihf-" \
  EXTRA_CFLAGS="-no-canonical-prefixes -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard" \
  -j"${JOBS:-8}" busybox
"$aliases/tc/bin/arm-linux-musleabihf-strip" busybox
if "$aliases/tc/bin/arm-linux-musleabihf-readelf" -d busybox | grep -q NEEDED; then
  echo "BusyBox unexpectedly needs shared libraries" >&2; exit 1
fi
mkdir -p "$(dirname "$out")"
install -m 0755 busybox "$out"
sha256sum "$out"
qemu-arm "$out" --list | wc -l
