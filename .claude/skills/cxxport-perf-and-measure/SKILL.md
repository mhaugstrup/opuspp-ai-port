---
name: cxxport-perf-and-measure
description: Profile and optimise a bit-exact, use-case-specific C++ port of a versatile audio C library with Linux perf, and measure its speed, code size and memory against the reference library fairly - identical compiler flags on both sides per named build profile, cache-aware pinned best-of-N runs, code size as program-minus-stub, run-time stack measurement, a SIMD ablation, and honest reporting of losses. Use when making a port faster, benchmarking it, or publishing numbers.
---

# Optimise, then measure fairly

Throughout, "the port" is the C++ code, "the reference" is the original C
library, and "the workload" is whatever both process: encoded packets for a
decoder, PCM for an encoder or filter, and so on.

## Profile

* Build a small driver that prepares the input once and then runs the same
  workload many times, selectable between the port and the reference
  (`driver port|ref <parameters> <passes>`), and profile the **slowest
  realistic case** (for a codec: usually the highest bit rate).

```sh
perf record -g -o /tmp/port.data <driver> port <parameters> 40
perf record -g -o /tmp/ref.data  <driver> ref  <parameters> 40
perf report -i /tmp/port.data --no-children --percent-limit 1
perf stat -e cycles,instructions <driver> port <parameters> 40
```

* Compare port and reference per function. Most of the port is inlined, so
  use `perf annotate` or `--sort sym,srcline`. Quote **cycles per processed
  block** (frame, buffer), not just totals.
* Once the vector work is fast, serial integer work that the reference pays
  too (entropy coding, combinatorial coding, divisions) often dominates.
  What tends to work: compile-time lookup tables that replace chains of
  dependent loads, a hardware-sqrt integer square root with an integer
  correction instead of a bit-by-bit loop, and force-inlining hot functions
  whose many arguments otherwise go through the stack.
* What tends not to: branchless rewrites when the cost is a load latency
  chain rather than branches, and software `sqrt`/`exp` (use hardware).

## Serial integer code: branches and latency

When the profile is dominated by scalar integer code (in a codec: entropy
decoding, combinatorial index decoding, bit allocation), wider SIMD won't
help; mispredicted branches and dependent-load latency are the levers.

* **Measure per processed block, net of setup:** run the driver once with
  the real number of passes and once with zero, subtract, and divide by the
  block count. Count `cycles` and `branch-misses` with `perf stat`, best of
  three; at roughly 15 cycles each, the mispredictions per block show what
  branches cost.
* **Find the mispredicted branches:** `perf record -e branch-misses` and
  `perf annotate -l` per function. Samples skid to the instruction after the
  branch.
* **A data-dependent loop exit** (a search that scans until a value is
  found) is a classic misprediction source. Options, in order of what
  usually works:
  * If the data is monotonic, **count instead of scan:** compare a fixed
    window of entries against the key with independent loads and add up the
    results; that count is the number of scan steps. Fully unroll the window
    (as a loop, its own exit mispredicts), loop only when the whole window
    matched, and pick the width by measurement.
  * A branchless binary search removes the mispredictions but makes every
    step a dependent load; it is often slower than the scan it replaces.
  * Write selects as arithmetic (`x += d & -int(cond)`): a compiler may turn
    `cond ? a : b` back into a branch, and did.
* **Leave well-predicted branches alone:** short scans that usually take one
  step, and corrections that are almost never taken. Evaluating both sides
  of a branch to avoid it costs real work and can be slower.
* **Confirm with the full benchmark.** Quick per-block measurements on one
  or two cases guide the work, but the effect of a change can be much larger
  (or smaller, or reversed) on cases they don't cover, such as other bit
  rates or build profiles.
* **Measure every change with both compilers and in every build profile,**
  not just the one used while developing; the same rewrite has gone opposite
  ways in GCC and Clang, and between -O2 and `-march=native`.
* **Check the generated code of a scalar rewrite in every profile.** A few
  independent compares, once unrolled, are fair game for the straight-line
  (SLP) vectoriser: it can turn them into a gather on AVX2/AVX-512 (slow on
  some CPUs) or into packed unsigned compares plus a horizontal sum, and
  `#pragma ... vectorize(disable)` doesn't stop SLP. Keeping the loop rolled
  and non-vectorised does. Grep the disassembly for `gather`, packed
  compares and unaligned moves after such a change; choosing the form per
  compiler and target with predefined macros is fine if every path stays
  correct.
* **Check that a chain is on the critical path before removing it.** An
  exact rewrite of a serial accumulation (for example summing small integer
  squares as `int` instead of `float`, which is exact below 2^24) gains
  nothing when other work in the same loop takes longer.
* **Count before optimising a special case.** Instrument a scratch copy to
  see how the work divides between cases (stride, block count, branch);
  a rewrite that only helps a case with 1 % of the work isn't worth it.
* **Latency-bound recurrences** where each step needs the previous result
  can't be sped up exactly. Where independent chains already interleave in
  program order, the out-of-order core overlaps them without help.
* **Prove the rewrite exact:** for a search over monotonic data, testing
  every key at and around every table boundary covers every possible
  outcome.

## Rules for optimisations

* **Bit-exact or it doesn't count:** run the verify script after each one.
* **Prove replacements exact** over the whole input domain
  (`cxxport-bit-exact-oracle`).
* **Keep only measured wins.** Report before and after; drop changes within
  noise (about ±1–3 %).

## Measure fairly

