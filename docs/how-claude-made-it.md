# How Claude made it

The story behind opuspp: who decided what, the working method, the
decisions, the optimisations with code, the measurement details behind the
published numbers, and the lessons. The results themselves are in
[performance.md](performance.md) (with its pages on
[additional results](performance-additional.md),
[build profiles](performance-build-profiles.md) and
[a shared libopus](performance-shared-library.md)),
[code-size.md](code-size.md), [memory.md](memory.md) and
[verification.md](verification.md).

## Who did what

* **The human** stated the requirements: C++23, freestanding, header-only,
  no heap or asm, bit-exact, stereo fp32, 10 ms, packet loss concealment but
  no FEC, aligned SIMD, and the one stream it must handle. They answered
  Claude's design questions, asked for audits and benchmarks, and decided
  what to keep.
* **Claude** read the libopus 1.6.1 sources and wrote everything in this
  repository: the decoder port, the `simd<T,N>` layer and the vectorised
  kernels, the table generator, the differential and fuzz tests, the code
  audit tools, the benchmarks, the optimisations and the documentation.

## The working method

The work spanned more than one Claude Code session. What kept it on track was
making every requirement, convention and check explicit in files, so that
each new session started from the same ground truth:

| Piece | Role |
|---|---|
| A requirements spec | **What**: every requirement and decision, with pinned inputs and acceptance criteria |
| A phased plan | **In what order**: phases with gates, so correctness was locked in before SIMD and performance work |
| [`CLAUDE.md`](../CLAUDE.md) | **How**: conventions, plus the pitfalls already paid for, loaded automatically every session |
| `.claude/skills/` | **Procedures**: port a module, verify, audit generated code, profile, benchmark |
| `opuspp/tools/verify.sh` | **The judge**: one command that decides "done" |

The spec's surviving rules now live in `opuspp/README.md` and the tools; the
plan's lessons went into the reusable skills ([below](#reusing-the-lessons)).

The phases, each closed by a gate:

1. **Port for correctness first.** Each libopus module was ported keeping
   the exact float evaluation order (same operands, temporaries and
   summation tree), and checked against `opus_decode_float` on every packet.
2. **Then vectorise.** The SIMD kernels mirror the libopus SSE/NEON lane
   layout, so every lane computes the identical scalar expression.
3. **Then audit the machine code** for loops that weren't vectorised and for
   unaligned vector accesses, and fix the findings at the source level.
4. **Then profile** with `perf` and optimise the hot spots.
5. **Measure honestly:** identical flags for libopus and opuspp per compiler
   and profile, pinned, best of 7, losses reported next to wins.

The code is not to be taken on trust because of who wrote it: everything it
claims is backed by checks anyone can rerun with `opuspp/tools/verify.sh`
(see [verification.md](verification.md)).

## Decisions

