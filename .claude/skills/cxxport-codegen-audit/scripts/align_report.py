#!/usr/bin/env python3
"""Lists unaligned vector memory accesses (movups/movdqu/...) per source line.

Compiles one translation unit with debug info for x86-64, disassembles it and
maps every vector load/store with a memory operand back to the innermost
inlined source line under --src (objdump -l). Unaligned forms are listed;
aligned forms (movaps/movdqa/..., and legacy-SSE packed instructions with a
memory operand, which fault when misaligned) are counted for comparison.

The first output line is "<cxx> <flags>: N unaligned, M aligned vector memory
accesses", so a verify script can parse it.

Usage:
  align_report.py --tu tests/freestanding.cpp --src include/mylib -I include \
                  [--cxx clang++] [-- -O2 -march=x86-64-v3]
"""
import argparse
import re
import subprocess
import tempfile
from collections import Counter
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--tu", required=True)
ap.add_argument("--src", required=True, help="directory whose source lines are attributed")
ap.add_argument("-I", dest="incs", action="append", default=[])
ap.add_argument("--cxx", default="g++")
ap.add_argument("--std", default="c++23")
ap.add_argument("flags", nargs="*", help="extra compiler flags (after --); the default level is -O3")
a = ap.parse_args()

src = Path(a.src).resolve()
with tempfile.TemporaryDirectory() as tmp:
    obj = Path(tmp) / "tu.o"
    subprocess.run([a.cxx, f"-std={a.std}", "-O3", "-g", "-ffreestanding", "-fno-exceptions", "-fno-rtti",
                    *[f"-I{Path(i).resolve()}" for i in a.incs], "-c", str(Path(a.tu).resolve()),
                    "-o", str(obj), *a.flags], check=True)
    dump = subprocess.run(["objdump", "-dl", "--no-show-raw-insn", str(obj)], capture_output=True,
                          text=True, check=True).stdout

unaligned = re.compile(r"\b(v?movups|v?movupd|v?movdqu|v?movdqu8|v?movdqu16|v?movdqu32|v?movdqu64|v?lddqu)\b")
aligned = re.compile(r"\b(v?movaps|v?movapd|v?movdqa|v?movdqa32|v?movdqa64|movntps|movntdq)\b")
packed_mem = re.compile(r"^\s*[0-9a-f]+:\s+(addps|subps|mulps|divps|minps|maxps|andps|andnps|orps|xorps|"
                        r"shufps|unpcklps|unpckhps|cmp\w*ps|paddd|psubd|pmulld|pmuludq|pand|pandn|por|pxor|"
                        r"pshufd|punpck\w+|cvtdq2ps|sqrtps)\s")
cur = None
bad, good = Counter(), Counter()
for line in dump.splitlines():
    m = re.match(r"^(/\S+):(\d+)", line)
    if m:
        try:
            cur = f"{Path(m.group(1)).resolve().relative_to(src)}:{m.group(2)}"
        except ValueError:
            cur = None
        continue
    if "(" not in line or cur is None:
        continue  # register-to-register, or not our source
    if unaligned.search(line):
        bad[cur] += 1
    elif aligned.search(line) or packed_mem.search(line):
        good[cur] += 1

print(f"{a.cxx} {' '.join(a.flags)}: {sum(bad.values())} unaligned, {sum(good.values())} aligned vector memory accesses")
for where, n in sorted(bad.items(), key=lambda kv: (kv[0].rsplit(':', 1)[0], int(kv[0].rsplit(':', 1)[1]))):
    print(f"  {n:4d} unaligned  {where}")
