#!/usr/bin/env python3
"""
Summarize synchronous GL bridge queries and per-op GPU/host timings from logs/ew2gl.log.
Usage:
    python tools/sync_ops.py [logs/ew2gl.log]
"""
from collections import defaultdict
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "logs", "ew2gl.log")
    if not os.path.exists(path):
        print(f"File not found: {path}")
        return
    sync_tot = defaultdict(float)
    ms_tot = defaultdict(float)
    n_reports = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "fps," not in line:
                continue
            n_reports += 1
            m_sync = re.search(r"sync/frame:([^|]*)", line)
            if m_sync:
                for k, v in re.findall(r"(\w+)=([\d.]+)", m_sync.group(1)):
                    sync_tot[k] += float(v)
            m_ms = re.search(r"ms/frame:([^|]*)", line)
            if m_ms:
                for k, v in re.findall(r"(\w+)=([\d.]+)", m_ms.group(1)):
                    ms_tot[k] += float(v)

    print(f"Host GL reports analyzed: {n_reports}")
    if ms_tot:
        print("\n=== Average Host GL Time per Frame (ms/frame) ===")
        for k, v in sorted(ms_tot.items(), key=lambda x: -x[1]):
            print(f"  {k:28s}: {v / max(1, n_reports):.3f} ms/frame")
    if sync_tot:
        print("\n=== Synchronous Queries per Frame (pipeline drains) ===")
        for k, v in sorted(sync_tot.items(), key=lambda x: -x[1]):
            print(f"  {k:28s}: {v / max(1, n_reports):.3f} /frame")


if __name__ == "__main__":
    main()
