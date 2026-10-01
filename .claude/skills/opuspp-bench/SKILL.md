---
name: opuspp-bench
description: Benchmark opuspp against libopus and measure decoder code size under the named build profiles (Code size optimised/Balanced/Fast, GCC and Clang, identical flags for libopus and opuspp), and update docs/performance*.md and docs/code-size.md (and the README headline numbers) with the measured numbers. Use when asked for benchmark or size numbers or after performance-relevant changes.
---

# Benchmark and size-measure opuspp vs libopus

Paths below are relative to `opuspp/` (see `CLAUDE.md`, "Layout"). The
general method (fair profiles, program-minus-stub code size, honest
reporting) is in the `cxxport-perf-and-measure` skill.

## The published numbers: build profiles

```sh
tools/fetch_deps.sh                                   # libopus + music clip, once
tools/profile_report.py --work <dir> --shared         # about 15 min: 2 compilers x 3 profiles
```

`tests/bench.cpp` encodes each stream once into a 64-byte aligned packet
arena (`tests/bench_stream.hpp`), and flushes that arena from the caches
(`clflush`) before every timed pass of either decoder; see the
`cxxport-perf-and-measure` skill, "Cache-aware benchmarking".

For each compiler and profile, **one set of flags builds everything**:
libopus (the reference decoder, and the benchmark's encoder), `tests/bench.cpp`
and `tools/opus_wav/decode.cpp` (with opuspp, with libopus, and as a stub).
The script prints the Markdown tables (settings, speed, code size) and saves
`results.json` in `<dir>`.

| Profile | Flags |
|---|---|
| Code size optimised (`size`) | `-Os` |
| Balanced | `-O2` |
| Fast | `-O3 -march=native -ffast-math -falign-loops=32` + FLOAT_APPROX on both sides |

All profiles add `-DNDEBUG -ffunction-sections -fdata-sections`
`-Wl,--gc-sections`, with libopus hardening off.

Useful options:
* `--profiles balanced --compilers gcc`: a subset (profile keys: `size`,
  `balanced`, `fast`).
* `--from-json <dir>/results.json`: re-print the tables without measuring.
* `--reuse-results`: skip benchmarks whose binary is byte-identical to one
  already measured in `<dir>`. This is useful after an interrupted run or
  when adding a profile. Don't use it for numbers after a code change; the
  binaries change then anyway.

Builds are cached in `<dir>`, one directory per configuration hash
(compiler version, flags, options). libopus is built once per
configuration, and the programs are rebuilt only when `include/`, the bench
sources or `tools/opus_wav/` change. All six configurations build in
parallel: about 14 s cold, about 5 s after a header change, and well under
a second when nothing changed. Keep reusing the same `<dir>`.
* `--seconds 30 --passes 3`: quicker, but noisier.
* `--shared`: also benchmark against libopus linked as a shared library
  (`bench_shared`), needed for `docs/performance-shared-library.md`; with
  `--reuse-results`, it adds that to an earlier run.
* `--cases 510`: only some benchmark cases (`128,256,320,510,loss,plc`;
  default `128,320,510,loss`), e.g. for flag sweeps. `plc` is concealment
  only.
* `--no-size` or `--no-bench`.
* `--pp-defines=-DOPUSPP_NO_VECTOR_EXT`: the SIMD ablation (the `=` is
  required).
* `--fast-flags "<flags>"`: try another Fast definition. Fast should stay
  the configuration in which **libopus** is fastest, so the comparison
  doesn't favour opuspp.

Run on an otherwise idle machine. Results vary by about ±2–3 % between
runs.

## Quick checks during development

```sh
cmake --build <build> --target opuspp_bench opuspp_bench_ieee
taskset -c 2 <build>/opuspp_bench_ieee 60 7 [segments] [cases]   # e.g. music; cases 510,plc
```

These CMake targets use the CMake Release flags (and the libopus variants
from `opuspp_reference()`), not the profiles. Don't put their numbers in
the published docs.

## Report

* **Repository-root docs:** the published speed numbers are for the
  320 kb/s stream at encoder complexity 4 (`REPORT_COMPLEXITY`), the
  audience's common case: one bit rate and one encoder complexity, rather
  than totals over streams whose decode cost differs a lot by bit rate. Paste the profile table and the 320 kb/s matrix
  into `docs/performance.md`; the "320 kb/s: each decoder's speed relative
  to its own Code size optimised build" table into
  `docs/performance-build-profiles.md`; the "320 kb/s: libopus statically
  linked against libopus as a shared library" table into
  `docs/performance-shared-library.md`; and the per-stream tables
  (complexity 4, and "Additional results" for complexity 0) into
  `docs/performance-additional.md`. The size table goes into
  `docs/code-size.md` (static and shared-library tables), and the two peak
  stack tables (opuspp and libopus, built with each profile) into
  `docs/memory.md`; check `sizeof(opuspp::Decoder)` there too. Keep the derived
  text in step: the summaries at the top of each doc, the README results
  and `opuspp/README.md`'s speed claim. The SIMD ablation figures in
  `docs/how-claude-made-it.md` change only when the `--pp-defines` run is
  repeated.
* **Notable changes:** call out anything that moved more than about 3 %
  against the previous numbers, and say whether it came from the input or
  the code.
* **Honesty:** state the CPU and the compiler versions. Never round in
  opuspp's favour, and report losses as clearly as wins (for example GCC
  code size optimised).

## Listening check

`<build>/opuspp_encode <dir> 510000 128000` writes the original clip as a WAV
plus `.opk` packet files. `<build>/opuspp_decode <dir>/figaro_510k.opk out.wav`
then decodes one with fast-math opuspp, aligned sample for sample with the
original.
