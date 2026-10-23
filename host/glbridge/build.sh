#!/bin/bash
# Builds build/ew3gl, the GL bridge host, as a native x86_64 program for WSL (WSLg draws on the GPU).
# Only libsdl2-dev:armhf is installed here: its headers are borrowed with an x86_64 config and the amd64
# runtime libraries are linked directly (nothing installed system-wide), as tools/play_wslg.sh does for ew3view.
set -eu
P=$(cd "$(dirname "$0")/../.." && pwd)
cd "$P"
mkdir -p build/sdl2-x64/include/SDL2
if [ ! -f build/sdl2-x64/include/SDL2/_real_SDL_config.h ]; then
    sed 's/#define SIZEOF_VOIDP 4/#define SIZEOF_VOIDP 8/' /usr/include/arm-linux-gnueabihf/SDL2/_real_SDL_config.h \
        > build/sdl2-x64/include/SDL2/_real_SDL_config.h
fi
L=/usr/lib/x86_64-linux-gnu
cc -std=c11 -D_DEFAULT_SOURCE -D_REENTRANT -O2 -g -Wall -Wextra \
    -Ibuild/sdl2-x64/include -I/usr/include/SDL2 \
    -o build/ew3gl host/glbridge/ew3gl.c \
    $L/libSDL2-2.0.so.0 $L/libGLESv2.so.2 $L/libEGL.so.1
echo "built build/ew3gl"
