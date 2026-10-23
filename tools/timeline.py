#!/usr/bin/env python3
"""
Chronological frame & event timeline from logs/play.log and logs/ew2gl.log.
Usage:
    python tools/timeline.py [logs/play.log]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

KEEP = re.compile(
    r"frames=\d+|"
    r"glprof:|"
    r"glprof_detail:|"
    r"slow frame \d+:|"
    r"sysprof\[|"
    r"pcprof:|"
    r"pcprof_wait:|"
    r"gl bridge: frame|"
    r"gl bridge: connected|"
    r"module loaded:|"
    r"patched libunity|"
    r"fmod|"
    r"io: |"
    r"=== CRASH|SIGSEGV|SIGABRT|ew2 exited"
)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "logs", "play.log")
    if not os.path.exists(path):
        print(f"File not found: {path}")
        return
    with open(path, encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f, 1):
            s = line.rstrip()
            if KEEP.search(s):
                print(f"{i:6d}: {s}")


if __name__ == "__main__":
    main()
