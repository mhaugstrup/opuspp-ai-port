#!/usr/bin/env python3
"""Per-loop vectorisation report for the opuspp headers.

Compiles tests/freestanding.cpp with GCC (-fopt-info-vec-all) and Clang
(-Rpass/-Rpass-missed/-Rpass-analysis for loop-vectorize), then lists every
for/while/do loop in include/opuspp together with each compiler's verdict.

Rule (not automated): every loop that is not vectorised must have a reason,
such as a serial dependency, a scalar head or tail, or explicit simd<T,N>
code. Check the report after adding or changing loops.

Usage: tools/vec_report.py [extra compiler flags...]
"""
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

root = Path(__file__).resolve().parent.parent
inc = root / "include"
extra = sys.argv[1:]
common = ["-std=c++23", "-O3", "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-I" + str(inc),
          "-c", str(root / "tests/freestanding.cpp"), "-o", "/dev/null", *extra]

gcc = subprocess.run(["g++", *common, "-fopt-info-vec-all"], capture_output=True, text=True).stderr
clang = subprocess.run(["clang++", *common, "-Rpass=loop-vectorize|slp-vectorizer", "-Rpass-missed=loop-vectorize",
                        "-Rpass-analysis=loop-vectorize"],
                       capture_output=True, text=True).stderr

loc = re.compile(r"include/opuspp/(\S+?):(\d+):\d+: (.*)")
g = defaultdict(set)
for line in gcc.splitlines():
    m = loc.search(line)
    if not m:
        continue
    key = (m.group(1), int(m.group(2)))
    msg = m.group(3)
    if "loop vectorized" in msg:
        g[key].add("VEC")
    elif "basic block part vectorized" in msg:
        g[key].add("SLP")
    elif "not vectorized:" in msg:
        g[key].add("miss: " + msg.split("not vectorized:", 1)[1].strip()[:70])

c = defaultdict(set)
for line in clang.splitlines():
    m = loc.search(line)
    if not m:
        continue
    key = (m.group(1), int(m.group(2)))
    msg = m.group(3)
    if "vectorized loop" in msg:
        c[key].add("VEC")
    elif "SLP vectorized" in msg or "Stores SLP vectorized" in msg:
        c[key].add("SLP")
    elif "remark: loop not vectorized" in msg or "loop not vectorized" in msg:
        c[key].add("miss: " + msg.split("loop not vectorized", 1)[1].strip(" :")[:70])
    elif "remark:" in msg and "loop" in msg:
        c[key].add("note: " + msg.split("remark:", 1)[1].strip()[:70])

loop_re = re.compile(r"^\s*(?:for\s*\(|while\s*\(|do\b)")


def loop_spans(lines):
    """Returns (start, end) line numbers (1-based, inclusive) of every loop."""
    spans = []
    for i, text in enumerate(lines):
        if not loop_re.match(text) or text.strip().startswith("//"):
            continue
        # Skip the trailing "while (...);" of a do-while.
        if text.strip().startswith("while") and text.rstrip().endswith(";") and "{" not in text:
            continue
        depth = 0
        opened = False
        end = i
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
            if not opened and j > i and code.rstrip().endswith(";"):
                end = j
                break
            if not opened and j == i and code.rstrip().endswith(";") and "(" in code:
                end = j
                break
        # do-while: include the closing "while (...);" line.
        if text.strip().startswith("do") and end + 1 < len(lines) and lines[end].strip().startswith("}") is False:
            pass
        spans.append((i + 1, end + 1 if not text.strip().startswith("do") else end + 2))
    return spans


def verdict(s):
    if "VEC" in s:
        return "VEC"
    if "SLP" in s:
        return "SLP"
    misses = sorted((x for x in s if x.startswith("miss")), key=len, reverse=True)
    if misses:
        return misses[0]
    notes = sorted(x for x in s if x.startswith("note"))
    return notes[0] if notes else "-"


for path in sorted(inc.glob("opuspp/**/*.hpp")):
    rel = str(path.relative_to(inc / "opuspp"))
    if rel.endswith("celt_tables.hpp"):
        continue
    lines = path.read_text().splitlines()
    spans = loop_spans(lines)
    for start, end in spans:
        # Innermost attribution: skip remark lines that belong to a nested loop.
        inner = [(a, b) for a, b in spans if (a, b) != (start, end) and start <= a and b <= end]
        def mine(line):
            return start <= line <= end and not any(a <= line <= b for a, b in inner)
        gs = set().union(*[v for (f, l), v in g.items() if f == rel and mine(l)] or [set()])
        cs = set().union(*[v for (f, l), v in c.items() if f == rel and mine(l)] or [set()])
        text = lines[start - 1].strip()[:58]
        print(f"{rel}:{start:<4} | gcc: {verdict(gs):<44.44} | clang: {verdict(cs):<44.44} | {text}")
