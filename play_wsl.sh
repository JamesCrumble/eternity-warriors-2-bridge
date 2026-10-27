#!/bin/bash
# Plays Eternity Warriors 2 entirely inside WSL (WSLg window), the .sh counterpart of play_win.cmd.
# qemu-arm (user mode) runs the ARM port and the GL stream is replayed on the host:
#   EW2_HOST=gpu   (default) host/build-win/ew2gl.exe, ANGLE -> Direct3D 11 on the real GPU (WSLg's Mesa is software only)
#   EW2_HOST=linux build/ew2gl, native SDL2 + Mesa (llvmpipe)
# Sound: guest FMOD -> fifo -> build/audio_sink (SDL2 -> WSLg PulseAudio).
# EW2_PROFILE=1 enables the guest profilers (logs/play.log), then: python3 tools/log_digest.py
# Usage: ./play_wsl.sh [WIDTHxHEIGHT]   Logs: logs/play.log (guest), logs/ew2gl.log, logs/http.log, logs/audio.log
P=$(cd "$(dirname "$0")" && pwd)
cd "$P"
mkdir -p logs ctl Saves build
# Every start overwrites the logs, so the previous session is first moved to logs/history/<time>/ (the last 20 are kept).
if [ -s logs/play.log ] || [ -s logs/ew2gl.log ]; then
    H=logs/history/$(date +%Y%m%d-%H%M%S)
    mkdir -p "$H" && mv logs/play.log logs/ew2gl.log logs/audio.log logs/http.log "$H"/ 2>/dev/null
    ls -dt logs/history/*/ 2>/dev/null | tail -n +21 | xargs -r rm -rf
fi
RES=${1:-1280x720}
PORT=${EW2_GL_PORT:-18090}

[ -f ew2data/base.apk ] || python3 tools/setup_ew2data.py || exit 1
# Unpacks the LZMA bundles of the OBB once (and pre-caches them in UnityCache): decompressing them in the
# emulated guest at every level load was a multi-second stall.
python3 -c "import sys; sys.path.insert(0, '.'); from tools.optimize_obb import optimize_obb; optimize_obb()" || exit 1
tools/build_port_wsl.sh || exit 1
if [ ! -x build/ew2gl ] || [ -n "$(find host/glbridge -newer build/ew2gl \( -name '*.c' -o -name '*.inc' \))" ]; then
    mkdir -p build/sdl2-x64/include/SDL2
    sed 's/#define SIZEOF_VOIDP 4/#define SIZEOF_VOIDP 8/' /usr/include/arm-linux-gnueabihf/SDL2/_real_SDL_config.h \
        > build/sdl2-x64/include/SDL2/_real_SDL_config.h
    L=/usr/lib/x86_64-linux-gnu
    cc -std=c11 -D_DEFAULT_SOURCE -D_REENTRANT -O2 -Wall -Ibuild/sdl2-x64/include -I/usr/include/SDL2 \
        -Ithird_party/khronos/include -o build/ew2gl host/glbridge/ew3gl.c \
        $L/libSDL2-2.0.so.0 $L/libGLESv2.so.2 $L/libEGL.so.1 -lm -lpthread || exit 1
fi

if [ ! -x build/audio_sink ] || [ tools/audio_sink.c -nt build/audio_sink ]; then
    cc -O2 -Ibuild/sdl2-x64/include -I/usr/include/SDL2 -o build/audio_sink tools/audio_sink.c \
        /usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0 || exit 1
fi
HOST=${EW2_HOST:-gpu}
if [ "$HOST" = gpu ]; then
    if [ ! -f host/build-win/ew2gl.exe ] || [ -n "$(find host/glbridge -newer host/build-win/ew2gl.exe \( -name '*.c' -o -name '*.inc' \))" ]; then
        tools/build_host_win.sh || { echo "GPU host build failed, using the Linux host"; HOST=linux; }
    fi
fi

# Saves/ is the single source of truth: ew2data/files points at it.
if [ -f Saves/savegame.bin ]; then :; elif [ -f ew2data/files/savegame.bin ] && [ ! -L ew2data/files ]; then cp ew2data/files/savegame.bin Saves/; fi
if [ ! -L ew2data/files ]; then rm -rf ew2data/files; ln -sfn "$P/Saves" ew2data/files; fi

pkill -f tools/http_stub.py 2>/dev/null
python3 tools/http_stub.py >> logs/http.log 2>&1 &
HTTP=$!
rm -f ctl/commands; : > ctl/commands
AUDIO_FIFO=/tmp/ew2_audio.fifo
rm -f $AUDIO_FIFO; mkfifo $AUDIO_FIFO
export PULSE_SERVER=${PULSE_SERVER:-unix:/mnt/wslg/PulseServer}
PULSE_LATENCY_MSEC=${EW2_AUDIO_LATENCY_MS:-40} SDL_AUDIODRIVER=pulseaudio build/audio_sink $AUDIO_FIFO > logs/audio.log 2>&1 &
SINK=$!
# Whatever ends the script (window closed, Ctrl+C, kill) takes the helpers and the game with it.
trap 'kill $HOSTPID $GUEST $HTTP $SINK 2>/dev/null; rm -f $AUDIO_FIFO' EXIT
trap 'exit 130' INT TERM HUP

(
    export QEMU_LD_PREFIX=/usr/arm-linux-gnueabihf LD_LIBRARY_PATH="$P/sdl-arm/lib:/usr/lib/arm-linux-gnueabihf"
    export TZ=UTC0 LOADER_TRACE=${LOADER_TRACE:-0} SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy EW2_AUDIO_DEV=$AUDIO_FIFO
    export EW2_NO_CNTVCT=1 EW2_GL_BRIDGE=1 EW2_GL_LISTEN=$PORT EW2_GL_INFLIGHT=4 EW2_SERVER=127.0.0.1 EW2_ALLOW_UNRESOLVED=1
    export EW2_CONTROL_DIR="$P/ctl" EW2_WIDTH=${RES%x*} EW2_HEIGHT=${RES#*x} EW2_TARGET_FPS=${EW2_TARGET_FPS:-120}
    export EW2_GL_RENDERER="${EW2_GL_RENDERER:-Adreno (TM) 420}"
    if [ "${EW2_PROFILE:-0}" = 1 ]; then   # per-frame log: tools/log_digest.py, tools/timeline.py, tools/guest_prof.py
        export EW2_GL_PROFILE=1 EW2_PC_PROF=1 EW2_SYSCALL_PROF=1 EW2_ICALL_PROF=1 LOADER_TRACE=1
    fi
    # EW2_QEMU=path overrides; otherwise a locally built newer qemu-arm (tools/build_qemu.sh), else the system one.
    QEMU=${EW2_QEMU:-$P/third_party/qemu/qemu-arm}
    [ -x "$QEMU" ] || QEMU=qemu-arm
    exec "$QEMU" port/build/ew2 ew2data
) > logs/play.log 2>&1 &
GUEST=$!

export EW2_GL_CONNECT=127.0.0.1:$PORT EW2_WIDTH=${RES%x*} EW2_HEIGHT=${RES#*x} EW2_CONTROL_DIR="$P/ctl"
mkdir -p ew2data/shader_cache; export EW2_SHADER_CACHE="$P/ew2data/shader_cache"
export EW2_GL_LOG="$P/logs/ew2gl.log" EW2_GL_VSYNC=${EW2_GL_VSYNC:-0} EW2_GL_REPORT=${EW2_GL_REPORT:-2}
: > logs/ew2gl.log
if [ "$HOST" = gpu ]; then
    # A Windows process: WSLENV passes the variables, /p turns paths into Windows ones.
    chmod +x host/build-win/ew2gl.exe
    export WSLENV=EW2_GL_CONNECT:EW2_WIDTH:EW2_HEIGHT:EW2_CONTROL_DIR/p:EW2_GL_LOG/p:EW2_SHADER_CACHE/p:EW2_GL_VSYNC:EW2_GL_REPORT:EW2_GL_CHECK:EW2_GL_CHECK_EVERY
    host/build-win/ew2gl.exe &
else
    build/ew2gl &
fi
HOSTPID=$!
wait $HOSTPID
for _ in $(seq 25); do kill -0 $GUEST 2>/dev/null || break; sleep 0.2; done
kill $GUEST $HTTP $SINK 2>/dev/null
rm -f $AUDIO_FIFO
wait 2>/dev/null
