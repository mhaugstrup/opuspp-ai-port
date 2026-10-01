---
name: cxxport-workflow
description: Overall method for porting a versatile audio C library (codec, DSP, resampler) to a header-only, freestanding C++ implementation specialised for one specific use case, bit-exact with the original and faster. Phases with gates, the project files to set up first, and the rules that keep multi-session work on track. Use at the start of such a port, when resuming one after a context compaction, or when deciding what to do next. Points to the other cxxport-* skills for each phase.
---

# Porting a versatile C audio library to a specialised C++ implementation

General-purpose C libraries cover every mode, rate and configuration their
users might need. A product usually needs one. This method ports such a
library to a header-only, freestanding C++ implementation for that one use
case, keeping the output bit-identical to the original while making it
faster. It applies to any library whose output can be compared sample for
sample with a reference: codecs, filters, resamplers, FFT-based effects. It
was developed porting a codec decoder.

## Before writing code: make the ground truth explicit

Long ports span many sessions and context compactions. Everything a later
session needs must be in files, not in the conversation:

| File | Holds |
|---|---|
| A requirements doc | Every decision the user made, with the reason where it isn't obvious: the use case, what is out of scope, the correctness bar, constraints, pinned inputs (reference version + hash, test media + hash), acceptance criteria |
| `CLAUDE.md` | Conventions and a **"pitfalls already paid for"** list. Add to it every time a bug costs more than a few minutes |
| One verify script | The judge: a single command that prints PASS or the first failing stage (see `cxxport-bit-exact-oracle`) |
| Project skills | Thin, project-specific wrappers (how to run verify, the benchmark profiles) over these generic skills |

Ask the user about anything the requirements doc doesn't settle; once
answered, write it down so it is never asked again.

## Phases and gates

Run the phases in order. Each ends with a **gate** that must pass before the
next starts. Most rework comes from changing several things at once, so
change one thing, re-run the gate, then continue.

| # | Phase | Gate | Skill |
|---|---|---|---|
| 0 | **Scope.** Build the reference. Measure what the real producer (encoder, upstream stage) actually emits for the target use case | The measured input space matches the assumed one; if not, stop and ask | `cxxport-scope-and-port` |
| 1 | **Harness first.** Differential test and fuzzer against the public API with a stub implementation; freestanding math; table generator | Harness builds and runs; math matches libm bitwise over millions of arguments | `cxxport-bit-exact-oracle`, `cxxport-freestanding` |
| 2 | **Scalar port, main path**, in dependency order, plain loops | Bit-exact output and internal checksums on every loss-free configuration | `cxxport-scope-and-port` |
| 3 | **Edge paths:** error handling (for a decoder: concealment), empty and malformed input | Full differential suite and fuzz pass, also under ASan+UBSan | `cxxport-bit-exact-oracle` |
| 4 | **Freestanding hygiene** | `nm -u` empty for every compiler × optimisation level, warnings as errors | `cxxport-freestanding` |
| 5 | **SIMD**, one kernel at a time, re-running the differential test after each | Bit-exact with both SIMD backends and on every ISA level | `cxxport-portable-simd` |
| 6 | **Alignment and code-generation audit** | 0 unaligned vector accesses; every scalar loop justified | `cxxport-codegen-audit` |
| 7 | **Float modes** (fast-math / approximation variants), if the original has them | Each mode bit-exact against the matching reference build | `cxxport-bit-exact-oracle` |
| 8 | **Profile and optimise**, then **measure fairly** | Verify still passes; speed-ups measured with identical flags on both sides | `cxxport-perf-and-measure` |
| 9 | **Document** | Docs hold measured numbers only, losses next to wins | `cxxport-perf-and-measure` |

Never vectorise or optimise before the scalar port is bit-exact: once SIMD is
in, a mismatch can come from anywhere.

## Rules that held up

* **Oracle before code.** Write (or freeze) the differential test before the
  implementation it judges, and don't weaken it to make code pass. If the
  oracle itself is wrong, stop and tell the user.
* **The verify script decides "done",** not a reading of the code. Run the
  quick mode while iterating and the full mode before reporting.
* **Resume by re-running the last gate.** After a compaction or a restart,
  run verify first, then continue from the phase whose gate fails.
* **Report as measured.** Say plainly when a check was skipped or failed;
  never round in the port's favour.
* **Toolchain drift.** A newer compiler can change generated code: the
  bit-exactness checks are robust to that, alignment results and speed-ups
  are not. Document differences rather than hiding them.
* **Text edits by script:** assert the old text exists (exactly once) before
  replacing it. A blind `sed` once produced `std::int32_tband_widths`.
* **Shell state:** `cd` doesn't persist between tool calls; use absolute
  paths. Scratch builds go outside the source tree.
