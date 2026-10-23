#!/usr/bin/env bash
# Decrypts the build properties Glu's AJTUtil.GetProperty reads (res/raw/properties.dat in the APK,
# AES-128-CBC; key and IV are the two 16-byte arrays in AJTUtil.GetProperty, classes.dex 4.1.0)
# into ew3data/glu_properties.txt, where the port's jni/glu_plugins.cpp reads them.
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
APK="$P/gamebin/E-Warriors-3-v4-1-0.apk"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
unzip -q -o "$APK" classes.dex res/raw/properties.dat -d "$T"
KEY=$(xxd -p -s $((0x301c68)) -l 16 "$T/classes.dex")
IV=$(xxd -p -s $((0x301c80)) -l 16 "$T/classes.dex")
openssl enc -d -aes-128-cbc -K "$KEY" -iv "$IV" -in "$T/res/raw/properties.dat" -out "$P/ew3data/glu_properties.txt"
grep -c = "$P/ew3data/glu_properties.txt"
