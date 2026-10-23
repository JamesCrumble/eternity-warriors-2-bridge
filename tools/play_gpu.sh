#!/bin/bash
# Plays the game in a window on the Windows desktop, drawn by the GPU.
# qemu-arm runs the port with the GL bridge on (EW3_GL_BRIDGE=1): every GL call goes down a fifo to
# build/ew3gl (native x86_64), which replays it on WSLg's GLES (Mesa d3d12 -> the Windows GPU), shows the
# frames and sends the mouse back as touches. Sound: the guest's SDL -> WSLg's PulseAudio, as before.
# Starts the stand-in server if it is not running.
# Usage: tools/play_gpu.sh [WIDTHxHEIGHT]   Logs: logs/play.log (guest), logs/ew3gl.log, logs/server.log
P=$(cd "$(dirname "$0")/.." && pwd)
cd "$P"
mkdir -p logs ctl build
RES=${1:-1280x720}
if [ ! -x build/ew3gl ] || [ -n "$(find host/glbridge -newer build/ew3gl -name '*.[ch]*' -o -newer build/ew3gl -name '*.inc')" ]; then
    host/glbridge/build.sh || exit 1
fi
if ! (exec 3<>/dev/tcp/127.0.0.1/18080) 2>/dev/null; then
    server/.venv/bin/python server/ew3server.py >> logs/server.log 2>&1 &
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/18080) 2>/dev/null && break; sleep 0.2; done
fi
FIFO_DIR=/tmp/ew3gl.$$
mkdir -p "$FIFO_DIR"
rm -f ctl/commands
: > ctl/commands
export EW3_WIDTH=${RES%x*} EW3_HEIGHT=${RES#*x} EW3_CONTROL_DIR=ctl
export EW3_GL_CMD=$FIFO_DIR/cmd EW3_GL_REPLY=$FIFO_DIR/reply
mkfifo "$EW3_GL_CMD" "$EW3_GL_REPLY"

# The host first: it owns the context the guest's first glGetString is answered from.
EW3_GL_LOG=logs/ew3gl.log build/ew3gl &
GL=$!

export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-pulseaudio}
export EW3_GL_BRIDGE=1 EW3_SERVER=127.0.0.1 EW3_ALLOW_UNRESOLVED=1
export QEMU_LD_PREFIX=/usr/arm-linux-gnueabihf
export LD_LIBRARY_PATH="$P/sdl-arm/lib:/usr/lib/arm-linux-gnueabihf"
qemu-arm port/build/ew3 ew3data > logs/play.log 2>&1
kill $GL 2>/dev/null
wait $GL 2>/dev/null
rm -rf "$FIFO_DIR"
