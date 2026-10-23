#!/bin/bash
# Plays the game in a window on the Windows desktop through WSLg (Windows 11).
# qemu-arm runs the port and Mesa renders in software (llvmpipe, offscreen); every frame
# goes to shared memory, and build/ew3view (native x86_64 SDL2) shows it in a window and
# sends the mouse back as touches. Sound: the guest's SDL -> WSLg's PulseAudio.
# Starts the stand-in server if it is not running.
# Usage: tools/play_wslg.sh [WIDTHxHEIGHT]   Logs: logs/play.log, logs/server.log
P=$(cd "$(dirname "$0")/.." && pwd)
cd "$P"
mkdir -p logs ctl build
RES=${1:-1280x720}
if [ ! -x build/ew3view ] || [ tools/ew3view.c -nt build/ew3view ]; then
    # Only libsdl2-dev:armhf is installed here: borrow its headers with an x86_64 config
    # and link the amd64 runtime library directly (nothing installed system-wide).
    mkdir -p build/sdl2-x64/include/SDL2
    sed 's/#define SIZEOF_VOIDP 4/#define SIZEOF_VOIDP 8/' /usr/include/arm-linux-gnueabihf/SDL2/_real_SDL_config.h \
        > build/sdl2-x64/include/SDL2/_real_SDL_config.h
    cc -O2 -Ibuild/sdl2-x64/include -I/usr/include/SDL2 -D_REENTRANT -o build/ew3view tools/ew3view.c \
        /usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0 || exit 1
fi
if ! (exec 3<>/dev/tcp/127.0.0.1/18080) 2>/dev/null; then
    server/.venv/bin/python server/ew3server.py >> logs/server.log 2>&1 &
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/18080) 2>/dev/null && break; sleep 0.2; done
fi
FB=/dev/shm/ew3fb
rm -f "$FB" ctl/commands
: > ctl/commands
build/ew3view "$FB" "$P/ctl" &
VIEW=$!
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-pulseaudio}
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LOADER_TRACE=1
export EW3_SERVER=127.0.0.1 EW3_WIDTH=${RES%x*} EW3_HEIGHT=${RES#*x} EW3_ALLOW_UNRESOLVED=1
export EW3_CONTROL_DIR=ctl EW3_FB_SHM=$FB
export QEMU_LD_PREFIX=/usr/arm-linux-gnueabihf
export LD_LIBRARY_PATH="$P/sdl-arm/lib:/usr/lib/arm-linux-gnueabihf"
qemu-arm port/build/ew3 ew3data > logs/play.log 2>&1
kill $VIEW 2>/dev/null
