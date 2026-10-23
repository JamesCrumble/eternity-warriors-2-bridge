import binascii
import ctypes
import glob
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
os.chdir(ROOT)
os.makedirs("logs", exist_ok=True)
os.makedirs("ctl", exist_ok=True)
os.makedirs("Saves", exist_ok=True)
os.makedirs(os.path.join("ew2data", "files"), exist_ok=True)
os.makedirs(os.path.join("ew2data", "shared_prefs"), exist_ok=True)

stop_event = threading.Event()


def wsl_base_cmd():
    # Use ew3-wsl if registered on this machine, otherwise default wsl
    return [r"C:\Windows\System32\wsl.exe", "-d", "ew3-wsl"]


def has_wsl_qemu_arm():
    if os.environ.get("EW2_FORCE_QEMU_SYSTEM", "") == "1":
        return False
    if os.environ.get("EW2_USE_WSL", "") != "1":
        return False
    try:
        r = subprocess.run(
            wsl_base_cmd() + ["-e", "sh", "-c", "command -v qemu-arm >/dev/null 2>&1"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=3,
        )
        return r.returncode == 0
    except Exception:
        return False


def to_wsl_path(win_path):
    p = os.path.abspath(win_path).replace("\\", "/")
    if len(p) >= 2 and p[1] == ":":
        return f"/mnt/{p[0].lower()}{p[2:]}"
    return p


def normalize_savegame_platform(save_path):
    """Ensure GLU2 savegame.bin has platformFlg == 'Android' so Android Unity runtime accepts PC saves."""
    if not os.path.exists(save_path):
        return
    with open(save_path, "rb") as f:
        data = f.read()
    if len(data) < 10 or data[:4] != b"GLU2" or data[8] != 1:
        return
    key = b"EternityWorriors2isagoodgameIloveIt"

    def rc4_crypt(buf):
        s = list(range(256))
        j = 0
        for i in range(256):
            j = (j + key[i % len(key)] + s[i]) & 0xFF
            s[i], s[j] = s[j], s[i]
        x = y = 0
        out = bytearray(buf)
        for i in range(len(out)):
            x = (x + 1) & 0xFF
            y = (y + s[x]) & 0xFF
            s[x], s[y] = s[y], s[x]
            out[i] ^= s[(s[x] + s[y]) & 0xFF]
        return bytes(out)

    plain = rc4_crypt(data[9:])
    target = b"\x0bplatformFlg\x03\x0dWindowsPlayer"
    replacement = b"\x0bplatformFlg\x03\x07Android"
    if target not in plain:
        return
    plain = plain.replace(target, replacement)
    enc = rc4_crypt(plain)
    crc = (binascii.crc32(enc) & 0xFFFFFFFF) ^ struct.unpack("<I", key[:4])[0]
    new_data = b"GLU2" + struct.pack("<I", crc) + b"\x01" + enc
    with open(save_path, "wb") as f:
        f.write(new_data)
    print(f"[saves] normalized platformFlg (WindowsPlayer -> Android) in {os.path.relpath(save_path, ROOT)}")


def sync_saves_before_launch():
    """Ensure Saves/savegame.bin and ew2data/files/savegame.bin are in sync before boot."""
    saves_bin = os.path.join(ROOT, "Saves", "savegame.bin")
    ew2_bin = os.path.join(ROOT, "ew2data", "files", "savegame.bin")
    cheat_bin = os.path.join(ROOT, "Optional_Cheat_Save", "savegame.bin")
    normalize_savegame_platform(saves_bin)
    normalize_savegame_platform(ew2_bin)
    normalize_savegame_platform(cheat_bin)
    if os.path.exists(saves_bin):
        shutil.copy2(saves_bin, ew2_bin)
    elif os.path.exists(ew2_bin):
        shutil.copy2(ew2_bin, saves_bin)


def ensure_build(use_wsl=False):
    zig = os.path.join(ROOT, "third_party", "zig", "zig.exe")

    ew2gl_exe = os.path.join(ROOT, "host", "build-win", "ew2gl.exe")
    gl_srcs = glob.glob(os.path.join(ROOT, "host", "glbridge", "*.c")) + glob.glob(
        os.path.join(ROOT, "host", "glbridge", "*.inc")
    )
    if not os.path.exists(ew2gl_exe) or any(
        os.path.getmtime(s) > os.path.getmtime(ew2gl_exe) for s in gl_srcs
    ):
        print("[launcher] building host/build-win/ew2gl.exe...")
        os.makedirs(os.path.join(ROOT, "host", "build-win"), exist_ok=True)
        subprocess.run(
            [
                zig,
                "cc",
                "-target",
                "x86_64-windows-gnu",
                "-O3",
                "-DSDL_MAIN_HANDLED",
                "-Ihost/glbridge",
                "-Ithird_party/khronos/include",
                "-Ithird_party/sdl2-win/include",
                "-Ithird_party/sdl2-win/include/SDL2",
                "host/glbridge/ew3gl.c",
                "third_party/sdl2-win/lib/SDL2.lib",
                "host/build-win/libGLESv2.lib",
                "-lws2_32",
                "-o",
                ew2gl_exe,
            ],
            check=True,
        )

    wsl_runner_src = os.path.join(ROOT, "third_party", "qemu-guest", "wsl_runner.c")
    wsl_runner_bin = os.path.join(ROOT, "third_party", "qemu-guest", "wsl_runner")
    if os.path.exists(wsl_runner_src) and (
        not os.path.exists(wsl_runner_bin)
        or os.path.getmtime(wsl_runner_src) > os.path.getmtime(wsl_runner_bin)
    ):
        print("[launcher] building third_party/qemu-guest/wsl_runner...")
        subprocess.run(
            [
                zig,
                "cc",
                "-target",
                "x86_64-linux-musl",
                "-static",
                "-O2",
                wsl_runner_src,
                "-o",
                wsl_runner_bin,
                "-lpthread",
            ],
            check=True,
        )

    from tools.build_port import build_all
    from tools.optimize_obb import optimize_obb

    optimize_obb()
    port_rebuilt = build_all()

    if use_wsl:
        return

    initrd_path = os.path.join(ROOT, "third_party", "qemu-guest", "ew2_initrd.cpio")
    initrd_deps = [
        os.path.join(ROOT, "port", "build", "ew2"),
        os.path.join(ROOT, "sdl-arm", "lib", "libSDL2-2.0.so.0"),
        os.path.join(ROOT, "third_party", "qemu-guest", "runner.c"),
        os.path.join(ROOT, "third_party", "qemu-guest", "build_initrd.py"),
        os.path.join(
            ROOT, "ew2data", "Android", "obb", "com.glu.ewarriors2", "main.431.com.glu.ewarriors2.obb"
        ),
    ]
    if (
        port_rebuilt
        or not os.path.exists(initrd_path)
        or any(os.path.getmtime(d) > os.path.getmtime(initrd_path) for d in initrd_deps)
    ):
        print("[launcher] packing guest initramfs (ew2_initrd.cpio)...")
        subprocess.run(
            [sys.executable, os.path.join(ROOT, "third_party", "qemu-guest", "build_initrd.py")],
            check=True,
        )


class EW2HTTPHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if "/version" in self.path:
            body = b"Version\nskip\nskip\nfalse\nAAAAAAAAAA\n0\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        body = b"{}"
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or "0")
        if n > 0:
            self.rfile.read(n)
        body = b'{"status":"ok"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def start_local_http_stub():
    try:
        srv = ThreadingHTTPServer(("127.0.0.1", 18080), EW2HTTPHandler)
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        return srv
    except OSError:
        return None


