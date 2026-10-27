#!/usr/bin/env bash
# Builds host/build-native/ew2host.exe from WSL by running host/build.cmd on the Windows side (MSVC from Visual Studio).
# Extra CMake arguments: EW2_CMAKE_ARGS="-DDYNARMIC_DIR=... -DBOOST_DIR=..." host/build.sh
set -eu
H=$(cd "$(dirname "$0")" && pwd)
cd /mnt/c && cmd.exe /c "$(wslpath -w "$H/build.cmd")" "$@" 2>&1 | tr -d '\r'
# A Windows build on the WSL disk has no exec bit.
chmod +x "$H"/build-native/*.exe 2>/dev/null || true
