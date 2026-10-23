import concurrent.futures
import glob
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

zig = os.path.abspath("third_party/zig/zig.exe")
sysroot = os.path.abspath("third_party/sysroot-armhf")
src_dirs = [
    "src",
    "android",
    "loader",
    "thunks/libc",
    "thunks/khronos",
    "jni",
    "jni/classes",
    "third_party/powervr",
]
srcs = []
for d in src_dirs:
    srcs.extend(glob.glob(f"port/{d}/*.cpp"))

os.makedirs("port/build/obj", exist_ok=True)
includes = [
    "-Iport",
    "-Iport/src",
    "-Iport/android",
    "-Iport/loader",
    "-Iport/thunks",
    "-Iport/thunks/libc",
    "-Iport/thunks/libc/generated",
    "-Iport/jni",
    "-Ithird_party/khronos/include",
    f"-I{sysroot}/usr/include",
    f"-I{sysroot}/usr/include/arm-linux-gnueabihf",
    f"-I{sysroot}/usr/include/SDL2",
]
flags = [
    "c++",
    "-target",
    "arm-linux-gnueabihf.2.31",
    "-mcpu=cortex_a15",
    "-std=gnu++20",
    "-O3",
    "-fno-strict-aliasing",
    "-fuse-cxa-atexit",
    "-Wno-unused-parameter",
    "-Wno-c++11-narrowing",
    '-DPORT_ENV_PREFIX="EW2"',
    "-D_REENTRANT",
] + includes


def needs_recompile(src, obj, dep_file):
    if not os.path.exists(obj):
        return True
    obj_mtime = os.path.getmtime(obj)
    if obj_mtime < os.path.getmtime(src):
        return True
    if not os.path.exists(dep_file):
        return False
    try:
        content = open(dep_file, "r", encoding="utf-8", errors="ignore").read()
        content = content.replace("\\\n", " ")
        if ":" in content:
            deps = content.split(":", 1)[1].split()
            for d in deps:
                if os.path.exists(d) and os.path.getmtime(d) > obj_mtime:
                    return True
    except OSError:
        pass
    return False


def compile_one(src):
    rel = os.path.relpath(src, "port").replace("\\", "/")
    obj = f"port/build/obj/{rel}.o"
    dep = f"{obj}.d"
    if not needs_recompile(src, obj, dep):
        return src, 0, "", False
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    cmd = [zig] + flags + ["-MMD", "-MF", dep, "-c", src, "-o", obj]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return src, r.returncode, r.stderr, True


def build_all():
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        results = list(ex.map(compile_one, srcs))

    failed = [r for r in results if r[1] != 0]
    rebuilt = sum(1 for r in results if r[3])
    if failed:
        for src, rc, err, _ in failed[:10]:
            print("=== FAILED:", src, "===")
            print(err[:1500])
        raise SystemExit(1)

    os.makedirs("sdl-arm/lib", exist_ok=True)
    sdl_shim = os.path.join(ROOT, "tools", "sdl_shim.cpp")
    sdl_so = "sdl-arm/lib/libSDL2-2.0.so.0"
    sdl_rebuilt = False
    if not os.path.exists(sdl_so) or os.path.getmtime(sdl_so) < os.path.getmtime(sdl_shim):
        sdl_cmd = [
            zig, "cc", "-x", "c", "-target", "arm-linux-gnueabihf.2.31", "-mcpu=cortex_a15",
            "-shared", "-fPIC", "-O3", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
            f"-I{sysroot}/usr/include/SDL2",
            f"-I{sysroot}/usr/include",
            f"-I{sysroot}/usr/include/arm-linux-gnueabihf",
            "-Wl,-soname,libSDL2-2.0.so.0",
            sdl_shim, "-o", sdl_so,
            "-lpthread", "-ldl", "-lm",
        ]
        r = subprocess.run(sdl_cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("SDL2 shim failed:", r.stderr)
            raise SystemExit(r.returncode)
        sdl_rebuilt = True

    ew2_bin = "port/build/ew2"
    if rebuilt > 0 or sdl_rebuilt or not os.path.exists(ew2_bin):
        objs = [
            f"port/build/obj/{os.path.relpath(s, 'port').replace(chr(92), '/')}.o"
            for s in srcs
        ]
        lib_dirs = [
            "-Lsdl-arm/lib",
            f"-L{sysroot}/usr/lib/arm-linux-gnueabihf",
            f"-L{sysroot}/lib/arm-linux-gnueabihf",
        ]
        link_cmd = (
            [zig, "c++", "-target", "arm-linux-gnueabihf.2.31", "-mcpu=cortex_a15", "-O3"]
            + objs
            + lib_dirs
            + [
                "-Wl,--allow-shlib-undefined",
                "-lpthread",
                "-ldl",
                "-lm",
                "-lrt",
                "-lz",
                "-lzip",
                "-lbsd",
                "-l:libSDL2-2.0.so.0",
                "-o",
                ew2_bin,
            ]
        )
        r = subprocess.run(link_cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("Link ew2 failed:", r.stderr)
            raise SystemExit(r.returncode)
        print(f"[build] Rebuilt port/build/ew2 ({rebuilt} files recompiled)")
        return True
    return False


if __name__ == "__main__":
    build_all()