GUEST_HOSTS = ["127.0.0.1"]
CONNECTED_HOST = "127.0.0.1"
mux_connected = threading.Event()


def connect_retry(port):
    global CONNECTED_HOST
    while not stop_event.is_set():
        for h in list(GUEST_HOSTS):
            try:
                s = socket.create_connection((h, port), timeout=0.5)
                s.settimeout(None)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                CONNECTED_HOST = h
                if port == 18092:
                    mux_connected.set()
                return s
            except OSError:
                pass
        time.sleep(0.1)
    return None


def ctl_relay_worker(cmd_file):
    s = connect_retry(18091)
    if not s:
        return
    with open(cmd_file, "rb") as f:
        while not stop_event.is_set():
            data = f.read()
            if data:
                try:
                    s.sendall(data)
                except OSError:
                    break
            else:
                time.sleep(0.001)
    try:
        s.close()
    except OSError:
        pass


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def net_mux_worker(play_log=None):
    mux_sock = connect_retry(18092)
    if not mux_sock:
        return
    mux_lock = threading.Lock()
    conns = {}

    def send_mux(cid, cmd, payload=b""):
        hdr = struct.pack("<IHH", cid, cmd, len(payload))
        with mux_lock:
            try:
                mux_sock.sendall(hdr + payload)
            except OSError:
                pass

    # Push current local Saves/savegame.bin, Saves/playerprefs.txt, and Saves/GWalletHelper.xml to the guest
    save_files = [
        (1, os.path.join(ROOT, "Saves", "savegame.bin")),
        (2, os.path.join(ROOT, "Saves", "playerprefs.txt")),
        (3, os.path.join(ROOT, "Saves", "GWalletHelper.xml")),
    ]
    for cid, spath in save_files:
        if os.path.exists(spath):
            try:
                data = open(spath, "rb").read()
                if 0 < len(data) <= 32768:
                    send_mux(cid, 10, data)
            except OSError:
                pass
    send_mux(0, 11, b"")

    def backend_reader(cid, bsock):
        try:
            while not stop_event.is_set():
                data = bsock.recv(16384)
                if not data:
                    break
                send_mux(cid, 2, data)
        except OSError:
            pass
        finally:
            send_mux(cid, 3, b"")
            conns.pop(cid, None)
            try:
                bsock.close()
            except OSError:
                pass

    while not stop_event.is_set():
        try:
            hdr = recv_exact(mux_sock, 8)
        except OSError:
            break
        if not hdr:
            break
        cid, cmd, dlen = struct.unpack("<IHH", hdr)
        if cmd == 1:
            port = dlen
            try:
                bsock = socket.create_connection(("127.0.0.1", port), timeout=2.0)
                bsock.settimeout(None)
                bsock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                conns[cid] = bsock
                t = threading.Thread(target=backend_reader, args=(cid, bsock), daemon=True)
                t.start()
            except OSError:
                send_mux(cid, 3, b"")
        elif cmd == 2:
            payload = recv_exact(mux_sock, dlen) if dlen > 0 else b""
            if payload is None:
                break
            bsock = conns.get(cid)
            if bsock:
                try:
                    bsock.sendall(payload)
                except OSError:
                    conns.pop(cid, None)
                    try:
                        bsock.close()
                    except OSError:
                        pass
        elif cmd == 3:
            bsock = conns.pop(cid, None)
            if bsock:
                try:
                    bsock.close()
                except OSError:
                    pass
        elif cmd == 10:
            payload = recv_exact(mux_sock, dlen) if dlen > 0 else b""
            if payload is None:
                break
            fname = (
                "savegame.bin"
                if cid == 1
                else ("playerprefs.txt" if cid == 2 else ("GWalletHelper.xml" if cid == 3 else None))
            )
            if fname and payload:
                second_dir = (
                    os.path.join(ROOT, "ew2data", "shared_prefs")
                    if cid == 3
                    else os.path.join(ROOT, "ew2data", "files")
                )
                for dst_dir in (os.path.join(ROOT, "Saves"), second_dir):
                    os.makedirs(dst_dir, exist_ok=True)
                    tmp_p = os.path.join(dst_dir, fname + ".tmp")
                    dst_p = os.path.join(dst_dir, fname)
                    try:
                        with open(tmp_p, "wb") as sf:
                            sf.write(payload)
                        os.replace(tmp_p, dst_p)
                    except OSError:
                        pass
                print(f"[saves] persisted {fname} ({len(payload)} bytes) -> Saves/{fname}")
        elif cmd == 12:
            payload = recv_exact(mux_sock, dlen) if dlen > 0 else b""
            if payload is None:
                break
            if play_log is not None and payload:
                try:
                    play_log.write(payload)
                    play_log.flush()
                except OSError:
                    pass


