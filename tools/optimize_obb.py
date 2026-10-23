import hashlib
import lzma
import os
import struct
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBB_PATH = os.path.join(
    ROOT, "ew2data", "Android", "obb", "com.glu.ewarriors2", "main.431.com.glu.ewarriors2.obb"
)
CACHE_SHARED_DIR = os.path.join(ROOT, "ew2data", "files", "UnityCache", "Shared")


def unityweb_to_unityraw(b: bytes) -> bytes:
    if not b.startswith(b"UnityWeb\x00"):
        return b
    pos = 9
    ver = struct.unpack_from(">I", b, pos)[0]
    pos += 4
    end = b.find(b"\x00", pos)
    uver = b[pos : end + 1]
    pos = end + 1
    end = b.find(b"\x00", pos)
    urev = b[pos : end + 1]
    pos = end + 1
    file_sz, hdr_sz, min_lvls, n_lvls = struct.unpack_from(">IIII", b, pos)
    pos += 16
    lvls = []
    for _ in range(n_lvls):
        comp_end, uncomp_end = struct.unpack_from(">II", b, pos)
        pos += 8
        lvls.append((uncomp_end, uncomp_end))
    cfile_sz, dhdr_sz = struct.unpack_from(">II", b, pos)
    pos += 8
    pad = b[pos:hdr_sz]
    uncomp_total = lvls[-1][1]
    dec = lzma.LZMADecompressor().decompress(b[hdr_sz:], max_length=uncomp_total)
    if len(dec) != uncomp_total:
        raise ValueError(f"Decompressed size mismatch: {len(dec)} != {uncomp_total}")
    new_file_sz = hdr_sz + len(dec)
    out = bytearray()
    out += b"UnityRaw\x00"
    out += struct.pack(">I", ver)
    out += uver
    out += urev
    out += struct.pack(">IIII", new_file_sz, hdr_sz, min_lvls, n_lvls)
    for c_end, u_end in lvls:
        out += struct.pack(">II", c_end, u_end)
    out += struct.pack(">II", new_file_sz, dhdr_sz)
    out += pad
    if len(out) != hdr_sz:
        raise ValueError(f"Header size mismatch: {len(out)} != {hdr_sz}")
    out += dec
    return bytes(out)


def parse_bundle_versions(index_raw: bytes) -> dict[str, int]:
    pos = index_raw.find(b"\x07\x00\x00\x00effects\x00")
    if pos < 0:
        raise ValueError("Could not find 'effects' entry in assets/Bundles/index")

    def r_str(p: int):
        l = struct.unpack_from("<I", index_raw, p)[0]
        s = index_raw[p + 4 : p + 4 + l].decode("latin1")
        p += 4 + ((l + 3) & ~3)
        return s, p

    versions = {}
    for _ in range(11):
        name, pos = r_str(pos)
        fn, pos = r_str(pos)
        hl = struct.unpack_from("<I", index_raw, pos)[0]
        h = index_raw[pos + 4 : pos + 4 + hl]
        pos += 4 + ((hl + 3) & ~3)
        _sz = struct.unpack_from("<I", index_raw, pos)[0]
        pos += 4
        sc_cnt = struct.unpack_from("<I", index_raw, pos)[0]
        pos += 4
        for _ in range(sc_cnt):
            _, pos = r_str(pos)
        as_cnt = struct.unpack_from("<I", index_raw, pos)[0]
        pos += 4
        for _ in range(as_cnt):
            _, pos = r_str(pos)
        _cn, pos = r_str(pos)
        ver = struct.unpack_from("<i", h, 0)[0]
        if ver < 0:
            ver = -ver
        versions[fn] = ver
    return versions


def extract_bundle_to_cache(fn: str, ver: int, raw_bundle: bytes):
    key = f"{fn}@{ver}".encode("utf-8")
    folder_hash = hashlib.sha1(key).hexdigest()
    target_dir = os.path.join(CACHE_SHARED_DIR, folder_hash)
    os.makedirs(target_dir, exist_ok=True)

    pos = 13
    pos = raw_bundle.find(b"\x00", pos) + 1
    pos = raw_bundle.find(b"\x00", pos) + 1
    _file_sz, hdr_sz, _min_lvls, _n_lvls = struct.unpack_from(">IIII", raw_bundle, pos)
    p = hdr_sz
    nfiles = struct.unpack_from(">I", raw_bundle, p)[0]
    p += 4

    entry_names = []
    total_bytes = 0
    for _ in range(nfiles):
        end = raw_bundle.find(b"\x00", p)
        entry_name = raw_bundle[p:end].decode("utf-8")
        off, sz = struct.unpack_from(">II", raw_bundle, end + 1)
        p = end + 9
        entry_names.append(entry_name)
        payload = raw_bundle[hdr_sz + off : hdr_sz + off + sz]
        with open(os.path.join(target_dir, entry_name), "wb") as f:
            f.write(payload)
        total_bytes += len(payload)

    info_lines = ["-1", "1792680234", str(nfiles)] + entry_names
    info_content = ("\n".join(info_lines) + "\n").encode("utf-8")
    with open(os.path.join(target_dir, "__info"), "wb") as f:
        f.write(info_content)

    print(
        f"[optimize_obb] Pre-cached {fn}@{ver} -> UnityCache/Shared/{folder_hash} ({nfiles} files, {total_bytes/1048576:.2f} MB)"
    )


def optimize_obb():
    if not os.path.exists(OBB_PATH):
        print("OBB not found:", OBB_PATH)
        return

    os.makedirs(CACHE_SHARED_DIR, exist_ok=True)

    with zipfile.ZipFile(OBB_PATH, "r") as zin:
        index_data = unityweb_to_unityraw(zin.read("assets/Bundles/index"))
        versions = parse_bundle_versions(index_data)

        has_unextracted_bundles = False
        for info in zin.infolist():
            if info.filename.startswith("assets/Bundles/") and info.filename != "assets/Bundles/index":
                has_unextracted_bundles = True
                break

        if not has_unextracted_bundles:
            print("[optimize_obb] OBB is already optimized and bundles are pre-cached in UnityCache/Shared.")
            return

        tmp_path = OBB_PATH + ".tmp"
        stored_files = 0
        precached_bundles = 0
        with zipfile.ZipFile(tmp_path, "w", compression=zipfile.ZIP_STORED) as zout:
            for info in zin.infolist():
                if info.filename.startswith("assets/libs/"):
                    continue
                data = zin.read(info.filename)
                if info.filename.startswith("assets/Bundles/"):
                    if data.startswith(b"UnityWeb\x00"):
                        data = unityweb_to_unityraw(data)
                    bn = info.filename.split("/")[-1]
                    if bn != "index" and bn in versions:
                        extract_bundle_to_cache(bn, versions[bn], data)
                        precached_bundles += 1
                        continue
                zi = zipfile.ZipInfo(filename=info.filename, date_time=info.date_time)
                zi.compress_type = zipfile.ZIP_STORED
                zi.external_attr = info.external_attr
                zout.writestr(zi, data, compress_type=zipfile.ZIP_STORED)
                stored_files += 1

    os.replace(tmp_path, OBB_PATH)
    print(
        f"[optimize_obb] Done: {stored_files} files stored in OBB ({os.path.getsize(OBB_PATH)/1048576:.2f} MB), {precached_bundles} bundles pre-cached into UnityCache/Shared."
    )


if __name__ == "__main__":
    optimize_obb()
