#!/bin/bash
# Cross-build the patched dropbear server for the UB820 as a static musl binary:
# self-contained, no dependency on the player's libraries, its own getpwnam and
# crypt(). Runs on the player's 4.4 kernel.
set -e
TC="$HOME/ub-ssh/arm-linux-musleabihf-cross/bin"
CC="$TC/arm-linux-musleabihf-gcc"
STRIP="$TC/arm-linux-musleabihf-strip"
RE="$TC/arm-linux-musleabihf-readelf"
D="${1:-$HOME/ub-ssh/dropbear-2022.83}"   # the patched source tree
cd "$D"
make distclean >/dev/null 2>&1 || true

./configure --host=arm-linux-musleabihf --build=x86_64-pc-linux-gnu \
  CC="$CC" \
  CFLAGS="-Os -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard" \
  LDFLAGS="-static -Wl,--gc-sections" \
  --disable-harden --disable-zlib --disable-syslog --disable-utmp --disable-wtmp --disable-lastlog \
  --disable-loginfunc --disable-pututline --disable-pututxline \
  > /tmp/cfg.log 2>&1
echo "configure:"; grep -i "password authentication\|checking for crypt\b\|zlib" /tmp/cfg.log | head

make dropbear dropbearkey MULTI=0 -j"$(nproc)" > /tmp/make.log 2>&1 || { echo "MAKE FAILED"; tail -40 /tmp/make.log; exit 1; }
"$STRIP" dropbear dropbearkey
echo "built:"; ls -l dropbear dropbearkey
echo "=== file type ==="; file dropbear
echo "=== dynamic section (should be none: static) ==="; "$RE" -d dropbear 2>/dev/null | grep -E "NEEDED" || echo "(no NEEDED - fully static)"
echo "=== keygen test under qemu ==="; rm -f /tmp/hk; qemu-arm ./dropbearkey -t ed25519 -f /tmp/hk 2>&1 | head -4; ls -l /tmp/hk 2>/dev/null && echo "KEYGEN OK"
