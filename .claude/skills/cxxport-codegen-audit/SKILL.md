---
name: cxxport-codegen-audit
description: Audit the machine code of a header-only C++ port - per-loop vectorisation verdicts from GCC and Clang, every unaligned vector memory access mapped back to its source line, and a SIMD width census (how much of the code and of the run time uses scalar, 128-, 256- or 512-bit instructions) - then fix findings at the source level without breaking bit-exactness. Ships vec_report.py, align_report.py and width_report.py. Use after adding or changing loops, buffers or SIMD code, or when an alignment check fails.
---

# Code-generation audit: vectorisation and alignment

Two scripts in `scripts/` next to this file (`<skill-dir>` below is this
skill's directory). Both compile one translation
unit that instantiates the library (the freestanding test TU works well) and
attribute compiler output to the headers under `--src`.

## Vectorisation: every loop has a verdict

```sh
python3 <skill-dir>/scripts/vec_report.py \
    --tu tests/freestanding.cpp --src include/<lib> -I include --exclude <generated_tables>.hpp
python3 .../vec_report.py ... -- -march=x86-64-v3          # AVX2
```

Each loop gets `VEC`, `SLP` or the compiler's reason per compiler. A loop
that isn't vectorised is acceptable only for one of these reasons:

* a serial dependency (entropy decoding, recursions, IIR/LPC, rotations);
* it is the scalar head or tail of an explicit SIMD loop;
* it already is explicit SIMD;
* it is too short or cold to matter, with a comment saying so.

Anything else becomes an explicit SIMD loop (`cxxport-portable-simd`). Loops
that the auto-vectoriser would vectorise in a way that changes results
(reductions) get a no-vectorise pragma with a comment.

## Alignment: 0 unaligned vector accesses

```sh
python3 <skill-dir>/scripts/align_report.py \
    --tu tests/freestanding.cpp --src include/<lib> -I include --cxx clang++ -- -O2 -march=x86-64-v3
```

It prints `N unaligned, M aligned`, then each offending `file:line`
(innermost inlined location). Run it for GCC at -O1..-O3, -Os on x86-64
baseline/v2/v3/v4, and Clang at the same levels on baseline/v2/v3, from the
project's verify script. Clang with `-march=x86-64-v4` is the known exception
(it recombines shuffled aligned blocks into unaligned loads).

### Typical causes and fixes

| Finding | Fix |
|---|---|
| A load at `base + i + k` for a runtime `k` | Misalignment dispatch to R = 0..3 plus windows assembled from aligned blocks |
| A buffer or table without `alignas(64)`, or a row stride that isn't a whole number of vectors | Align and pad it |
| The compiler can't prove alignment through a pointer | `__builtin_assume_aligned(p, 64)`; `[[assume(n % 4 == 0)]]` for trip counts |
| Clang AVX merges two aligned 16-byte operations into an unaligned 32-byte one | 64-byte base alignment plus bulk helpers that move whole cache lines as `simd<float, 16>` |
| Struct zeroing merged into overlapping unaligned stores | Keep the struct at a 64-byte multiple size and alignment; zero it as a whole under `__OPTIMIZE__`, with loops otherwise (`cxxport-freestanding`) |
| An auto-vectorised scalar loop with an unaligned peel | An explicit SIMD loop over the aligned part, or a no-vectorise pragma if it's tiny |

Inspect one spot by hand:

```sh
clang++ -std=c++23 -O3 -g -march=x86-64-v3 -ffreestanding -fno-exceptions -fno-rtti -Iinclude \
  -c tests/freestanding.cpp -o /tmp/x.o
objdump -dl --no-show-raw-insn -C /tmp/x.o | grep -B25 "vmovups.*(" | less
```

After any fix, re-run the differential test (`cxxport-bit-exact-oracle`):
alignment work often changes buffer layouts.

## SIMD width: how much of the target's vector width is used

Many C libraries were written in the 4-wide era (SSE, NEON), and a port that
mirrors their kernels inherits that width. On an AVX2 or AVX-512 target this
leaves half to three quarters of each vector unused. A census of the binary
built for that target shows how much:

```sh
python3 <skill-dir>/scripts/width_report.py <binary> --symbols '<namespace>::'
perf record -F 20000 -o w.data <driver> port <parameters> 40
python3 <skill-dir>/scripts/width_report.py <binary> --symbols '<namespace>::' --perf w.data
```

The first counts instructions in the port's functions by class (no SIMD,
scalar floating point, 64-, 128-, 256-, 512-bit); the second weights them by
sampled cycles, which is what matters. Read it as:

* **Mostly 128-bit vector work on a wide target:** element-wise kernels are
  candidates for wider vectors (`cxxport-portable-simd`, "Vector width").
* **256/512-bit instructions that are only moves or integer loops:** the
  compiler widened bulk copies or vectorised integer code by itself; the
  float arithmetic is still narrow.
* **Mostly no-SIMD or scalar time:** the run time is in serial code (entropy
  coding, recursions), and wider vectors can't gain much. The 128-bit share
  of cycles bounds what an 8- or 16-wide rewrite can save: making that share
  twice as fast saves at most half of it. Check this before investing.

Run it per compiler: they widen different loops on their own.

## Notes

* Clang `-Rpass=...`: a later flag replaces an earlier one, so the script
  passes one combined regex.
* The scripts are x86-64 specific in what they count (`movups` and friends);
  on other targets, inspect the disassembly for the target's unaligned forms.
