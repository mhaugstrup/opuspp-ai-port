#!/usr/bin/env python3
"""Lists unaligned vector memory accesses (movups/movdqu/...) per source line.

Compiles tests/freestanding.cpp with debug info and maps every unaligned
vector load/store with a memory operand back to the opuspp source line that
produced it (objdump -l, innermost inlined location). Aligned accesses
(movaps/movdqa or folded memory operands) are counted for comparison.

Requirement: 0 unaligned accesses for GCC (-O1..-O3, -Os; x86-64 baseline,
v2, v3, v4) and Clang (same levels; baseline, v2, v3). tools/verify.sh stage 5
checks this. Clang with AVX-512 is the one accepted exception: it recombines
shuffled aligned blocks into unaligned loads, and preventing that would need
inline asm.

Usage: tools/align_report.py [g++|clang++] [extra flags...]
"""
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

root = Path(__file__).resolve().parent.parent
cxx = sys.argv[1] if len(sys.argv) > 1 else "g++"
extra = sys.argv[2:]
with tempfile.TemporaryDirectory() as tmp:
    obj = Path(tmp) / "fs.o"
    subprocess.run([cxx, "-std=c++23", "-O3", "-g", "-ffreestanding", "-fno-exceptions", "-fno-rtti",
                    "-I" + str(root / "include"), "-c", str(root / "tests/freestanding.cpp"), "-o", str(obj),
                    *extra], check=True)
    dump = subprocess.run(["objdump", "-dl", "--no-show-raw-insn", str(obj)], capture_output=True,
                          text=True, check=True).stdout

unaligned = re.compile(r"\b(v?movups|v?movupd|v?movdqu|v?movdqu8|v?movdqu16|v?movdqu32|v?movdqu64|v?lddqu)\b")
aligned = re.compile(r"\b(v?movaps|v?movapd|v?movdqa|v?movdqa32|v?movdqa64|movntps|movntdq)\b")
# Legacy-SSE packed arithmetic/logic/shuffle with a memory operand requires a
# 16-byte aligned address (it faults otherwise), so it counts as aligned.
packed_mem = re.compile(r"^\s*[0-9a-f]+:\s+(addps|subps|mulps|divps|minps|maxps|andps|andnps|orps|xorps|"
                        r"shufps|unpcklps|unpckhps|cmp\w*ps|paddd|psubd|pmulld|pmuludq|pand|pandn|por|pxor|"
                        r"pshufd|punpck\w+|cvtdq2ps|sqrtps)\s")
src = None
bad = Counter()
good = Counter()
for line in dump.splitlines():
    m = re.match(r"^(/\S+):(\d+)", line)
    if m:
        src = (m.group(1), int(m.group(2)))
        continue
    if "(" not in line or src is None or "include/opuspp" not in src[0]:
        continue  # register-to-register or not ours
    rel = src[0].split("include/opuspp/", 1)[1] + ":" + str(src[1])
    if unaligned.search(line):
        bad[rel] += 1
    elif aligned.search(line) or packed_mem.search(line):
        good[rel] += 1

print(f"{cxx} {' '.join(extra)}: {sum(bad.values())} unaligned, {sum(good.values())} aligned vector memory accesses")
for loc, n in sorted(bad.items(), key=lambda kv: (kv[0].split(':')[0], int(kv[0].split(':')[1]))):
    print(f"  {n:4d} unaligned  {loc}")
