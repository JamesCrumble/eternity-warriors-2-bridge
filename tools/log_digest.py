#!/usr/bin/env python3
"""
Digest of EW2 Bridge logs (logs/play.log + logs/ew2gl.log):
  - Frame rate, step/p95/max/cpu/bridge/swap wait (glprof)
  - Unity PlayerLoop subsystem & GL bridge breakdown (glprof_detail)
  - Slow frames / hitches attribution (slow frame)
  - Sampling profiler hotspots (pcprof, pcprof_incl, pcprof_wait) with ELF symbol resolution
  - Syscall profiler breakdown (sysprof)
  - Host GL bridge op timings, sync queries, counts, and bandwidth (ew2gl.log)
  - Crashes / signals / errors

Usage:
    python tools/log_digest.py [logs_dir_or_files...]
"""
from bisect import bisect_right
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ELF_PATHS = {
    "libunity.so": os.path.join(ROOT, "ew2data", "lib", "armeabi-v7a", "libunity.so"),
    "libmono.so": os.path.join(ROOT, "ew2data", "lib", "armeabi-v7a", "libmono.so"),
    "libmain.so": os.path.join(ROOT, "ew2data", "lib", "armeabi-v7a", "libmain.so"),
    "host:ew2": os.path.join(ROOT, "port", "build", "ew2"),
    "host:libSDL2-2.0.so.0": os.path.join(ROOT, "sdl-arm", "lib", "libSDL2-2.0.so.0"),
}

_SYM_CACHE = {}


def load_elf_symbols(path):
    if path in _SYM_CACHE:
        return _SYM_CACHE[path]
    syms = []
    if not path or not os.path.exists(path):
        _SYM_CACHE[path] = syms
        return syms
    try:
        data = open(path, "rb").read()
        if len(data) < 52 or data[:4] != b"\x7fELF":
            _SYM_CACHE[path] = syms
            return syms
        e_phoff, e_shoff = struct.unpack_from("<II", data, 28)
        e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from("<HHHH", data, 42)
        text_vaddr = 0
        for i in range(e_phnum):
            poff = e_phoff + i * e_phentsize
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags = struct.unpack_from(
                "<7I", data, poff
            )
            if p_type == 1 and (p_flags & 1):  # PT_LOAD + PF_X
                text_vaddr = p_vaddr
                break
        shdrs = []
        for i in range(e_shnum):
            soff = e_shoff + i * e_shentsize
            shdrs.append(struct.unpack_from("<10I", data, soff))
        for sh in shdrs:
            sh_type, sh_offset, sh_size, sh_link, sh_entsize = sh[1], sh[4], sh[5], sh[6], sh[9]
            if sh_type not in (2, 11) or sh_entsize == 0 or sh_link >= len(shdrs):
                continue
            str_sh = shdrs[sh_link]
            strtab = data[str_sh[4] : str_sh[4] + str_sh[5]]
            n = sh_size // sh_entsize
            for j in range(n):
                eoff = sh_offset + j * sh_entsize
                st_name, st_value, st_size, st_info = struct.unpack_from("<IIIB", data, eoff)
                st_type = st_info & 0xF
                if st_value == 0 or st_type not in (1, 2):
                    continue
                end = strtab.find(b"\x00", st_name)
                if end <= st_name:
                    continue
                name = strtab[st_name:end].decode("utf-8", "replace")
                if name.startswith("$"):
                    continue
                addr = (st_value & ~1) - text_vaddr
                if addr >= 0:
                    syms.append((addr, name))
        syms.sort()
    except Exception:
        pass
    _SYM_CACHE[path] = syms
    return syms


def resolve_sym(mod, off):
    syms = load_elf_symbols(ELF_PATHS.get(mod, ""))
    if not syms:
        return f"0x{off:x}"
    idx = bisect_right(syms, (off, "\xff")) - 1
    if idx < 0:
        return f"0x{off:x}"
    base, name = syms[idx]
    if off - base > 0x10000:
        return f"0x{off:x}"
    return f"{name}+0x{off - base:x} (0x{off:x})"


