---
name: opuspp-perf
description: Profile and optimise the opuspp decoder with Linux perf (primary target 510 kb/s), keeping output bit-exact. Use when asked to make opuspp faster, or when an opuspp benchmark regresses.
---

# Profile and optimise opuspp

Paths below are relative to `opuspp/`. The method, the rules (bit-exact,
prove replacements exact, keep only measured wins) and the lessons are in the
`cxxport-perf-and-measure` skill.

```sh
cmake --build <build> --target opuspp_profile
perf record -g -o /tmp/pp.data  <build>/opuspp_profile pp  510000 40 [segments]
perf record -g -o /tmp/ref.data <build>/opuspp_profile ref 510000 40 [segments]
perf report -i /tmp/pp.data --no-children --percent-limit 1
perf stat -e cycles,instructions <build>/opuspp_profile pp 510000 40
```

For the concealment path, profile the benchmark's concealment-only case:
`perf record -g <build>/opuspp_bench_ieee 20 3 music plc`.

Cycles per frame = cycles ÷ (passes × 2000). Reference point at 510 kb/s:
about 95k for opuspp against 114k for libopus.

Already done at 510 kb/s, where serial PVQ decoding dominates: the
`bits2pulses` LUT (23 × 258, exact by construction), `isqrt_small` (exact on
[1, 2^24)), and `OPUSPP_INLINE` on `quant_band`. Already tried without gain: a
branchless `bits2pulses`, and software `sqrtf`/`exp`.

Decode-only cost per frame, net of the encoding the driver does first: run
it with 40 passes and with 0, subtract, divide by 80,000 frames; best of
three, for both compilers:

```sh
perf stat -x, -e cycles,branch-misses taskset -c 2 <driver> pp 510000 40
perf stat -x, -e cycles,branch-misses taskset -c 2 <driver> pp 510000 0
perf record -e branch-misses -c 2000 <driver> pp 510000 40   # where they miss
```

Round two at 510 kb/s (-O2, before: GCC 97.1k / Clang 95.6k cycles and
about 830 mispredictions per frame; `cwrsi` had 37 % of them):

* Kept: `last_le()` in `cwrsi` replaces the row scan
  `for (p = row[k]; p > i; p = row[k]) k--` by counting entries > i in a
  fully unrolled window of 8 (independent loads, arithmetic compare).
  −2.6 to −4.4 % per frame at 128 and 510 kb/s, both compilers. Exact:
  checked at every table boundary (`row[k'] - 1/0/+1`).
* The window must stay scalar: unrolled, Clang with AVX2/AVX-512 turned it
  into a `vpgatherqd` (Clang Fast fell from 1.26× to 1.13× overall), and a
  contiguous variant became SSE unsigned compares with unaligned loads at
  -O2. It is now unrolled except under Clang with `__AVX2__`, where it stays
  a rolled `OPUSPP_NO_VECTORIZE` loop.
* Tried and dropped: a binary search (Clang compiled the select as a branch;
  with a fixed 8 steps, the dependent loads made it slower than the scan);
  windows of 4 and 16; the same window for the column scans in `cwrsi`
  (usually one step and well predicted, so slower at 128 kb/s); evaluating
  both sides of the triangular theta pdf (slower, no fewer mispredictions);
  a branchless `isqrt_small` correction (its branches are never taken).
* Round three, no gain: summing `cwrsi`'s norm as an integer (exact, as the
  partial sums are integers below 128^2, but not on the critical path), and
  interleaving `exp_rotation` over short blocks (93 % of the rotated samples
  at 128 kb/s, and all at 320 kb/s, are in the serial `stride == 1` case).
* Precompute candidates with a limited input set, checked and not taken:
  `celt_rsqrt` of the pulse norm (an integer up to 128^2; 3.6 % of the time
  at 510 kb/s, but 76 % of the norms there are above 4096, so only a 64 KB
  table would help), `bitexact_cos` of the quantised angle (32 KB, about
  2 %), and `exp_rotation`'s cosines (barely visible in the profile).
  Everything the stream contract fixes is already `constexpr`; `LM` stays a
  run-time value inside the band recursion because splits and the
  time-frequency resolution change it.
* Encoder settings (counted per decoder path at 128 kb/s): complexity 0
  disables transients, tf changes and spreading (−16 % decode time for both
  decoders); forced mono −18 %; prediction off or a loss hint make every
  coded intra flag 1. The ratio to libopus stays 1.46–1.49× throughout.
* The post-filter: complexity < 5 usually keeps it off, but libopus 1.6
  enables the pre-filter for strongly tonal input at any complexity; only
  prediction disabled guarantees it. Hence the stream contract (complexity
  0–4, prediction disabled): the comb filter and post-filter state are gone,
  and post-filter packets are rejected (`uses_postfilter`).
* Inter-frame energy prediction stays, even with prediction disabled:
  silence frames don't code the intra flag, so they decode with intra = 0,
  and the prediction is what decays the band energies (`compare`, 47
  configurations: 80,226 coded flags, all intra; 7,923 silence frames with
  prediction). Rejecting a coded intra = 0 would only free the 42-byte
  inter probability model, so it isn't done. Counting the flag in a
  scratch copy of the decoder needs the AVX2-off reference, or concealment
  differences show up as failures.
* Left as is: the range decoder's divisions (inherent), `celt_rsqrt`
  latency in `alg_unquant`, the square root in the theta decode, and
  `exp_rotation`'s serial recurrence. Remaining headroom is mainly wider
  vector code (IMDCT/FFT) for AVX targets, at most a few percent and only
  with `-march=native`.

After every optimisation: `opuspp-verify`, then measure with `opuspp-bench`.
