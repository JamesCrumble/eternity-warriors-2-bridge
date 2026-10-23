#!/bin/bash
# Plays the game with native GPU rendering on Windows: the port runs under qemu-arm in WSL with the GL bridge
# listening on TCP (EW3_GL_LISTEN), and host/build-win/ew3gl.exe (ANGLE -> Direct3D 11) connects to it through
# WSL's localhost forwarding, replays the GL stream on the GPU, shows the window and sends input to ctl/commands.
# Sound: the guest's SDL -> WSLg's PulseAudio. Starts the stand-in server if it is not running.
# Usage: tools/play_win.sh [WIDTHxHEIGHT]   Logs: logs/play.log (guest), logs/ew3gl.log, logs/server.log
P=$(cd "$(dirname "$0")/.." && pwd)
cd "$P"
mkdir -p logs ctl
RES=${1:-1280x720}
PORT=${EW3_GL_PORT:-18090}
EXE=host/build-win/ew3gl.exe
if [ ! -f "$EXE" ] || [ -n "$(find host/glbridge -newer "$EXE" \( -name '*.c' -o -name '*.inc' \))" ]; then
    host/build.sh ew3gl || exit 1
fi
chmod +x "$EXE"   # built by Windows on the WSL disk: no exec bit
if ! (exec 3<>/dev/tcp/127.0.0.1/18080) 2>/dev/null; then
    server/.venv/bin/python server/ew3server.py >> logs/server.log 2>&1 &
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/18080) 2>/dev/null && break; sleep 0.2; done
fi
rm -f ctl/commands
: > ctl/commands

export EW3_WIDTH=${RES%x*} EW3_HEIGHT=${RES#*x} EW3_CONTROL_DIR="$P/ctl"
# Uncapped by default: the engine's limiter at EW3_TARGET_FPS, no vsync in the window (EW3_GL_VSYNC=1 to cap
# at the monitor's rate). F2 in the window toggles the frame-rate counter.
export EW3_TARGET_FPS=${EW3_TARGET_FPS:-240} EW3_GL_VSYNC=${EW3_GL_VSYNC:-0}

# The GPU the game is told it runs on. It picks its quality tier from the name alone
# (AndroidQualitySettings.DetectQualitySetting); ANGLE's real name is unknown to it and
# gets the lowest tier. EW3_GPU=adreno (default) | mali | powervr | tegra | real.
case "${EW3_GPU:-adreno}" in
    adreno)  GPU_NAME="Adreno (TM) 420" ;;      # Tier_5 with 4+ cores
    mali)    GPU_NAME="Mali-T760" ;;            # Tier_5 with 8+ cores
    powervr) GPU_NAME="PowerVR Rogue G6430" ;;  # Tier_2..3
    tegra)   GPU_NAME="NVIDIA Tegra" ;;
    real)    GPU_NAME="" ;;
    *) echo "EW3_GPU must be adreno, mali, powervr, tegra or real" >&2; exit 2 ;;
esac
[ -n "$GPU_NAME" ] && export EW3_GL_RENDERER="$GPU_NAME"

# The guest: GL goes to whoever connects to 127.0.0.1:$PORT.
(
    export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-pulseaudio}
    export EW3_GL_BRIDGE=1 EW3_GL_LISTEN=$PORT EW3_SERVER=127.0.0.1 EW3_ALLOW_UNRESOLVED=1 LOADER_TRACE=1
    export QEMU_LD_PREFIX=/usr/arm-linux-gnueabihf
    export LD_LIBRARY_PATH="$P/sdl-arm/lib:/usr/lib/arm-linux-gnueabihf"
    exec qemu-arm port/build/ew3 ew3data
) > logs/play.log 2>&1 &
GUEST=$!

# The host on Windows; WSLENV hands it the variables, /p turns paths into Windows ones.
export EW3_GL_CONNECT=127.0.0.1:$PORT EW3_GL_LOG="$P/logs/ew3gl.log"
export WSLENV=EW3_GL_CONNECT:EW3_WIDTH:EW3_HEIGHT:EW3_CONTROL_DIR/p:EW3_GL_LOG/p:EW3_GL_VSYNC:EW3_GL_REPORT:EW3_GL_CHECK:EW3_GL_ES:EW3_GL_HUD
: > logs/ew3gl.log
"$EXE"
# Window closed: ew3gl.exe already asked the game to quit; give it a moment, then make sure.
for _ in $(seq 25); do kill -0 $GUEST 2>/dev/null || break; sleep 0.2; done
kill $GUEST 2>/dev/null
wait $GUEST 2>/dev/null
