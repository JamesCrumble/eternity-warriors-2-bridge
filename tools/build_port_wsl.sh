#!/bin/bash
# Builds port/build/ew2 and sdl-arm/lib/libSDL2-2.0.so.0 with the system arm-linux-gnueabihf toolchain (no zig).
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
cd "$P"
CROSS=arm-linux-gnueabihf-
SDLINC=/usr/include/SDL2
mkdir -p port/build/obj sdl-arm/lib
INC="-Iport -Iport/src -Iport/android -Iport/loader -Iport/thunks -Iport/thunks/libc -Iport/thunks/libc/generated -Iport/jni -Ithird_party/khronos/include -I$SDLINC"
FLAGS=(-std=gnu++2a -O2 -mcpu=cortex-a15 -marm -fno-strict-aliasing -fuse-cxa-atexit -Wno-unused-parameter '-DPORT_ENV_PREFIX="EW2"' -D_REENTRANT -DMESA_EGL_NO_X11_HEADERS -DEGL_NO_X11 -MMD -MP)
SRCS=$(for d in src android loader thunks/libc thunks/khronos jni jni/classes third_party/powervr; do ls port/$d/*.cpp; done)
OBJS=""
PIDS=()
FAIL=0
for s in $SRCS; do
    o=port/build/obj/${s#port/}.o
    OBJS="$OBJS $o"
    if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || { [ -f "${o%.o}.d" ] && [ -n "$(find $(sed 's/\\$//; s/^[^:]*://' "${o%.o}.d" | tr -s ' \n' '\n\n') -newer "$o" 2>/dev/null | head -1)" ]; }; then
        mkdir -p "$(dirname "$o")"
        echo "CXX $s"
        ${CROSS}g++ "${FLAGS[@]}" $INC -c $s -o $o &
        PIDS+=($!)
        if [ ${#PIDS[@]} -ge "$(nproc)" ]; then wait "${PIDS[0]}" || FAIL=1; PIDS=("${PIDS[@]:1}"); fi
    fi
done
for p in "${PIDS[@]}"; do wait "$p" || FAIL=1; done
[ $FAIL = 0 ] || { echo "compile failed"; exit 1; }
SHIM=sdl-arm/lib/libSDL2-2.0.so.0
if [ ! -f $SHIM ] || [ tools/sdl_shim.cpp -nt $SHIM ]; then
    ${CROSS}gcc -x c -mcpu=cortex-a15 -shared -fPIC -O2 -I$SDLINC -Wl,-soname,libSDL2-2.0.so.0 tools/sdl_shim.cpp -o $SHIM -lpthread -ldl -lm
fi
if [ ! -f port/build/ew2 ] || [ -n "$(find port/build/obj sdl-arm/lib -newer port/build/ew2 -name '*.o' -o -newer port/build/ew2 -name '*.so*' | head -1)" ]; then
    ${CROSS}g++ -mcpu=cortex-a15 -marm -O2 $OBJS -Lsdl-arm/lib -Wl,--allow-shlib-undefined -lpthread -ldl -lm -lrt -lz -lzip -lbsd -l:libSDL2-2.0.so.0 -o port/build/ew2
fi
echo "built port/build/ew2"
