#!/usr/bin/env python3
"""
Extracts and prepares ew2data/ and local Saves/ for Eternity Warriors 2 Bridge:
- ew2data/base.apk
- ew2data/Android/obb/com.glu.ewarriors2/main.431.com.glu.ewarriors2.obb
- ew2data/lib/armeabi-v7a/*.so (from APK + real libunity.so & libmono.so from OBB)
- ew2data/glu_properties.txt (AES-128-CBC decrypted from APK res/raw/properties.dat)
- Saves/savegame.bin and Optional_Cheat_Save/savegame.bin
"""
import os
import shutil
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARENT = os.path.dirname(ROOT)

APK_SRC = os.path.join(PARENT, "com.glu.ewarriors2-435.apk")
OBB_SRC = os.path.join(PARENT, "cache_extracted", "com.glu.ewarriors2", "main.431.com.glu.ewarriors2.obb")
PC_SAVES_SRC = os.path.join(PARENT, "Eternity_Warriors_2_PC", "Saves")
PC_CHEAT_SAVES_SRC = os.path.join(PARENT, "Eternity_Warriors_2_PC", "Optional_Cheat_Save")

EW2DATA = os.path.join(ROOT, "ew2data")
OBB_DIR = os.path.join(EW2DATA, "Android", "obb", "com.glu.ewarriors2")
LIB_DIR = os.path.join(EW2DATA, "lib", "armeabi-v7a")
FILES_DIR = os.path.join(EW2DATA, "files")
SHARED_PREFS_DIR = os.path.join(EW2DATA, "shared_prefs")
CACHE_DIR = os.path.join(EW2DATA, "cache")
SAVES_DIR = os.path.join(ROOT, "Saves")
CHEAT_SAVES_DIR = os.path.join(ROOT, "Optional_Cheat_Save")


