#!/usr/bin/env python3
"""Per-loop vectorisation report for a header library.

Compiles one translation unit with GCC (-fopt-info-vec-all) and Clang
(-Rpass/-Rpass-missed/-Rpass-analysis for loop-vectorize, one combined regex,
because a later -Rpass flag replaces an earlier one), then lists every
for/while/do loop in the headers under --src together with each compiler's
verdict (VEC, SLP, or the reason it was missed).

Rule: every loop that is not vectorised needs a reason (serial dependency,
scalar head/tail, explicit SIMD, or too short/cold to matter).

Usage:
  vec_report.py --tu tests/freestanding.cpp --src include/mylib -I include \
                [--exclude tables.hpp] [-- extra compiler flags...]
"""
import argparse
import re
import subprocess
from collections import defaultdict
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--tu", required=True, help="translation unit that instantiates the code to audit")
ap.add_argument("--src", required=True, help="directory whose headers are reported")
ap.add_argument("-I", dest="incs", action="append", default=[], help="include directory (repeatable)")
ap.add_argument("--exclude", action="append", default=[], help="file name to skip (repeatable), e.g. generated tables")
ap.add_argument("--std", default="c++23")
ap.add_argument("flags", nargs="*", help="extra compiler flags (after --)")
a = ap.parse_args()

src = Path(a.src).resolve()
common = [f"-std={a.std}", "-O3", "-ffreestanding", "-fno-exceptions", "-fno-rtti",
          *[f"-I{Path(i).resolve()}" for i in a.incs], "-c", str(Path(a.tu).resolve()), "-o", "/dev/null", *a.flags]

gcc = subprocess.run(["g++", *common, "-fopt-info-vec-all"], capture_output=True, text=True).stderr
clang = subprocess.run(["clang++", *common, "-Rpass=loop-vectorize|slp-vectorizer",
                        "-Rpass-missed=loop-vectorize", "-Rpass-analysis=loop-vectorize"],
                       capture_output=True, text=True).stderr

loc = re.compile(r"^(\S+?):(\d+):\d+: (.*)")


def key_of(path):
    try:
        return str(Path(path).resolve().relative_to(src))
    except ValueError:
        return None


g = defaultdict(set)
for line in gcc.splitlines():
    m = loc.search(line)
    if not m or (rel := key_of(m.group(1))) is None:
        continue
    k, msg = (rel, int(m.group(2))), m.group(3)
    if "loop vectorized" in msg:
        g[k].add("VEC")
    elif "basic block part vectorized" in msg:
        g[k].add("SLP")
    elif "not vectorized:" in msg:
        g[k].add("miss: " + msg.split("not vectorized:", 1)[1].strip()[:70])

c = defaultdict(set)
for line in clang.splitlines():
    m = loc.search(line)
    if not m or (rel := key_of(m.group(1))) is None:
        continue
    k, msg = (rel, int(m.group(2))), m.group(3)
    if "vectorized loop" in msg:
        c[k].add("VEC")
    elif "SLP vectorized" in msg:
        c[k].add("SLP")
    elif "loop not vectorized" in msg:
        c[k].add("miss: " + msg.split("loop not vectorized", 1)[1].strip(" :")[:70])
    elif "remark:" in msg and "loop" in msg:
        c[k].add("note: " + msg.split("remark:", 1)[1].strip()[:70])

loop_re = re.compile(r"^\s*(?:for\s*\(|while\s*\(|do\b)")


def loop_spans(lines):
    """(start, end) line numbers (1-based, inclusive) of every loop."""
    spans = []
    for i, text in enumerate(lines):
        s = text.strip()
        if not loop_re.match(text) or s.startswith("//"):
            continue
        if s.startswith("while") and s.endswith(";") and "{" not in s:
            continue  # the trailing "while (...);" of a do-while
        depth, opened, end = 0, False, i
        for j in range(i, min(len(lines), i + 400)):
            code = lines[j].split("//")[0]
            for ch in code:
                if ch == "{":
                    depth += 1
                    opened = True
                elif ch == "}":
                    depth -= 1
            if opened and depth <= 0:
                end = j
                break
            if not opened and code.rstrip().endswith(";") and (j > i or "(" in code):
                end = j
                break
        spans.append((i + 1, end + 2 if s.startswith("do") else end + 1))
    return spans


def verdict(s):
    for v in ("VEC", "SLP"):
        if v in s:
            return v
    misses = sorted((x for x in s if x.startswith("miss")), key=len, reverse=True)
    if misses:
        return misses[0]
    notes = sorted(x for x in s if x.startswith("note"))
    return notes[0] if notes else "-"


for path in sorted(src.rglob("*.h*")):
    if path.name in a.exclude:
        continue
    rel = str(path.relative_to(src))
    lines = path.read_text().splitlines()
    spans = loop_spans(lines)
    for start, end in spans:
        inner = [(x, y) for x, y in spans if (x, y) != (start, end) and start <= x and y <= end]

        def mine(n):
            return start <= n <= end and not any(x <= n <= y for x, y in inner)

        gs = set().union(*[v for (f, n), v in g.items() if f == rel and mine(n)] or [set()])
        cs = set().union(*[v for (f, n), v in c.items() if f == rel and mine(n)] or [set()])
        print(f"{rel}:{start:<4} | gcc: {verdict(gs):<44.44} | clang: {verdict(cs):<44.44} | "
              f"{lines[start - 1].strip()[:58]}")