The design reference is in [opuspp/README.md](../opuspp/README.md#design).
The decisions that shaped it, several of them answers to Claude's questions:

* **Specialise to one stream.** Everything the stream makes constant is
  hardcoded: the 48 kHz CELT mode, LM = 2, two channels, only the 240- and
  60-point FFTs, and only the tables those paths touch.
* **Bit-exact, not just close.** The stated minimum was output within the
  tolerance of the Opus test vectors. Bit-identical output and final range
  turned out to be reachable, and became the bar for every later change.
* **An own SIMD class.** Of the options Claude offered (raw compiler vector
  extensions, a wrapper around intrinsics, or an own `simd<T,N>` type), the
  choice was an own class: portable to SSE, AVX, NEON or RVV, with a
  plain-array fallback that gives identical results.
* **Aligned means every vector access.** An early answer mentioned `movaps`
  as an example; it was clarified that every vector memory access must use
  an alignment-expecting form, whatever the instruction.
* **Freestanding:** `exp`, `cos` and `sqrt` are implemented in the headers
  and match libm's float results.
* **No hidden dependency on the CPU.** When the reference libopus turned out
  to conceal differently on CPUs with AVX2 (run-time kernel dispatch), the
  reference was pinned to its SSE baseline rather than the comparison being
  loosened.
* **A narrower stream contract.** The product's encoder runs at complexity
  0–4 with prediction disabled, which codes the band energies of every coded
  frame without reference to the previous frame, for clean recovery after a
  lost packet. Silence frames still decay the previous energies, so the
  decoder keeps its inter-frame energy prediction. Prediction disabled also
  rules out the pitch post-filter, so opuspp dropped it (about 9 KB of code,
  its state and its decoding) and rejects packets that enable it before any
  state changes. Reading the encoder source first suggested complexity alone was
  enough (the pre-filter's pitch search needs complexity 5); the compare
  test's pure tones showed that libopus 1.6 also enables it for strongly
  tonal input at any complexity, so only prediction disabled guarantees it.
  The application became libopus 1.6's `OPUS_APPLICATION_RESTRICTED_CELT`,
  whose packets were checked to be byte-identical to
  `OPUS_APPLICATION_RESTRICTED_LOWDELAY`'s (50,000 frames at the contract
  settings); it only saves the encoder its SILK state.
* **Where to optimise:** faster than libopus on every benchmark case, with
  `perf` work aimed at the slowest stream (510 kb/s).

## Optimisations beyond the port

Porting libopus to templated C++ and specialising it to one stream already
removes a lot: the stream's constants become compile-time constants, and
SILK, hybrid and mode switching disappear. On its own, that left opuspp at
0.68× of libopus with GCC -O2 and 1.30× with Clang (built with the
plain-array SIMD backend, so only the compilers' auto-vectorisers apply;
measured over all benchmark streams at encoder complexity 0, 2 and 4
combined, so not directly comparable with the 320 kb/s results).
Everything below was done on top of that, deliberately; all of it
keeps the output bit-identical.

**Explicit SIMD** lifts that to 1.40× (GCC) and 1.41× (Clang), and with 10 %
packet loss from 0.49× to 1.40× (GCC). GCC at -O2 auto-vectorises only very
cheap loops, so the explicit layer is what makes opuspp's speed independent
of the compiler:

* libopus's own SSE kernels, `xcorr_kernel` and `celt_inner_prod` (and
  `comb_filter_const` until the post-filter left the contract), ported to the
  portable type with the same lane layout, so they also vectorise on NEON and
  other targets.
* Code libopus runs as scalar C, vectorised with each lane computing the
  exact scalar expression:
  * the radix-3/4/5 FFT butterflies, two complex values per vector;
  * the IMDCT pre- and post-rotation and the TDAC windowing;
  * band noise fill, with a 4-lane version of the CELT random generator
    using jump-ahead constants, and band folding;
  * the Hadamard and Haar steps, as shuffles and 4×4 transposes;
  * denormalisation, stereo merge, renormalisation, the concealment loops
    and the energy state updates;
  * de-emphasis loads and stores (the recursion itself stays scalar).

**Memory layout and alignment:**

* Every buffer and table `alignas(64)`, per-channel rows and band arrays
  padded to whole vectors, so all vector loads and stores are aligned.
* Sliding windows at arbitrary offsets resolved once per call into one of
  four template instantiations and assembled from aligned blocks with
  compile-time shuffles, instead of unaligned loads.
* Bulk copies, fills and moves in whole 64-byte cache lines, and the decoder
  state laid out as cache-line blocks, which also stops compilers from
  merging aligned stores into unaligned ones.

**Compile-time precomputation:** per-stage FFT twiddle tables laid out for
aligned vector loads, the FFT stage sequence unrolled at compile time, IMDCT
post-rotation twiddles in lane order, and a `bits2pulses` lookup table
(6 KB) built by a `constexpr` lambda.

**Serial integer code**, found with `perf` in three rounds:

1. The `bits2pulses` table replaces a chain of six dependent loads per
   partition, and `isqrt_small` (hardware square root plus an integer
   correction, exact on its whole range) replaces a 16-step loop in theta
   decoding. Measured at -O3 during development, they cut the cycles per
   frame at 510 kb/s from 109k to 96.6k (GCC, −11 %) and from 119k to 94.8k
   (Clang, −20 %), against 114k for libopus. `quant_band` is
   force-inlined, because as a call its many arguments went through the
   stack.
2. At 510 kb/s opuspp then mispredicted about 830 branches per frame, worth
   roughly 13 % of its cycles, and `cwrsi` (the pulse-vector index decoder)
   had 37 % of them: a scan down a table row that ends at a data-dependent
   point. A binary search removed the mispredictions but was slower (its
   dependent loads cost more, and Clang turned its select back into a
   branch). Counting worked: the row is monotonic, so the number of scan
   steps equals the number of entries above the key in a window of eight,
   compared independently. That cut 2.6–4.4 % of the cycles per frame at 128
   and 510 kb/s; the full benchmark showed the largest effect at 320 kb/s,
   which the per-frame check hadn't covered (GCC Balanced from 1.26× to
   1.48×). Five other ideas were measured and dropped, among them evaluating
   both sides of the theta decode and the same window for `cwrsi`'s column
   scans.
