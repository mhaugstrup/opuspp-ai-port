# opuspp: working notes for Claude

A header-only, freestanding C++23 port of the libopus 1.6.1 float decoder, for
one kind of stream only. What it decodes, the rules every change must follow
and the acceptance checks are in `opuspp/README.md` ("What it decodes",
"Changing the code"); read them before changing behaviour.

## Layout

* `opuspp/`: the library, tests, benchmarks and tools. It must stay
  redistributable as is and **must not reference Claude tooling** (no
  `CLAUDE.md`, skill or `docs/` references inside it). **Paths in this file
  and in the `opuspp-*` skills are relative to `opuspp/`** unless they start
  with `docs/` or `.claude/`; run commands from `opuspp/` or prefix them (the
  scripts find their root themselves).
* Repository root: the write-up of the experiment (`README.md`, `docs/`:
  verification, performance with its additional-results, build-profile and
  shared-library pages, code size, memory, how Claude made it) and this
  file.
* `opus-1.6.1/` at the repository root: the reference sources (unmodified
  official tarball, fetched with the benchmark music by
  `tools/fetch_deps.sh`; neither is committed). CMake and the scripts find it
  as `../opus-1.6.1` or via `OPUSPP_LIBOPUS_DIR`.

## Ground rules

* **The bar is bit-exactness.** Every change must keep `tools/verify.sh`
  passing: `--quick` (about 1.5 min) while iterating, the full run (about
  5 min) before calling something done.
* **Skills:** the project ones (`opuspp-verify`, `opuspp-port`,
  `opuspp-codegen-audit`, `opuspp-perf`, `opuspp-bench`) hold the opuspp
  commands; the general method and the pitfalls behind them are in the
  generic `cxxport-*` skills next to them (`cxxport-workflow` is the entry
  point), which are meant to move to a repository of their own later.
* **Report numbers as measured.** The speed, size and stack numbers in
  `README.md`, `docs/performance*.md`, `docs/code-size.md` and
  `docs/memory.md` come only from `tools/profile_report.py --shared`
  (pinned, best of 7; the three build profiles everywhere; speed for the
  320 kb/s stream at encoder complexity 4, the audience's common case, with
  other streams only in `docs/performance-additional.md`). Those docs hold
  current results only; history and learnings go in
  `docs/how-claude-made-it.md`.

## Code conventions

* 3-space indent. Each header starts with a comment naming its libopus origin
  plus the BSD-3-Clause copyright notice of the sources it ports.
* Constants live in `detail/config.hpp` (`frame_size`, `overlap`,
  `nb_ebands`, `band_stride`, `buffer_alignment`, ...). Don't repeat literals.
* Buffers and tables are `alignas(64)`; per-channel arrays are padded
  (`band_stride`, `decode_mem_stride`). Tell the compiler with
  `assume_aligned` and `[[assume(...)]]`.
* Loops that must stay scalar get `OPUSPP_NO_VECTORIZE` plus a comment saying
  why.
* Tables are generated (`tools/gen_tables.py`) or built by `constexpr`
  lambdas, never hand-copied; constant-evaluation failures call
  `table_generation_failed()`.

## opuspp pitfalls already paid for

The general ones (sign of zero, hidden `memset`/`memcpy`, the `sqrtf`
fallback, merged stores, `-Rpass` flags, AVX2 dispatch in the reference) are
in the `cxxport-*` skills and marked in the code where they bite. Specific to
this tree:

* `CeltDecoder::State::clear()` is the pattern for zeroing state without a
  `memset` call at -O0.
* `celt_sqrt` uses `__builtin_ia32_sqrtss` for GCC -O0/-Os on x86, and the
  slow software `sqrtf` on other targets at those levels.
* GCC `-Wmaybe-uninitialized` in the simd broadcast: `native_type{} + x`.
  The `decimate` padding loop needs the destination size passed in
  (`-Warray-bounds`). Helper names (`fill`, `window`, `c1`) were shadowed by
  parameters: keep `-Wshadow`.
* `isqrt32(0)` is undefined; exhaustive tests start at 1.
* libopus's speed depends on the compiler, in both directions: at 510 kb/s,
  Clang's build is 7–11 % slower than GCC's at -O2/-O3 but 22 % faster at
  -Os. Benchmarks compare per compiler, never across.
* Dead ends: a branchless `bits2pulses` (use the LUT), software
  `sqrtf`/`exp` (use the hardware sqrt), a binary search in `cwrsi` (use the
  windowed count in `last_le`), the window for `cwrsi`'s column scans, a
  two-sided branchless theta decode, and a branchless `isqrt_small`
  correction. Details in the `opuspp-perf` skill.
* Removing `State` fields regrouped the ones `decode()` writes together, and
  Clang merged them into an unaligned 16-byte store; they now start the
  cache line. Passing `RangeDecoder` by value made Clang -O0 call `memcpy`
  (`uses_postfilter` builds its own decoder instead).
* After a branchless or unrolled rewrite, grep the disassembly of every build
  profile for `gather` and packed compares: Clang with AVX2/AVX-512 turned
  `last_le`'s unrolled compares into a gather.

## Environment

* Scratch builds go outside the source tree. `build*/` directories and the
  `*.wav` / `*.opk` files from `opuspp_encode` / `opuspp_decode` are not
  source.