class SDL_AudioSpec(ctypes.Structure):
    _fields_ = [
        ("freq", ctypes.c_int),
        ("format", ctypes.c_uint16),
        ("channels", ctypes.c_uint8),
        ("silence", ctypes.c_uint8),
        ("samples", ctypes.c_uint16),
        ("padding", ctypes.c_uint16),
        ("size", ctypes.c_uint32),
        ("callback", ctypes.c_void_p),
        ("userdata", ctypes.c_void_p),
    ]


def audio_worker():
    s = connect_retry(18093)
    if not s:
        return
    try:
        sdl = ctypes.CDLL(os.path.join(ROOT, "host", "build-win", "SDL2.dll"))
        sdl.SDL_InitSubSystem.argtypes = [ctypes.c_uint32]
        sdl.SDL_InitSubSystem.restype = ctypes.c_int
        sdl.SDL_OpenAudioDevice.argtypes = [
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.POINTER(SDL_AudioSpec),
            ctypes.POINTER(SDL_AudioSpec),
            ctypes.c_int,
        ]
        sdl.SDL_OpenAudioDevice.restype = ctypes.c_uint32
        sdl.SDL_PauseAudioDevice.argtypes = [ctypes.c_uint32, ctypes.c_int]
        sdl.SDL_PauseAudioDevice.restype = None
        sdl.SDL_QueueAudio.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32]
        sdl.SDL_QueueAudio.restype = ctypes.c_int
        sdl.SDL_GetQueuedAudioSize.argtypes = [ctypes.c_uint32]
        sdl.SDL_GetQueuedAudioSize.restype = ctypes.c_uint32
        sdl.SDL_CloseAudioDevice.argtypes = [ctypes.c_uint32]
        sdl.SDL_CloseAudioDevice.restype = None

        sdl.SDL_InitSubSystem(0x00000010)  # SDL_INIT_AUDIO
        dev_id = 0
        cur_freq = 0
        while not stop_event.is_set():
            hdr = recv_exact(s, 12)
            if not hdr:
                break
            magic, freq, dlen = struct.unpack("<III", hdr)
            if magic != 0x41335745 or dlen > 65536:
                break
            pcm = recv_exact(s, dlen)
            if not pcm:
                break
            if freq != cur_freq and freq > 0:
                if dev_id:
                    sdl.SDL_CloseAudioDevice(dev_id)
                want = SDL_AudioSpec()
                obtained = SDL_AudioSpec()
                want.freq = freq
                want.format = 0x8010  # AUDIO_S16LSB
                want.channels = 2
                want.samples = 1024
                dev_id = sdl.SDL_OpenAudioDevice(
                    None, 0, ctypes.byref(want), ctypes.byref(obtained), 0
                )
                if dev_id:
                    sdl.SDL_PauseAudioDevice(dev_id, 0)
                    cur_freq = freq
                    print(f"[audio] opened SDL2 audio device ({freq} Hz)", flush=True)
            if dev_id:
                max_queued = cur_freq * 4 // 4  # ~250ms max latency buffer
                if sdl.SDL_GetQueuedAudioSize(dev_id) <= max_queued:
                    sdl.SDL_QueueAudio(dev_id, pcm, dlen)
    except Exception as e:
        print(f"[audio] note: {e}")


