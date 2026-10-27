#!/bin/bash
# Kept for the name: the window drawn by WSLg on Mesa (software GL) is play_wsl.sh with EW2_HOST=linux.
# Usage: tools/play_wslg.sh [WIDTHxHEIGHT]
EW2_HOST=linux exec "$(cd "$(dirname "$0")/.." && pwd)/play_wsl.sh" "$@"
