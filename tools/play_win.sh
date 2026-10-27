#!/bin/bash
# Kept for the name: the game is started by play_wsl.sh in the project root (qemu-arm in WSL, GL on the Windows GPU).
# Usage: tools/play_win.sh [WIDTHxHEIGHT]
exec "$(cd "$(dirname "$0")/.." && pwd)/play_wsl.sh" "$@"
