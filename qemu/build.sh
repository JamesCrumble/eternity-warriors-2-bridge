#!/bin/bash
# Builds a static x86_64 qemu-arm (linux-user) from the QEMU source tarball inside a container (nothing is
# installed on the host) and puts it at third_party/qemu/qemu-arm. play_wsl.sh uses it when present.
#   QEMU_TARBALL   source archive (default: the one the nova3 project downloaded, qemu-11.1.2.tar.xz;
#                  upstream: https://download.qemu.org/qemu-11.1.2.tar.xz)
#   QEMU_PATCHES   space separated patch files applied with patch -p1 (default: qemu/patches/nova3.patch + qemu/patches/x86_neon.patch)
#   QEMU_CFLAGS    extra compiler flags (default: -O3 -DNOVA3_JMP_BITS=16 -DNOVA3_FAST_LOOKUP)
#
# nova3.patch is host-conditional: its fast host-FP/Neon parts are under #if defined(__aarch64__), so the same
# patch set builds a tuned qemu-arm for an Android (aarch64) host; there qemu/patches/android/*.patch can be added
# (QEMU_PATCHES="qemu/patches/nova3.patch qemu/patches/android/neon_nofpcr.patch qemu/patches/android/fast_fp_inline.patch").
# x86_neon.patch adds an SSE path for single-precision Neon on x86-64 hosts (about 4x on Neon-heavy code, results
# identical to softfloat: anything unusual - NaN, denormal - falls back to it); it is compiled out on aarch64.
# No -march=native: the x86_64 binary stays portable.
#   QEMU_VARIANT   name of this build (own source copy, build dir and output third_party/qemu/qemu-arm-NAME;
#                  the default variant is installed as third_party/qemu/qemu-arm)
#   QEMU_NOHARDEN  1 (default): drop QEMU's default hardening flags (-ftrivial-auto-var-init, -fzero-call-used-regs)
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)   # project root; this script lives in qemu/
cd "$P"
TARBALL=${QEMU_TARBALL:-$P/../nova3probe/downloads/qemu-11.1.2.tar.xz}
[ -f "$TARBALL" ] || { echo "no QEMU tarball at $TARBALL (set QEMU_TARBALL)"; exit 1; }
NAME=$(basename "$TARBALL" .tar.xz)
VAR=${QEMU_VARIANT:-}
SRC=$P/build/qemu-src/$NAME${VAR:+-$VAR}
if [ ! -d "$SRC" ]; then
    mkdir -p "$P/build/qemu-src"
    tmp=$(mktemp -d "$P/build/qemu-src/x.XXXXXX")
    tar -xJf "$TARBALL" -C "$tmp"
    mv "$tmp/$NAME" "$SRC"; rmdir "$tmp"
    [ "${QEMU_NOHARDEN:-1}" = 1 ] && sed -i "s/^qemu_common_flags += hardening_flags/#&/" "$SRC/meson.build"
    for pf in ${QEMU_PATCHES:-$P/qemu/patches/nova3.patch $P/qemu/patches/x86_neon.patch}; do (cd "$SRC" && patch -p1 < "$pf"); done
fi
docker image inspect ew2-qemu-build >/dev/null 2>&1 || docker build -t ew2-qemu-build qemu
OBJ=$P/build/qemu-obj${VAR:+-$VAR}
OUT=third_party/qemu/qemu-arm${VAR:+-$VAR}
mkdir -p "$OBJ" third_party/qemu
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$P":/work ew2-qemu-build bash -c "
  set -e
  cd /work/build/$(basename $OBJ)
  [ -f build.ninja ] || /work/build/qemu-src/$(basename $SRC)/configure --target-list=arm-linux-user --static \
      --disable-system --disable-tools --disable-docs --disable-guest-agent --disable-plugins \
      --disable-werror --disable-rust --extra-cflags='-O2 ${QEMU_CFLAGS:--O3 -DNOVA3_JMP_BITS=16 -DNOVA3_FAST_LOOKUP}' > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
  ninja qemu-arm > ninja.log 2>&1 || { grep -E 'error' ninja.log | head -20; exit 1; }
  strip -o /work/$OUT qemu-arm
"
ls -la $OUT
$OUT --version | head -1