3. A third round found nothing more to gain exactly: an integer norm in
   `cwrsi` was exact but not on the critical path, and running
   `exp_rotation` over short blocks in parallel would help less than 1 % of
   its work (93 % of the rotated samples at 128 kb/s, and all at 320 kb/s,
   are in the serial single-block case). What is left is inherent: range
   decoder divisions, square-root latency in the angle decode, and the
   serial spreading rotation.

**Side by side.** Four of these, libopus first, opuspp second (shortened):

*Noise fill.* libopus generates one random value per coefficient; opuspp
advances four generator states at once with jump-ahead constants, so the
four lanes produce exactly the next four values of the same sequence.

```c
/* libopus, celt/bands.c */
for (j=0;j<N;j++)
{
   ctx->seed = celt_lcg_rand(ctx->seed);
   X[j] = SHL32((celt_norm)((opus_int32)ctx->seed>>20), NORM_SHIFT-14);
}
```

```cpp
// opuspp, detail/bands.hpp (aligned middle part; scalar head and tail as above)
Lcg4 lcg(seed);                       // lanes: seed_{j+1}..seed_{j+4}
for (; j + 4 <= n; j += 4) {
   const u32x4 r = lcg.next();        // s = s * a^4 + c * (1 + a + a^2 + a^3)
   convert<float>(bit_cast<std::int32_t>(r) >> 20).store_aligned(x + j);
   seed = r[3];
}
```

*`bits2pulses`.* libopus binary-searches the band's pulse cache on every
call; opuspp looks the answer up in a table built at compile time from the
same cache.

```c
/* libopus, celt/rate.h */
lo = 0;
hi = cache[0];
bits--;
for (i=0;i<LOG_MAX_PSEUDO;i++)
{
   int mid = (lo+hi+1)>>1;
   if ((int)cache[mid] >= bits) hi = mid; else lo = mid;
}
if (bits- (lo == 0 ? -1 : (int)cache[lo]) <= (int)cache[hi]-bits)
   return lo;
else
   return hi;
```

```cpp
// opuspp, detail/rate.hpp
const int r = bits2pulses_lut.row[(LM + 1) * nb_ebands + band];  // constexpr table
if (bits < 0) [[unlikely]]
   return bits2pulses_search(pulse_cache(band, LM + 1), bits);
return bits2pulses_lut.v[r][min(bits, Bits2PulsesLut::max_bits)];
```

*Integer square root in the theta decode.* libopus finds the root one bit at
a time; opuspp takes the hardware float square root, which is exact for
these arguments, and keeps a correction that makes that a guarantee.

```c
/* libopus, celt/mathops.c: isqrt32 */
g=0;
bshift=(EC_ILOG(_val)-1)>>1;
b=1U<<bshift;
do{
  opus_uint32 t;
  t=(((opus_uint32)g<<1)+b)<<bshift;
  if(t<=_val){ g+=b; _val-=t; }
  b>>=1;
  bshift--;
}while(bshift>=0);
```

```cpp
// opuspp, detail/bands.hpp: isqrt_small, for 1 <= v < 2^24
unsigned g = static_cast<unsigned>(celt_sqrt(static_cast<float>(v)));
if (g * g > v) g--;
else if ((g + 1) * (g + 1) <= v) g++;
```

*The pulse-vector scan in `cwrsi`.* libopus steps down a table row until the
entry fits, an exit that depends on the data; opuspp counts the entries
above the index in a window of eight, which is the same number of steps
because the row is sorted.

```c
/* libopus, celt/cwrs.c */
else for(p=row[_k];p>_i;p=row[_k])_k--;
```

```cpp
// opuspp, detail/rate.hpp
k = last_le(row, n, k, i);            // in last_le, for row[k'] > i in [lo, hi]:
// for (;;) {
//    int steps = 0;
//    for (int j = 0; j < 8; j++) steps += int(row[max(hi - j, lo)] > i);
//    hi -= steps;
//    if (steps < 8) return hi;
// }
p = row[k];
```

**Platform details:** a hardware square root wherever the compiler can emit
it without a libm fallback, including the SSE scalar builtin for GCC at -O0
and -Os on x86 (with the software `sqrtf`, GCC -Os ran at 0.95× of
libopus); and fast-math support that keeps the concealment's NaN guard
working by testing the bit pattern.

