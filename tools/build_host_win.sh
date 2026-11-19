#!/bin/bash
# Cross-builds host/build-win/ew2gl.exe (GL bridge on ANGLE -> Direct3D 11, i.e. the real GPU) from WSL with mingw-w64.
# Needs sdl2-win (headers + SDL2.dll) and angle/bin (libGLESv2.dll, libEGL.dll): third_party/<name> of the project, else ../__shared-third-party__/<name>.
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
cd "$P"
tp() { [ -e "$P/${2:-$1}" ] && echo "$P/${2:-$1}" || echo "${SHARED_3P:-$P/../__shared-third-party__}/$1"; }  # third-party: this project first, then ../__shared-third-party__
KH=$(tp khronos third_party/khronos); SDLW=$(tp sdl2-win third_party/sdl2-win); ANG=$(tp angle third_party/angle)
mkdir -p host/build-win
cp -u $SDLW/lib/SDL2.dll $ANG/bin/*.dll host/build-win/
x86_64-w64-mingw32-gcc -std=gnu11 -O2 -DSDL_MAIN_HANDLED -D_WIN32_WINNT=0x0601 \
    -Ihost/glbridge -I$KH/include -I$SDLW/include -I$SDLW/include/SDL2 \
    -o host/build-win/ew2gl.exe host/glbridge/ew3gl.c \
    $SDLW/lib/SDL2.dll $ANG/bin/libGLESv2.dll $ANG/bin/libEGL.dll -lws2_32
echo "built host/build-win/ew2gl.exe"
