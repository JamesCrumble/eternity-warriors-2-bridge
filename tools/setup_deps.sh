#!/bin/bash
# Fetches what the build needs that is not in the repository (everything stays inside the project, nothing is
# installed system-wide). Safe to re-run: what is already there is left alone.
#
#   third_party/khronos/include   EGL/GLES headers - copied from /usr/include (packages libegl1-mesa-dev,
#                                 libgles2-mesa-dev), or from the Khronos registries, https://registry.khronos.org/
#   third_party/sdl2-win          SDL2 for Windows x64 (headers + SDL2.dll) for the GPU host ew2gl.exe;
#                                 from SDL2-devel-<ver>-VC.zip, https://github.com/libsdl-org/SDL/releases
#   third_party/angle/bin         libEGL.dll + libGLESv2.dll (ANGLE: OpenGL ES on Direct3D 11); ANGLE has no
#                                 official binaries, take the two DLLs from an application that ships them
#
# Usage: tools/setup_deps.sh [--angle DIR_WITH_DLLS] [--sdl2 SDL2-devel-2.30.9-VC.zip]
#   --angle  default: looks in OBS Studio, Google Chrome and Microsoft Edge under /mnt/c/Program Files*
#   --sdl2   default: downloads SDL2-devel-2.30.9-VC.zip into build/downloads (curl needed)
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
cd "$P"
ANGLE_DIR=""; SDL_ZIP=""
while [ $# -gt 0 ]; do
    case $1 in
        --angle) ANGLE_DIR=$2; shift 2 ;;
        --sdl2) SDL_ZIP=$2; shift 2 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done
fail=0

# --- Khronos headers
if [ -f third_party/khronos/include/EGL/egl.h ] && [ -f third_party/khronos/include/GLES2/gl2.h ]; then
    echo "[deps] khronos headers: present"
elif [ -f /usr/include/EGL/egl.h ] && [ -f /usr/include/GLES2/gl2.h ]; then
    mkdir -p third_party/khronos/include
    for d in EGL GLES2 GLES3 KHR; do [ -d /usr/include/$d ] && cp -r /usr/include/$d third_party/khronos/include/; done
    echo "[deps] khronos headers: copied from /usr/include"
else
    echo "[deps] khronos headers: MISSING - sudo apt install libegl1-mesa-dev libgles2-mesa-dev, then re-run"
    fail=1
fi

# --- SDL2 for Windows
if [ -f third_party/sdl2-win/include/SDL2/SDL.h ] && [ -f third_party/sdl2-win/lib/SDL2.dll ]; then
    echo "[deps] SDL2 (Windows): present"
else
    if [ -z "$SDL_ZIP" ]; then
        SDL_ZIP=build/downloads/SDL2-devel-2.30.9-VC.zip
        if [ ! -f "$SDL_ZIP" ]; then
            mkdir -p build/downloads
            curl -fL -o "$SDL_ZIP" https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-VC.zip \
                || { echo "[deps] SDL2: download failed; fetch the zip by hand and pass --sdl2"; rm -f "$SDL_ZIP"; SDL_ZIP=""; }
        fi
    fi
    if [ -n "$SDL_ZIP" ] && [ -f "$SDL_ZIP" ]; then
        tmp=$(mktemp -d "$P/build/sdl2.XXXXXX")
        python3 -I - "$SDL_ZIP" "$tmp" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for n in z.namelist():
    if ".." in n.split("/") or n.startswith("/"):
        sys.exit("unsafe path in archive: " + n)
z.extractall(sys.argv[2])
PY
        root=$(ls -d "$tmp"/SDL2-*/ | head -1)
        mkdir -p third_party/sdl2-win/lib
        cp -r "$root/include" third_party/sdl2-win/
        cp "$root/lib/x64/SDL2.dll" third_party/sdl2-win/lib/
        rm -rf "$tmp"
        echo "[deps] SDL2 (Windows): installed from $SDL_ZIP"
    else
        fail=1
    fi
fi

# --- ANGLE
if [ -f third_party/angle/bin/libEGL.dll ] && [ -f third_party/angle/bin/libGLESv2.dll ]; then
    echo "[deps] ANGLE: present"
else
    if [ -z "$ANGLE_DIR" ]; then
        for d in "/mnt/c/Program Files/obs-studio/obs-plugins/64bit" \
                 /mnt/c/Program\ Files/Google/Chrome/Application/*/ \
                 "/mnt/c/Program Files (x86)/Microsoft/Edge/Application"/*/ ; do
            if [ -f "$d/libEGL.dll" ] && [ -f "$d/libGLESv2.dll" ]; then ANGLE_DIR=$d; break; fi
        done
    fi
    if [ -n "$ANGLE_DIR" ] && [ -f "$ANGLE_DIR/libEGL.dll" ] && [ -f "$ANGLE_DIR/libGLESv2.dll" ]; then
        mkdir -p third_party/angle/bin
        cp "$ANGLE_DIR/libEGL.dll" "$ANGLE_DIR/libGLESv2.dll" third_party/angle/bin/
        # Chromium's ANGLE loads the D3D compiler from its own folder or from System32 (d3dcompiler_47.dll).
        [ -f "$ANGLE_DIR/d3dcompiler_47.dll" ] && cp "$ANGLE_DIR/d3dcompiler_47.dll" third_party/angle/bin/
        chmod u+w third_party/angle/bin/*.dll
        echo "[deps] ANGLE: copied from $ANGLE_DIR"
    else
        echo "[deps] ANGLE: MISSING - pass --angle <folder with libEGL.dll and libGLESv2.dll> (OBS Studio, Chrome or Edge have them)"
        fail=1
    fi
fi
[ $fail = 0 ] && echo "[deps] done: ./play_wsl.sh builds ew2gl.exe on first start" || { echo "[deps] some dependencies are missing; EW2_HOST=linux ./play_wsl.sh works without the GPU ones"; exit 1; }