**Memory:** the decoded spectrum is kept in the caller's output buffer, and
the concealment steps run out of line
([below](#stack-and-concealment-scratch)).

## Measurement details

The details behind the published numbers, and what was checked along the
way.

### Benchmark design

* **Like with like.** libopus's speed depends on the compiler, in both
  directions: at 510 kb/s, Clang's build is 7–11 % slower than GCC's at
  -O2/-O3 but 22 % faster at -Os. So every comparison builds libopus and
  opuspp with the same compiler and flags. An independent check confirmed that each
  benchmark binary contains exactly its own configuration's libopus, and
  that `perf stat` timings of a separate decoder program agree with the
  benchmark within a few percent.
* **Cache-aware.** Both decoders read the same packets from a 64-byte
  aligned buffer that is flushed from the caches before every timed pass, so
  neither gains from packets an earlier pass left in cache. Both decoders
  live in one benchmark binary; the layout check below shows what that
  costs.
* **The Fast profile in the reference's favour.** -O2 and -O3 were measured,
  each with and without `-march=native`, all with fast math; -O2 and -O3 were
  within 3 % of each other, and Fast uses -O3, where libopus was fastest.
  Code alignment was settled the same way: a sweep of `-falign-loops`
  (none/16/32/64) × `-falign-functions` (none/32/64) at 510 kb/s, then the
  three best again at best of 25. GCC-built libopus was 2.4 % faster with
  `-falign-loops=32` (367 vs 376 ms), which was also libopus's best Clang
  build (407 ms), while opuspp stayed within 1 % (313–315 ms GCC, 301–303 ms
  Clang). So Fast uses it, although it lowers opuspp's GCC Fast ratio by
  about 3 %.
* **The loss case** drops packets independently at random, so most losses
  are isolated and each starts a new pitch search; with burst losses only
  the first loss of a burst searches. A concealment-only case is available
  in the benchmark for profiling.

### Code layout sensitivity

Restructuring only the concealment code once moved the Clang -O2 result from
1.30× to 1.23× overall, while GCC and the other Clang builds didn't move. A
check across six layouts (function alignment 32/64/128, loop alignment
32/64, and the default) settled it: on average the two versions were equal
(627 vs 626 ms), and the default layout happened to be the fastest of the
twelve builds for the old code and the slowest for the new. Builds of the
same code that differ only in layout vary by about ±2 % with Clang -O2.

### How wide the vector code is

libopus's kernels, and opuspp's ports of them, are 4-wide. A census of the
Fast binary (every instruction classified as scalar, 128-, 256- or 512-bit,
weighted by sampled cycles) showed the compilers don't widen that code on
their own: on an AVX-512 machine, 128-bit instructions took about 10–15 % of
opuspp's cycles and 256/512-bit ones at most 1.5 %, mostly moves and integer
loops. Most of the time is serial integer work, so wider element-wise
kernels could save at most a few percent.

### Code generation audit

Two scripts check the generated code (paths relative to `opuspp/`):

* `tools/vec_report.py` lists every loop in the headers with the GCC and
  Clang vectoriser verdicts, so any loop that isn't vectorised can be
  justified (serial dependency, scalar head or tail, explicit SIMD).
* `tools/align_report.py` disassembles the library and maps every vector
  memory access back to its source line, flagging any unaligned form
  (`movups`, `movdqu`, `vmovups`, ...).

Result: 0 unaligned vector memory accesses for GCC 16 at -O1 to -O3 and -Os
on x86-64 baseline, v2, v3 (AVX2) and v4 (AVX-512), and for Clang 22 at the
same levels on baseline, v2 and v3. The exception is Clang with AVX-512,
whose instruction combiner turns some "aligned blocks + shuffle" windows
back into single unaligned loads; preventing that would need inline asm.

### Stack and concealment scratch

`-fstack-usage` per function misses how inlining merges frames, so the stack
is measured at run time (see [memory.md](memory.md)). That showed the
earlier documented figures (11 KB per packet, 20 KB in concealment) were
overstated, and that concealment used about 6 KB more than libopus.

Concealment rebuilds the audio from the history: a pitch search on the first
lost packet (the history downsampled by 2, 4 KB, plus the search's own
4.7 KB), the excitation (`exc_buf`, about 4.1 KB, and the whitened copy
`fir_tmp`, 4 KB) and the synthesis filter (about 2.5–3 KB). libopus
allocates these as variable-length arrays, which exist only from their
declaration on; opuspp's fixed-size arrays occupy the whole stack frame of
their function, inlined helpers included. So the steps that never run at the
same time are kept out of line (`plc_pitch_search`, `conceal_periodic`,
`celt_iir`), the autocorrelation uses `fir_tmp` as scratch before the FIR
filter needs it, and both normal decoding and noise-based concealment keep
their frequency-domain coefficients in the caller's output buffer.

### Rejecting unsupported packets

The fuzz test confirms every rejection independently: whenever opuspp reports
a packet with a supported header as unsupported, a separate libopus decoder
decodes it from a fresh state and must report a pitch post-filter
(`OPUS_GET_PITCH` > 0). A stream from an encoder with prediction enabled is
also checked: exactly its post-filter packets are rejected, and the others
decoded.

### Encoder settings and decoder paths

The encoder settings were evaluated by counting which decoder paths each one
exercises (20 s of the benchmark signal, GCC -O2). They change how much work
the decoder does, not what it could skip at compile time: complexity 0
disables transients, time-frequency changes and spreading (about 16 % less
decode time for both decoders at 128 kb/s); forced mono saves about 18 %;
complexity 1–4 all use transients, and spreading is fixed at 1–2 and
adaptive at 3–4. opuspp's lead over libopus stays the same across all of
them. QEXT, DRED and the deep-learning features need special libopus builds
and travel in padding extensions, which opuspp skips like a standard libopus
decoder.

## Problems hit and lessons

Claude debugged its own failures along the way, and each lesson went into
`CLAUDE.md` or the skills so it wasn't paid for twice:

* the sign of zero lost in `0 - x` (fixed by using `x * -1`);
* `memset`/`memcpy` calls hidden in aggregate initialisers and struct copies
  at -O0, which break the freestanding guarantee;
* compilers merging aligned stores into unaligned wider ones;
* a libopus reference whose concealment changed with the CPU because of
  run-time AVX2 dispatch;
* concealment using more stack than libopus, fixed as described above;
* a regression the -O2 measurements had missed: Clang's Fast build turned
  `cwrsi`'s eight unrolled compares into a gather instruction, slow on this
  CPU, and a contiguous variant made Clang -O2 vectorise them with unaligned
  loads instead. The window now stays scalar, and the lesson went into the
  skills: check the generated code of every build profile after a rewrite;
* dead ends: a branchless `bits2pulses` (the cost is the load-latency
  chain, not branches) and a software `sqrtf`.

## What could unlock more

Within the current constraints, the remaining time is inherent to the
algorithm. Changing a constraint would open more:

* **Tolerance instead of bit-exactness:** FMA contraction, wider reductions
  with a different summation order, and faster approximations of `exp`,
  `cos` and `1/sqrt`.
* **A larger code size budget:** a 64 KB `1/sqrt` table for the pulse norm
  and a 32 KB `bitexact_cos` table, for a few percent at high bit rates.
* **A minimum instruction set** (for example x86-64-v3): element-wise kernels
  written 8 lanes wide without a fallback, for a few percent at most.
* **Stereo-coded packets only:** the mono-coded paths could go, a small
  simplification rather than a speed-up.

## Reusing the lessons

The method and the lessons from this port are captured as a set of generic
Claude Code skills for rewriting audio-focused C libraries into fast,
freestanding, bit-exact C++:

| Skill | Covers |
|---|---|
| `cxxport-workflow` | The entry point: ground-truth files, phases with gates, rules for multi-session work |
| `cxxport-scope-and-port` | Measuring the real input space, reading the preprocessed reference, translating bit-exactly, generated tables |
| `cxxport-bit-exact-oracle` | A deterministic reference, differential tests, lockstep fuzzing, the build matrix, the verify script, triage |
| `cxxport-freestanding` | No libc/libm/heap, hidden `memset`/`sqrtf` calls, math that matches libm, `constexpr` tables |
| `cxxport-portable-simd` | `simd<T,N>` on vector extensions, reference lane layouts, aligned sliding windows, store merging |
| `cxxport-codegen-audit` | Per-loop vectorisation verdicts and unaligned-access reports (scripts included) |
| `cxxport-perf-and-measure` | `perf` workflow, fair benchmark profiles, code size and memory measurement, honest reporting |

They live in `.claude/skills/` in this repository for now, next to the
opuspp-specific skills, and are meant to move to a repository of their own.
