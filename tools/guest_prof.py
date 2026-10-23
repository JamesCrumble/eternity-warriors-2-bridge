#!/usr/bin/env python3
"""
Symbol-resolved aggregate guest CPU / call-stack / syscall profiler report from logs/play.log.
Usage:
    python tools/guest_prof.py [logs/play.log]
"""
import os
import re
import sys
from tools.log_digest import load_elf_symbols, resolve_sym, ROOT


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "logs", "play.log")
    if not os.path.exists(path):
        print(f"File not found: {path}")
        return
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()

    leaf = {}
    incl = {}
    waits = {}
    total_samples = 0

    for ln in lines:
        m = re.search(r"pcprof: (\d+) samples of the engine thread:", ln)
        if m:
            total_samples += int(m.group(1))
        m = re.search(r"pcprof\[([^\]]+)\](.*)", ln)
        if m:
            mod, rest = m.group(1), m.group(2)
            for off_s, cnt_s in re.findall(r"0x([0-9a-fA-F]+)=(\d+)", rest):
                off = int(off_s, 16)
                leaf.setdefault(mod, {})[off] = leaf.get(mod, {}).get(off, 0) + int(cnt_s)
        m = re.search(r"pcprof_incl\[([^\]]+)\](.*)", ln)
        if m:
            mod, rest = m.group(1), m.group(2)
            for off_s, cnt_s in re.findall(r"0x([0-9a-fA-F]+)=(\d+)", rest):
                off = int(off_s, 16)
                incl.setdefault(mod, {})[off] = incl.get(mod, {}).get(off, 0) + int(cnt_s)
        if "pcprof_wait:" in ln:
            for k, cnt_s in re.findall(r"\[([^\]]+)\]=(\d+)", ln):
                waits[k] = waits.get(k, 0) + int(cnt_s)

    print(f"Total engine thread samples: {total_samples}")
    for title, data in (("LEAF HOTSPOTS (exclusive PC)", leaf), ("INCLUSIVE CALLSTACK HOTSPOTS (LR/stack)", incl)):
        print(f"\n=== {title} ===")
        for mod, mp in sorted(data.items(), key=lambda x: -sum(x[1].values())):
            mod_tot = sum(mp.values())
            pct = (100.0 * mod_tot / total_samples) if total_samples else 0.0
            print(f"\n  [{mod}] total={mod_tot} ({pct:.1f}%):")
            by_sym = {}
            for off, cnt in mp.items():
                sym = resolve_sym(mod, off)
                fn = sym.split("+")[0]
                by_sym.setdefault(fn, [0, []])[0] += cnt
                by_sym[fn][1].append((cnt, off, sym))
            for fn, (fn_cnt, blocks) in sorted(by_sym.items(), key=lambda x: -x[1][0])[:20]:
                fn_pct = (100.0 * fn_cnt / total_samples) if total_samples else 0.0
                blocks.sort(reverse=True)
                top_b = ", ".join(f"0x{o:x}={c}" for c, o, _ in blocks[:4])
                print(f"    {fn_cnt:5d} ({fn_pct:5.1f}%)  {fn:45s} [{top_b}]")

    if waits:
        print("\n=== SYSCALL WAITS ON ENGINE THREAD ===")
        for k, cnt in sorted(waits.items(), key=lambda x: -x[1])[:20]:
            pct = (100.0 * cnt / total_samples) if total_samples else 0.0
            print(f"  {cnt:5d} ({pct:5.1f}%)  [{k}]")


if __name__ == "__main__":
    main()
