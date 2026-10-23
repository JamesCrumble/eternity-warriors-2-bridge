#!/bin/bash
# Runs the port on the PC under the host's qemu-arm with Mesa's software GLES (slow; for bring-up only).
# Usage: tools/run_local.sh [seconds]   Log: logs/local.log
P=$(cd "$(dirname "$0")/.." && pwd)
export SDL_VIDEODRIVER=offscreen LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe EGL_PLATFORM=surfaceless
export EW3_CONTROL_DIR=${EW3_CONTROL_DIR:-ctl} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
export LOADER_TRACE=1 EW3_SHOT_EVERY=${EW3_SHOT_EVERY:-100} EW3_WIDTH=${EW3_WIDTH:-854} EW3_HEIGHT=${EW3_HEIGHT:-480} EW3_ALLOW_UNRESOLVED=${EW3_ALLOW_UNRESOLVED:-1}
export QEMU_LD_PREFIX=/usr/arm-linux-gnueabihf
export LD_LIBRARY_PATH="$P/sdl-arm/lib:/usr/lib/arm-linux-gnueabihf"
cd "$P"
rm -f ctl/shot_*.bmp
timeout "${1:-60}" qemu-arm port/build/ew3 ew3data > logs/local.log 2>&1
echo "exit=$?"
