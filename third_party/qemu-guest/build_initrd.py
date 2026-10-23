import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.chdir(ROOT)

ino = 1


def cpio_entry(f_out, name, data=b"", mode=0o100755):
    global ino
    name_b = name.encode("utf-8") + b"\x00"
    nlink = 2 if (mode & 0o170000) == 0o040000 else 1
    size = 0 if (mode & 0o170000) == 0o040000 else len(data)
    hdr = (
        f"070701"
        f"{ino:08x}{mode:08x}{0:08x}{0:08x}{nlink:08x}{0:08x}"
        f"{size:08x}{0:08x}{0:08x}{0:08x}{0:08x}{len(name_b):08x}{0:08x}"
    ).encode("ascii")
    ino += 1
    f_out.write(hdr)
    f_out.write(name_b)
    rem = (len(hdr) + len(name_b)) % 4
    if rem:
        f_out.write(b"\x00" * (4 - rem))
    if size:
        f_out.write(data)
        rem = size % 4
        if rem:
            f_out.write(b"\x00" * (4 - rem))


def main():
    subprocess.run(
        [
            r"third_party\zig\zig.exe",
            "cc",
            "-target",
            "arm-linux-musleabihf",
            "-static",
            "-O3",
            "third_party/qemu-guest/runner.c",
            "-o",
            "third_party/qemu-guest/init",
            "-lpthread",
        ],
        check=True,
    )

    out_path = "third_party/qemu-guest/ew2_initrd.cpio"
    dirs_added = set()

    def ensure_dir(f_out, d):
        d = d.strip("/")
        if not d or d in dirs_added:
            return
        parent = os.path.dirname(d)
        if parent:
            ensure_dir(f_out, parent)
        cpio_entry(f_out, d, b"", 0o040755)
        dirs_added.add(d)

    with open(out_path, "wb") as f_out:
        cpio_entry(f_out, "init", open("third_party/qemu-guest/init", "rb").read(), 0o100755)

        for d in ["lib", "lib/arm-linux-gnueabihf", "app", "ew2data", "ew2data/files"]:
            ensure_dir(f_out, d)

        ld_data = open("third_party/sysroot-armhf/lib/arm-linux-gnueabihf/ld-2.31.so", "rb").read()
        cpio_entry(f_out, "lib/ld-linux-armhf.so.3", ld_data, 0o100755)

        for src_dir in ["lib/arm-linux-gnueabihf", "usr/lib/arm-linux-gnueabihf"]:
            full_dir = os.path.join("third_party/sysroot-armhf", src_dir)
            for fn in sorted(os.listdir(full_dir)):
                fp = os.path.join(full_dir, fn)
                if os.path.isfile(fp) and ".so" in fn:
                    dst = f"lib/arm-linux-gnueabihf/{fn}"
                    cpio_entry(f_out, dst, open(fp, "rb").read(), 0o100755)

        cpio_entry(
            f_out,
            "lib/arm-linux-gnueabihf/libSDL2-2.0.so.0",
            open("sdl-arm/lib/libSDL2-2.0.so.0", "rb").read(),
            0o100755,
        )
        cpio_entry(f_out, "app/ew2", open("port/build/ew2", "rb").read(), 0o100755)

        for root, dirs, files in os.walk("ew2data"):
            rel_root = os.path.relpath(root, ".").replace("\\", "/")
            ensure_dir(f_out, rel_root)
            for d in dirs:
                ensure_dir(f_out, f"{rel_root}/{d}")
            for fn in files:
                fp = os.path.join(root, fn)
                rel_fp = f"{rel_root}/{fn}"
                mode = 0o100755 if fn.endswith(".so") else 0o100644
                cpio_entry(f_out, rel_fp, open(fp, "rb").read(), mode)

        cpio_entry(f_out, "TRAILER!!!", b"", 0)

    print("Created", out_path, os.path.getsize(out_path), "bytes")


if __name__ == "__main__":
    main()
