#!/bin/bash
# Kept for the name: the GPU host (ew2gl.exe, ANGLE -> Direct3D 11) is what play_wsl.sh uses by default.
# Usage: tools/play_gpu.sh [WIDTHxHEIGHT]
EW2_HOST=gpu exec "$(cd "$(dirname "$0")/.." && pwd)/play_wsl.sh" "$@"