* **Named build profiles, identical flags for both sides.** For each compiler
  and profile, one set of flags builds the reference library, the benchmark
  and the size program, for example:
  * *Code size optimised:* `-Os`;
  * *Balanced:* `-O2`;
  * *Fast:* `-O3 -march=native -ffast-math` plus the code alignment settled
    below, with the reference's own fast/approximate float options, chosen
    as the
    configuration where **the reference** is fastest, so the comparison
    doesn't favour the port;
  * all with `-DNDEBUG -ffunction-sections -fdata-sections -Wl,--gc-sections`
    and the reference's hardening options off.
* **Settle code alignment the same way:** sweep `-falign-loops` ×
  `-falign-functions` on one representative case (full rebuild of both
  sides per variant), recheck the best few at best of 25, and take the
  setting where the reference is fastest.
* **Compare per compiler.** The same reference library can be considerably
  faster with one compiler than another on its heaviest workload, so mixing
  compilers would distort the ratio.
* **Link the reference statically** for the speed comparison, and check once
  that a shared build of it performs on par (differences of a percent or two
  are code layout, not linking). The benchmark is then one self-contained
  binary per configuration.
* **Representative workloads:** several signal types (tones, noise,
  transients, real music with a wide stereo image, fetched and hash-checked,
  not committed), the parameter range the use case needs (rates,
  complexities, channel modes), and the error paths it exercises (for a
  decoder: packet loss). Keep narrow cases available as options for
  profiling.
* **Pin and repeat:** one core other than core 0 (`taskset`), an idle
  machine, best of 7. The core's SMT sibling isn't reserved unless you also
  keep it free.

## Cache-aware benchmarking

Port and reference must see the same memory conditions, or the comparison
measures the caches instead of the code:

* **Prepare the input once, outside the timed region,** and run the
  identical input through both sides.
* **Lay out inputs and outputs the same way for both sides.** Put the input
  in one contiguous arena of 64-byte blocks, each item (packet, buffer)
  starting on a block boundary, and give the reference output buffers with
  the same alignment as the port's. Otherwise allocator placement (`malloc`
  gives 16 bytes) varies between runs and between the two sides.
* **Flush the input from the caches before every timed pass,** for both
  sides: `_mm_clflush` on every cache line of the input arena plus
  `_mm_mfence` on x86 (elsewhere, sweep a buffer larger than the last-level
  cache), outside the timed region. Inputs of a few megabytes fit in a large
  L3, so without the flush every pass after the first, and whichever side
  runs second, would read its input from cache.
* **Reset the processing state at the start of each pass** so every pass
  does identical work.
* **One binary or two:** with port and reference in one benchmark binary,
  each one's code placement depends on the other's. That is acceptable when
  the inputs are flushed and layout sensitivity is checked (next point);
  separate single-implementation programs remove the coupling at the cost of
  a file-based pipeline.
* **Check layout sensitivity:** rebuild the same code with different
  `-falign-functions` / `-falign-loops` values and compare. If results move
  by more than the run-to-run noise, report the range, not the luckiest
  build.
* **Cache builds** keyed by a hash of the full configuration (compiler
  version, flags, options), rebuild only what changed, and save raw results
  as JSON so tables can be re-rendered.
* **SIMD ablation:** rebuild the port with its plain-array backend to show
  what the explicit SIMD contributes.

## Code size and memory

* **Code size = program − stub.** Build one small program three ways: with
  the port, with the reference library, and as a stub without either.
  Text + data minus the stub's cancels out the harness and the C++ runtime;
  with `--gc-sections`, only reachable code counts.
* **Explain the trade:** what the specialisation drops (other modes) and what
  speed costs (kernel instantiations per alignment, unrolled stages,
  precomputed tables). Report builds where the port is larger.
* **A shared reference library** can't drop unused code (the whole library
  is loaded, including parts the use case never calls), so measure it
  separately from the static case.
* **Memory:** state size (`sizeof` vs the reference's size query), heap use,
  peak stack per code path (the common path and the heaviest rare path, such
  as error handling or concealment), and the output buffers.
* **Measure stack at run time,** not from `-fstack-usage` alone (inlining
  merges frames): run each side on a thread whose stack is painted with a
  pattern (`pthread_attr_setstack`), find the deepest overwritten byte, and
  subtract an empty run (thread start-up, about 6 KB on glibc).

## Keeping the stack small

C references often use variable-length arrays or `alloca`, which exist only
from their declaration on. A port with fixed-size `alignas(64)` arrays gets
a frame that holds **every** local array of the function, inlined helpers
included, for its whole duration, so a rare path can need several KB more
stack than in the reference until it is restructured:

* **Split paths that never run together into out-of-line functions** (the
  common path vs a rare path; successive phases of one algorithm), so their
  arrays don't share a frame. One call per processed block costs nothing
  measurable.
* **Outline helpers with large scratch** that run once per block.
* **Reuse dead buffers as scratch:** a caller-provided output buffer is dead
  until the final write (a decoder can build its intermediate spectrum in
  it), and a buffer used later in a function can serve an earlier helper.
* Keep restrict-qualified pointers honest: a scratch pointer derived from a
  restrict output pointer must not itself be declared restrict if the output
  pointer is used later in the same scope.

## Report

* Measured numbers only; state the CPU and compiler versions.
* Never round in the port's favour; report losses as clearly as wins.
* **Lead with the audience's common case.** Publish one representative
  configuration as the headline result, with every build profile, and put
  the other configurations on a separate page as ranges. Totals over several
  configurations mix workloads of different weight and are hard to
  interpret.
* **Use the build profiles everywhere.** Speed, code size and memory (stack
  included) all come from the same named profiles and the same builds, so
  the numbers in every document can be compared with each other.
* Call out anything that moved more than about 3 % since the last run, and
  say whether it came from the input or the code.