def cleanup_old_session(use_wsl=False):
    for proc_name in ("ew2gl.exe", "qemu-system-arm.exe"):
        subprocess.run(
            ["taskkill", "/F", "/IM", proc_name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    if use_wsl:
        try:
            subprocess.run(
                wsl_base_cmd()
                + [
                    "-e",
                    "sh",
                    "-c",
                    "pkill -9 -f wsl_runner 2>/dev/null; pkill -9 -f '/opt/ew2_rt/app/ew2' 2>/dev/null; true",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=3,
            )
        except Exception:
            pass
    time.sleep(0.3)


def start_wsl_guest(w, h, play_log):
    try:
        ip_out = (
            subprocess.check_output(
                wsl_base_cmd()
                + [
                    "-e",
                    "sh",
                    "-c",
                    "ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i==\"src\") print $(i+1)}'",
                ],
                stdin=subprocess.DEVNULL,
                text=True,
                timeout=5,
            )
            .strip()
            .split()
        )
        if ip_out and ip_out[0] not in GUEST_HOSTS:
            GUEST_HOSTS.append(ip_out[0])
    except Exception:
        pass

    wsl_root = to_wsl_path(ROOT)
    start_sh = os.path.join(ROOT, "third_party", "qemu-guest", "start_wsl.sh")
    with open(start_sh, "wb") as sf:
        sf.write(
            b"#!/bin/sh\n"
            b'cp -f "$1/third_party/qemu-guest/wsl_runner" /tmp/ew2_wsl_runner\n'
            b"chmod +x /tmp/ew2_wsl_runner\n"
            b'exec /tmp/ew2_wsl_runner "$1" "$2" "$3"\n'
        )
    wsl_cmd = wsl_base_cmd() + [
        "-e",
        "sh",
        f"{wsl_root}/third_party/qemu-guest/start_wsl.sh",
        wsl_root,
        str(w),
        str(h),
    ]
    print("[launcher] starting ARM32 guest via WSL2 qemu-arm (user-mode)...")
    return subprocess.Popen(
        wsl_cmd,
        stdin=subprocess.DEVNULL,
        stdout=play_log,
        stderr=subprocess.STDOUT,
    )


def main():
    res = sys.argv[1] if len(sys.argv) > 1 else "1280x720"
    w, h = res.split("x")
    sync_saves_before_launch()
    use_wsl = has_wsl_qemu_arm()
    cleanup_old_session(use_wsl=use_wsl)
    ensure_build(use_wsl=use_wsl)
    start_local_http_stub()

    cmd_file = os.path.join(ROOT, "ctl", "commands")
    with open(cmd_file, "wb"):
        pass

    play_log_path = os.path.join(ROOT, "logs", "play.log")
    with open(play_log_path, "wb"):
        pass
    play_log = open(play_log_path, "ab")

    if use_wsl:
        guest_proc = start_wsl_guest(w, h, play_log)
    else:
        qemu_exe = os.path.join(
            ROOT, "third_party", "qemu", "xpack-qemu-arm-9.2.4-1", "bin", "qemu-system-arm.exe"
        )
        gpu_arg = os.environ.get("EW2_GPU", "").strip().replace(" ", "_")
        target_fps = os.environ.get("EW2_TARGET_FPS", "120").strip()
        append_str = f"console=ttyAMA0 raid=noautodetect quiet ew2_w={w} ew2_h={h} ew2_fps={target_fps}"
        if os.environ.get("EW2_TRACE", "") == "1":
            append_str += " ew2_trace=1"
        if os.environ.get("EW2_PROF", "") == "1":
            append_str += " ew2_prof=1"
        if gpu_arg:
            append_str += f" ew2_gpu={gpu_arg}"

        qemu_cmd = [
            qemu_exe,
            "-M",
            "virt,highmem=off",
            "-accel",
            "tcg,thread=multi,tb-size=2048",
            "-cpu",
            "cortex-a15",
            "-smp",
            "4",
            "-m",
            "3000",
            "-nographic",
            "-kernel",
            os.path.join(ROOT, "third_party", "qemu-guest", "vmlinuz"),
            "-initrd",
            os.path.join(ROOT, "third_party", "qemu-guest", "ew2_initrd.cpio"),
            "-device",
            "virtio-serial-device",
            "-chardev",
            "socket,id=ch1,host=127.0.0.1,port=18090,server=on,wait=off,nodelay=on",
            "-device",
            "virtserialport,chardev=ch1,name=p1",
            "-chardev",
            "socket,id=ch2,host=127.0.0.1,port=18091,server=on,wait=off,nodelay=on",
            "-device",
            "virtserialport,chardev=ch2,name=p2",
            "-chardev",
            "socket,id=ch3,host=127.0.0.1,port=18092,server=on,wait=off,nodelay=on",
            "-device",
            "virtserialport,chardev=ch3,name=p3",
            "-chardev",
            "socket,id=ch4,host=127.0.0.1,port=18093,server=on,wait=off,nodelay=on",
            "-device",
            "virtserialport,chardev=ch4,name=p4",
            "-append",
            append_str,
        ]
        print("[launcher] starting ARM32 guest VM (qemu-system-arm)...")
        HIGH_PRIORITY_CLASS = 0x00000080
        guest_proc = subprocess.Popen(
            qemu_cmd,
            stdin=subprocess.DEVNULL,
            stdout=play_log,
            stderr=subprocess.STDOUT,
            creationflags=HIGH_PRIORITY_CLASS,
        )

    threading.Thread(target=ctl_relay_worker, args=(cmd_file,), daemon=True).start()
    threading.Thread(target=net_mux_worker, args=(play_log,), daemon=True).start()
    threading.Thread(target=audio_worker, daemon=True).start()

    mux_connected.wait(timeout=10.0)

    env = os.environ.copy()
    env["EW2_GL_CONNECT"] = f"{CONNECTED_HOST}:18090"
    env["EW2_WIDTH"] = w
    env["EW2_HEIGHT"] = h
    env["EW2_CONTROL_DIR"] = os.path.join(ROOT, "ctl")
    env["EW2_GL_LOG"] = os.path.join(ROOT, "logs", "ew2gl.log")
    with open(env["EW2_GL_LOG"], "w", encoding="utf-8"):
        pass
    env["EW2_GL_VSYNC"] = env.get("EW2_GL_VSYNC", "0")
    env["EW2_GL_REPORT"] = env.get("EW2_GL_REPORT", "2")

    ew2gl_exe = os.path.join(ROOT, "host", "build-win", "ew2gl.exe")
    print("[launcher] starting ew2gl.exe window...")
    gl_proc = subprocess.Popen([ew2gl_exe], env=env, creationflags=0x00000080)

    try:
        gl_proc.wait()
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        if gl_proc.poll() is None:
            gl_proc.terminate()
        if guest_proc.poll() is None:
            guest_proc.terminate()
            try:
                guest_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                guest_proc.kill()
        if use_wsl:
            try:
                subprocess.run(
                    wsl_base_cmd()
                    + [
                        "-e",
                        "sh",
                        "-c",
                        "pkill -9 -f wsl_runner 2>/dev/null; pkill -9 -f '/opt/ew2_rt/app/ew2' 2>/dev/null; true",
                    ],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=3,
                )
            except Exception:
                pass
        play_log.close()
        print("[launcher] game session ended.")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback

        err = traceback.format_exc()
        print(err, file=sys.stderr)
        with open(os.path.join(ROOT, "logs", "launcher.log"), "w", encoding="utf-8") as f:
            f.write(err)
        sys.exit(1)
