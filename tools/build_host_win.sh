#!/bin/bash
# Cross-builds host/build-win/ew2gl.exe (GL bridge on ANGLE -> Direct3D 11, i.e. the real GPU) from WSL with mingw-w64.
# Needs third_party/sdl2-win (headers + SDL2.dll) and third_party/angle/bin (libGLESv2.dll, libEGL.dll).
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
cd "$P"
mkdir -p host/build-win
cp -u third_party/sdl2-win/lib/SDL2.dll third_party/angle/bin/*.dll host/build-win/
x86_64-w64-mingw32-gcc -std=gnu11 -O2 -DSDL_MAIN_HANDLED -D_WIN32_WINNT=0x0601 \
    -Ihost/glbridge -Ithird_party/khronos/include -Ithird_party/sdl2-win/include -Ithird_party/sdl2-win/include/SDL2 \
    -o host/build-win/ew2gl.exe host/glbridge/ew3gl.c \
    third_party/sdl2-win/lib/SDL2.dll third_party/angle/bin/libGLESv2.dll third_party/angle/bin/libEGL.dll -lws2_32
echo "built host/build-win/ew2gl.exe"
