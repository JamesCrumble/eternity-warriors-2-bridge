#!/usr/bin/env bash
# Assembles the guest's root filesystem for ew3host.exe: the port binary and every armhf shared library
# it needs (glibc and its interpreter included - on Windows there is no system to borrow them from).
#   tools/make_rootfs.sh [out-dir]       default: build/win/rootfs
# Guest layout: /bin/ew3, /lib/ld-linux-armhf.so.3, /lib/arm-linux-gnueabihf/*.so*
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$P/build/win/rootfs}
BIN=$P/port/build/ew3
READELF=arm-linux-gnueabihf-readelf
SEARCH="$P/sdl-arm/lib /usr/arm-linux-gnueabihf/lib /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf"
[ -f "$BIN" ] || { echo "no $BIN - run ./build.sh first" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/bin" "$OUT/lib/arm-linux-gnueabihf" "$OUT/etc" "$OUT/tmp"
cp "$BIN" "$OUT/bin/ew3"
declare -A seen
find_lib() { for d in $SEARCH; do [ -e "$d/$1" ] && { readlink -f "$d/$1"; return; }; done; echo ""; }
walk() {
    local f=$1 n
    for n in $($READELF -dW "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        [ -n "${seen[$n]:-}" ] && continue
        seen[$n]=1
        local src; src=$(find_lib "$n")
        [ -n "$src" ] || { echo "missing: $n (needed by $(basename "$f"))" >&2; exit 1; }
        if [ "$n" = ld-linux-armhf.so.3 ]; then cp "$src" "$OUT/lib/$n"; else cp "$src" "$OUT/lib/arm-linux-gnueabihf/$n"; fi
        walk "$src"
    done
}
walk "$BIN"
cp "$(find_lib ld-linux-armhf.so.3)" "$OUT/lib/ld-linux-armhf.so.3"
# glibc's resolver and NSS are never reached (the port answers getaddrinfo itself); a minimal /etc keeps
# libc's lookups quiet.
printf 'root:x:0:0:root:/root:/bin/sh\nplayer:x:1000:1000::/home/player:/bin/sh\n' > "$OUT/etc/passwd"
printf 'hosts: files\n' > "$OUT/etc/nsswitch.conf"
printf '127.0.0.1 localhost\n' > "$OUT/etc/hosts"
echo "rootfs: $OUT"; ls "$OUT/lib/arm-linux-gnueabihf" | tr '\n' ' '; echo; du -sh "$OUT"
