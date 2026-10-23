#!/bin/bash
# Puts ANGLE (libEGL.dll + libGLESv2.dll, x64) into third_party/angle/bin and writes
# third_party/angle/libGLESv2.def, from which the host build makes an import library
# (ANGLE as shipped inside applications comes without .lib files).
# Usage: tools/angle_setup.sh [directory with the two DLLs]   default: OBS Studio (Chromium build: system DLLs only; Firefox's needs mozglue.dll)
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-"/mnt/c/Program Files/obs-studio/obs-plugins/64bit"}
A="$P/third_party/angle"
mkdir -p "$A/bin"
cp "$SRC/libEGL.dll" "$SRC/libGLESv2.dll" "$A/bin/"
chmod u+w "$A/bin/"*.dll
VSROOT='C:\Program Files\Microsoft Visual Studio\18\Community'
CMD="$A/dumpbin.cmd"
printf '@echo off\r\ncall "%s\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\r\ndumpbin /nologo /exports %%1\r\n' "$VSROOT" > "$CMD"
# Exported names are the last column of the rows "ordinal hint RVA name".
(cd /mnt/c && cmd.exe /c "$(wslpath -w "$CMD")" "$(wslpath -w "$A/bin/libGLESv2.dll")") | tr -d '\r' \
    | awk 'BEGIN { print "LIBRARY libGLESv2"; print "EXPORTS" }
           $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9A-F]+$/ && NF == 4 { print "    " $4 }' > "$A/libGLESv2.def"
rm -f "$CMD"
echo "ANGLE from $SRC: $(($(wc -l < "$A/libGLESv2.def") - 2)) exports in $A/libGLESv2.def"
