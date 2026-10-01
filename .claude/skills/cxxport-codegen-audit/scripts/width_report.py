#!/usr/bin/env python3
"""SIMD width census of a port's code in a binary (x86-64).

Classifies every instruction in the functions whose (demangled) name matches
--symbols by the width it operates on: no SIMD, scalar floating point (one
lane in an xmm register), 64-bit, 128-bit, 256-bit and 512-bit. Many C
libraries were written for 4-wide SSE/NEON; this shows how much of a port's
vector work actually uses the width the target offers.

Static (default): counts instructions in the binary.
Dynamic (--perf perf.data): weights each instruction by the cycles sampled on
it, from `perf record -F 20000 <binary> ...`. Sample skid can attribute a
sample to a neighbouring instruction, so read the shares as approximate.

Usage:
  width_report.py <binary> --symbols 'mylib::' [--perf perf.data] [--top 10]
"""
import argparse
import re
import subprocess
from collections import Counter, defaultdict

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("binary")
ap.add_argument("--symbols", required=True, help="regex on demangled function names, e.g. 'mylib::'")
ap.add_argument("--perf", help="perf.data recorded on this binary: weight by sampled cycles")
ap.add_argument("--top", type=int, default=10, help="functions to list by wide-instruction share")
a = ap.parse_args()
want = re.compile(a.symbols)

ORDER = ["no SIMD", "scalar (1 lane)", "64-bit", "128-bit", "256-bit", "512-bit"]
SCALAR_OPS = {"movd", "vmovd", "vpinsrd", "vpextrd", "vextractps", "vinsertps", "vbroadcastss",
              "vpbroadcastd", "pinsrd", "pextrd", "insertps", "extractps"}
HALF_OPS = {"movq", "vmovq", "movlps", "vmovlps", "movhps", "vmovhps", "movlpd", "vmovlpd", "movhpd", "vmovhpd"}


def classify(text):
    op = text.split()[0]
    if "zmm" in text:
        return "512-bit"
    if "ymm" in text:
        return "256-bit"
    if "xmm" not in text:
        return "no SIMD"
    if re.search(r"(ss|sd)$", op) or re.search(r"si2s[sd]$|s[sd]2si$", op) or op in SCALAR_OPS:
        return "scalar (1 lane)"
    if op in HALF_OPS:
        return "64-bit"
    return "128-bit"


# Instructions by (function, offset) from the disassembly.
dump = subprocess.run(["objdump", "-d", "--no-show-raw-insn", "-C", a.binary],
                      capture_output=True, text=True, check=True).stdout
insn = {}
fn, base = None, 0
for line in dump.splitlines():
    m = re.match(r"^([0-9a-f]+) <(.*)>:$", line)
    if m:
        base, fn = int(m.group(1), 16), m.group(2)
        continue
    m = re.match(r"^\s+([0-9a-f]+):\s+(\S.*)$", line)
    if m and fn and want.search(fn):
        text = m.group(2)
        if not text.startswith(("nop", "int3", "data16", "cs nop", "xchg   %ax,%ax")):
            insn[(fn, int(m.group(1), 16) - base)] = text

weights = Counter()
if a.perf:
    out = subprocess.run(["perf", "script", "-i", a.perf, "-F", "ip,sym,symoff"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"^\s*[0-9a-f]+\s+(.*)\+0x([0-9a-f]+)\s*$", line)
        if m:
            key = (m.group(1).strip(), int(m.group(2), 16))
            if key in insn:
                weights[key] += 1
    unit = "sampled cycles"
else:
    weights = Counter({k: 1 for k in insn})
    unit = "instructions"

total = Counter()
per_fn = defaultdict(Counter)
for key, w in weights.items():
    c = classify(insn[key])
    total[c] += w
    per_fn[re.sub(r"\(.*", "", key[0])][c] += w

n = sum(total.values())
simd = n - total["no SIMD"]
print(f"{a.binary}: {n} {unit} in functions matching '{a.symbols}'")
print(f"  {'class':18s} {'share':>7s} {'of SIMD':>8s}")
for k in ORDER:
    of_simd = f"{100 * total[k] / simd:7.1f}%" if simd and k != "no SIMD" else ""
    print(f"  {k:18s} {100 * total[k] / max(1, n):6.1f}% {of_simd:>8s}")
print(f"\nFunctions by {unit} (wide = 256/512-bit):")
for name, c in sorted(per_fn.items(), key=lambda kv: -sum(kv[1].values()))[:a.top]:
    s = sum(c.values())
    print(f"  {s:7d}  no SIMD {100 * c['no SIMD'] / s:5.1f}%  128 {100 * c['128-bit'] / s:5.1f}%  "
          f"wide {100 * (c['256-bit'] + c['512-bit']) / s:5.1f}%  {name[:70]}")