def decrypt_aes_cbc_pkcs5(ciphertext: bytes, key: bytes, iv: bytes) -> bytes:
    """Pure-Python / OpenSSL / Crypto fallback AES-128-CBC decryption."""
    try:
        from Crypto.Cipher import AES
        data = AES.new(key, AES.MODE_CBC, iv).decrypt(ciphertext)
        pad = data[-1]
        return data[:-pad] if 1 <= pad <= 16 else data
    except ImportError:
        pass
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        decryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
        data = decryptor.update(ciphertext) + decryptor.finalize()
        pad = data[-1]
        return data[:-pad] if 1 <= pad <= 16 else data
    except ImportError:
        pass
    # Pure Python AES-128 implementation (no external dependencies needed)
    sbox = [
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
        0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
        0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
        0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
        0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
        0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
        0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
        0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
        0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
        0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
        0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
        0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
        0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
        0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
    ]
    isbox = [0] * 256
    for idx, val in enumerate(sbox):
        isbox[val] = idx

    def gmul(a, b):
        p = 0
        for _ in range(8):
            if b & 1:
                p ^= a
            hi = a & 0x80
            a = (a << 1) & 0xff
            if hi:
                a ^= 0x1b
            b >>= 1
        return p

    rcon = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36]
    w = [list(key[i:i+4]) for i in range(0, 16, 4)]
    for i in range(4, 44):
        temp = list(w[i - 1])
        if i % 4 == 0:
            temp = [sbox[temp[1]] ^ rcon[i // 4 - 1], sbox[temp[2]], sbox[temp[3]], sbox[temp[0]]]
        w.append([w[i - 4][j] ^ temp[j] for j in range(4)])

    out = bytearray()
    prev = list(iv)
    for blk in range(0, len(ciphertext), 16):
        cblock = list(ciphertext[blk:blk+16])
        s = [cblock[i] ^ w[40 + i // 4][i % 4] for i in range(16)]
        for r in range(9, 0, -1):
            # InvShiftRows
            s = [
                s[0], s[13], s[10], s[7],
                s[4], s[1], s[14], s[11],
                s[8], s[5], s[2], s[15],
                s[12], s[9], s[6], s[3],
            ]
            # InvSubBytes & AddRoundKey
            s = [isbox[s[i]] ^ w[r * 4 + i // 4][i % 4] for i in range(16)]
            # InvMixColumns
            ns = [0] * 16
            for col in range(4):
                b0, b1, b2, b3 = s[col*4 : col*4+4]
                ns[col*4+0] = gmul(b0, 14) ^ gmul(b1, 11) ^ gmul(b2, 13) ^ gmul(b3, 9)
                ns[col*4+1] = gmul(b0, 9)  ^ gmul(b1, 14) ^ gmul(b2, 11) ^ gmul(b3, 13)
                ns[col*4+2] = gmul(b0, 13) ^ gmul(b1, 9)  ^ gmul(b2, 14) ^ gmul(b3, 11)
                ns[col*4+3] = gmul(b0, 11) ^ gmul(b1, 13) ^ gmul(b2, 9)  ^ gmul(b3, 14)
            s = ns
        s = [
            s[0], s[13], s[10], s[7],
            s[4], s[1], s[14], s[11],
            s[8], s[5], s[2], s[15],
            s[12], s[9], s[6], s[3],
        ]
        s = [isbox[s[i]] ^ w[i // 4][i % 4] ^ prev[i] for i in range(16)]
        out.extend(s)
        prev = cblock
    pad = out[-1]
    return bytes(out[:-pad] if 1 <= pad <= 16 else out)


def main():
    for d in (EW2DATA, OBB_DIR, LIB_DIR, FILES_DIR, SHARED_PREFS_DIR, CACHE_DIR, SAVES_DIR, CHEAT_SAVES_DIR):
        os.makedirs(d, exist_ok=True)

    apk_dst = os.path.join(EW2DATA, "base.apk")
    if not os.path.exists(apk_dst):
        print(f"Copying {APK_SRC} -> {apk_dst}")
        shutil.copy2(APK_SRC, apk_dst)

    obb_dst = os.path.join(OBB_DIR, "main.431.com.glu.ewarriors2.obb")
    if not os.path.exists(obb_dst):
        print(f"Copying {OBB_SRC} -> {obb_dst}")
        shutil.copy2(OBB_SRC, obb_dst)

    # Extract native libs from APK
    with zipfile.ZipFile(apk_dst, "r") as z:
        for name in z.namelist():
            if name.startswith("lib/armeabi-v7a/") and name.endswith(".so"):
                base = os.path.basename(name)
                out_path = os.path.join(LIB_DIR, base)
                with open(out_path, "wb") as f:
                    f.write(z.read(name))
                print(f"APK lib -> {base} ({os.path.getsize(out_path)} bytes)")
        # Decrypt res/raw/properties.dat
        prop_raw = z.read("res/raw/properties.dat")
        key = bytes.fromhex("a9cc36b80e9e5c25947d80d676a3f638")
        iv = bytes.fromhex("fb3104bfc8db4ddbbbce2c38e780d67e")
        prop_txt = decrypt_aes_cbc_pkcs5(prop_raw, key, iv)
        prop_path = os.path.join(EW2DATA, "glu_properties.txt")
        with open(prop_path, "wb") as f:
            f.write(prop_txt)
        print(f"Decrypted glu_properties.txt ({len(prop_txt)} bytes)")

    # Extract real libunity.so and libmono.so from OBB (overwriting the tiny installer stubs from APK)
    with zipfile.ZipFile(obb_dst, "r") as z:
        for name in z.namelist():
            if name.startswith("assets/libs/armeabi-v7a/") and name.endswith(".so"):
                base = os.path.basename(name)
                out_path = os.path.join(LIB_DIR, base)
                with open(out_path, "wb") as f:
                    f.write(z.read(name))
                print(f"OBB lib -> {base} ({os.path.getsize(out_path)} bytes)")

    # Copy local saves
    for src_dir, dst_dir in ((PC_SAVES_SRC, SAVES_DIR), (PC_CHEAT_SAVES_SRC, CHEAT_SAVES_DIR)):
        if os.path.isdir(src_dir):
            for fname in os.listdir(src_dir):
                s = os.path.join(src_dir, fname)
                d = os.path.join(dst_dir, fname)
                if os.path.isfile(s) and not os.path.exists(d):
                    shutil.copy2(s, d)
                    print(f"Copied save: {d}")

    save_bin = os.path.join(SAVES_DIR, "savegame.bin")
    files_save_bin = os.path.join(FILES_DIR, "savegame.bin")
    if os.path.exists(save_bin) and not os.path.exists(files_save_bin):
        shutil.copy2(save_bin, files_save_bin)
        print(f"Seeded {files_save_bin}")

    import sys
    sys.path.insert(0, ROOT)
    import play_win
    play_win.sync_saves_before_launch()

    print("ew2data setup complete!")


if __name__ == "__main__":
    main()
