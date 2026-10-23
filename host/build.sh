#!/usr/bin/env bash
# Builds host/build-win/ew3host.exe (or the targets given: host/build.sh ew3gl) from WSL by running host/build.cmd on the Windows side.
set -eu
H=$(cd "$(dirname "$0")" && pwd)
cd /mnt/c && cmd.exe /c "$(wslpath -w "$H/build.cmd")" "$@" 2>&1 | tr -d '\r'