def median(xs):
    s = sorted(xs)
    return s[len(s) // 2] if s else 0.0


def digest(lines, label="logs"):
    print(f"=== {label} ({len(lines)} lines) ===")

    glprof = []
    for ln in lines:
        m = re.search(
            r"glprof: period ([\d.]+) ms, step ([\d.]+) ms "
            r"\((?:p95 ([\d.]+), max ([\d.]+); )?cpu ([\d.]+) ms, bridge ([\d.]+) ms, (\d+) calls\), "
            r"swap wait ([\d.]+) ms",
            ln,
        )
        if m:
            p, s, p95, mx, c, b, calls, w = m.groups()
            glprof.append(
                (
                    float(p),
                    float(s),
                    float(p95 or 0),
                    float(mx or 0),
                    float(c),
                    float(b),
                    int(calls),
                    float(w),
                )
            )

    if glprof:
        steady = glprof[1:] or glprof
        periods = [x[0] for x in steady if x[0] > 0]
        steps = [x[1] for x in steady]
        p95s = [x[2] for x in steady if x[2] > 0]
        maxs = [x[3] for x in steady if x[3] > 0]
        cpus = [x[4] for x in steady]
        bridges = [x[5] for x in steady]
        calls = [x[6] for x in steady]
        waits = [x[7] for x in steady]
        med_p = median(periods) if periods else median(steps)
        fps = 1000.0 / med_p if med_p > 0 else 0
        print(
            f"  frames (~{len(glprof)*120}): median {fps:.1f} fps (period {med_p:.1f} ms) | "
            f"step {median(steps):.1f} ms"
            + (f" (p95 {median(p95s):.1f}, peak {max(maxs):.0f})" if p95s else "")
            + f" = cpu {median(cpus):.1f} ms (of which bridge {median(bridges):.1f} ms, "
            f"{median(calls):.0f} calls) + wait {median(waits):.1f} ms"
        )
        print("  last glprof windows:")
        for p, s, p95, mx, c, b, ca, w in glprof[-6:]:
            fps_w = 1000.0 / p if p > 0 else 0
            print(
                f"    {fps_w:5.1f} fps | period {p:5.1f} ms | step {s:5.1f} ms "
                f"(p95 {p95:5.1f}, max {mx:5.1f}) | cpu {c:5.1f} ms | bridge {b:4.1f} ms ({ca} calls) | swap wait {w:4.1f} ms"
            )

    details = []
    for ln in lines:
        m = re.search(
            r"glprof_detail: unity\[scripts=([\d.]+) coro=([\d.]+) anim=([\d.]+) fixed=([\d.]+) "
            r"phys=([\d.]+) skin=([\d.]+) render\+gui=([\d.]+) ms\] "
            r"bridge\[enc=([\d.]+) write=([\d.]+) sync=([\d.]+) ms\] "
            r"work\[draws=([\d.]+) vtx=([\d.]+)KB unif=([\d.]+) io_opens=([\d.]+) io=([\d.]+)KB tex=([\d.]+)KB buf=([\d.]+)KB\]",
            ln,
        )
        if m:
            details.append([float(x) for x in m.groups()])

    if details:
        st = details[1:] or details
        names = [
            "scripts (C# Update/LateUpdate)",
            "coroutines (DelayedCall)",
            "animations",
            "fixedUpdate",
            "physics",
            "skinning/batching",
            "render+gui+audio (total)",
            "  -> bridge encode",
            "  -> bridge write",
            "  -> bridge sync wait",
        ]
        print("  per-frame subsystem breakdown (median over 120-frame windows):")
        for i, name in enumerate(names):
            vals = [row[i] for row in st]
            print(f"    {name:32s}: {median(vals):6.2f} ms  (max window {max(vals):6.2f} ms)")
        print(
            f"  per-frame work (median): draws={median([r[10] for r in st]):.0f}, "
            f"client_vtx={median([r[11] for r in st]):.1f} KB, "
            f"uniforms={median([r[12] for r in st]):.0f}, "
            f"io_opens={median([r[13] for r in st]):.1f}, "
            f"io_read={median([r[14] for r in st]):.1f} KB, "
            f"tex_up={median([r[15] for r in st]):.1f} KB, "
            f"buf_up={median([r[16] for r in st]):.1f} KB"
        )

    slow = []
    causes = {}
    for ln in lines:
        m = re.search(r"slow frame (\d+): step (\d+) ms", ln)
        if m:
            slow.append((int(m.group(2)), int(m.group(1)), ln.strip()))
            for part in ln.split(" | ")[1:]:
                head = part.split()[0]
                causes[head] = causes.get(head, 0) + 1
    if slow:
        slow.sort(reverse=True)
        why = ", ".join(f"{k}={v}" for k, v in sorted(causes.items(), key=lambda x: -x[1]))
        print(
            f"  slow frames: {len(slow)} hitches (worst {slow[0][0]} ms at frame {slow[0][1]}; "
            f"with activity: {why or 'cpu/emulation only'})"
        )
        for _, _, s in slow[:8]:
            print(f"    {s}")

    # pcprof
    mod_Pct = {}
    leaf_blocks = {}
    incl_blocks = {}
    waits = {}
    for ln in lines:
        if "pcprof:" in ln and "samples of the engine thread:" in ln:
            for m_mod, pct in re.findall(r"(\S+)\s+([\d.]+)%", ln.split("thread:")[1]):
                mod_Pct.setdefault(m_mod, []).append(float(pct))
        m = re.search(r"pcprof\[([^\]]+)\](.*)", ln)
        if m:
            mod, rest = m.group(1), m.group(2)
            for off_s, cnt_s in re.findall(r"0x([0-9a-fA-F]+)=(\d+)", rest):
                off = int(off_s, 16)
                leaf_blocks.setdefault(mod, {})[off] = leaf_blocks.get(mod, {}).get(off, 0) + int(cnt_s)
        m = re.search(r"pcprof_incl\[([^\]]+)\](.*)", ln)
        if m:
            mod, rest = m.group(1), m.group(2)
            for off_s, cnt_s in re.findall(r"0x([0-9a-fA-F]+)=(\d+)", rest):
                off = int(off_s, 16)
                incl_blocks.setdefault(mod, {})[off] = incl_blocks.get(mod, {}).get(off, 0) + int(cnt_s)
        if "pcprof_wait:" in ln:
            for k, cnt_s in re.findall(r"\[([^\]]+)\]=(\d+)", ln):
                waits[k] = waits.get(k, 0) + int(cnt_s)

    if mod_Pct:
        summary = ", ".join(
            f"{k} {median(v):.1f}%" for k, v in sorted(mod_Pct.items(), key=lambda x: -median(x[1]))
        )
        print(f"  pcprof modules (median): {summary}")
        for mod, mp in sorted(leaf_blocks.items(), key=lambda x: -sum(x[1].values()))[:4]:
            top = sorted(mp.items(), key=lambda x: -x[1])[:6]
            s_top = ", ".join(f"{resolve_sym(mod, o)}={c}" for o, c in top)
            print(f"    leaf [{mod}]: {s_top}")
        for mod, mp in sorted(incl_blocks.items(), key=lambda x: -sum(x[1].values()))[:3]:
            top = sorted(mp.items(), key=lambda x: -x[1])[:6]
            s_top = ", ".join(f"{resolve_sym(mod, o)}={c}" for o, c in top)
            print(f"    incl [{mod}]: {s_top}")
        if waits:
            top_w = sorted(waits.items(), key=lambda x: -x[1])[:6]
            print("    waits: " + ", ".join(f"[{k}]={v}" for k, v in top_w))

    # sysprof
    sysprof_last = {}
    for ln in lines:
        m = re.search(r"sysprof\[(\w+)\]: (.*)", ln)
        if m:
            sysprof_last[m.group(1)] = m.group(2).strip()
    if sysprof_last:
        print("  sysprof (latest window):")
        for k, v in sysprof_last.items():
            print(f"    [{k}]: {v}")

    # Host GL bridge lines (from ew2gl.log)
    host_gl = []
    for ln in lines:
        m = re.search(
            r"([\d.]+) fps, (\d+) cmds, ([\d.]+) MB/s, frames (\d+), busy (\d+)%(.*)", ln
        )
        if m:
            host_gl.append(
                (
                    float(m.group(1)),
                    int(m.group(2)),
                    float(m.group(3)),
                    int(m.group(4)),
                    int(m.group(5)),
                    m.group(6).strip(),
                )
            )
    if host_gl:
        st = host_gl[1:] or host_gl
        print(
            f"  host GL bridge ({len(host_gl)} reports): median {median([x[0] for x in st]):.1f} fps "
            f"(peak {max(x[0] for x in st):.1f}), {median([x[2] for x in st]):.1f} MB/s, "
            f"GPU/host busy median {median([x[4] for x in st]):.0f}%"
        )
        for fps_h, cmds, mbs, fr, busy, tail in host_gl[-5:]:
            print(f"    {fps_h:5.1f} fps, {mbs:4.1f} MB/s, busy {busy:2d}% {tail}")

    crashes = [
        ln.strip()
        for ln in lines
        if any(k in ln for k in ("=== CRASH", "SIGSEGV", "SIGABRT", "SIGILL", "SIGBUS", "FATAL", "unresolved symbol"))
    ]
    if crashes:
        print("  CRASH/ERROR:")
        for c in crashes[:12]:
            print(f"    {c}")
    print()


def main():
    args = sys.argv[1:] or [os.path.join(ROOT, "logs")]
    paths = []
    for a in args:
        if os.path.isdir(a):
            for name in ("play.log", "ew2gl.log", "launcher.log"):
                p = os.path.join(a, name)
                if os.path.exists(p):
                    paths.append(p)
        elif os.path.exists(a):
            paths.append(a)
    for p in paths:
        lines = open(p, encoding="utf-8", errors="replace").read().splitlines()
        digest(lines, os.path.relpath(p, ROOT))


if __name__ == "__main__":
    main()
