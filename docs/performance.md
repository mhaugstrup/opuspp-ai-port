# Performance

* **opuspp is faster than libopus in every build:** like for like (same
  compiler, same flags), 1.27× to 1.48×.
* **Five of the six opuspp builds beat the fastest libopus build** (GCC
  Fast), by 1.33× to 1.43×. Only opuspp built for size with GCC falls short
  (0.88×).
* **Other bit rates, packet loss and encoder complexities** give 1.09× to
  1.50× like for like ([additional results](performance-additional.md)).
* **The gains come from explicit SIMD and from the serial decoding code**,
  not from specialisation alone (see [below](#where-the-gains-come-from)).
* libopus is linked statically; as a shared library it performs on par
  ([details](performance-shared-library.md)).

Measured on a 320 kb/s stereo stream from a libopus encoder at complexity 4
(see [How it was measured](#how-it-was-measured)).

## Build profiles

| Profile | Compiler flags (identical for libopus and opuspp) | libopus CMake options | opuspp defines |
|---|---|---|---|
| **Code size optimised** ("Size") | `-Os` | | |
| **Balanced** | `-O2` | | |
| **Fast** | `-O3 -march=native -ffast-math -falign-loops=32` | `OPUS_FLOAT_APPROX=ON`, `OPUS_FAST_MATH=ON` | `OPUSPP_FLOAT_APPROX` |

All profiles also use `-DNDEBUG -ffunction-sections -fdata-sections` and
link with `-Wl,--gc-sections`, with GCC 16.2 and Clang 22.1. Size and
Balanced are strict IEEE float, where opuspp is bit-identical to libopus;
Fast is not bit-exact in either decoder. Fast, including its loop alignment,
is the configuration in which libopus itself is fastest, so the comparison
doesn't favour opuspp.

## Speed-up over libopus

The stream: 60 s of mixed audio (tones, noise, transients and music),
encoded by libopus at 320 kb/s, encoder complexity 4, fullband stereo, 10 ms
frames. Each decoder decodes it in every build profile; the times are the
best of 7 passes, pinned to one core of a Ryzen 5 9600X.

Each cell is the speed-up of the opuspp build in that column over the
libopus build in that row; the diagonal (bold) compares like with like. The
"libopus speed" column is each libopus build's speed relative to the fastest
one, GCC Fast.

| libopus build ↓ · opuspp build → | libopus speed | GCC Size | GCC Balanced | GCC Fast | Clang Size | Clang Balanced | Clang Fast |
|---|---|---|---|---|---|---|---|
| GCC Size | 0.69 | **1.27×** | 1.92× | 2.07× | 1.92× | 1.99× | 2.00× |
| GCC Balanced | 0.90 | 0.97× | **1.48×** | 1.59× | 1.47× | 1.53× | 1.53× |
| **GCC Fast** (fastest) | 1.00 | 0.88× | 1.33× | **1.43×** | 1.33× | 1.38× | 1.38× |
| Clang Size | 0.93 | 0.94× | 1.43× | 1.54× | **1.42×** | 1.48× | 1.48× |
| Clang Balanced | 0.94 | 0.93× | 1.41× | 1.52× | 1.41× | **1.46×** | 1.47× |
| Clang Fast | 0.97 | 0.90× | 1.37× | 1.47× | 1.37× | 1.42× | **1.42×** |

* **Like for like, opuspp is 1.27× to 1.48× faster.**
* **Against the fastest libopus build** (the GCC Fast row), opuspp is 0.88×
  to 1.43×: every opuspp build except GCC Size beats it.
* **GCC Size is the weakest case.** Most likely GCC at -Os inlines and
  vectorises less, and opuspp relies on both; this hasn't been profiled (see
  [performance by build profile](performance-build-profiles.md)).
* **Variation:** results vary by a few percent between runs; treat
  differences of that size as noise.

## Where the gains come from

Specialising the code to the stream alone is not what makes opuspp fast:
built without its explicit SIMD layer, it is slower than libopus with GCC
and gains much less with Clang. On top of that, a few exact rewrites of the
serial decoding code (a lookup table for the bit allocation, a hardware
integer square root, and a branch-free search in the pulse decoder) account
for most of the gain at the higher bit rates. The details, with code, are in
[how-claude-made-it.md](how-claude-made-it.md#optimisations-beyond-the-port).

## How it was measured

Paths to project files are relative to `opuspp/`.

`tests/bench.cpp` encodes audio with libopus, decodes it with libopus and
with opuspp, and reports the best of 7 passes for each decoder, pinned to
one core of a Ryzen 5 9600X. This page uses its 320 kb/s stream from an
encoder at complexity 4; the other streams it measures are in the
[additional results](performance-additional.md). The setup, as in
`tests/bench.cpp`:

```cpp
// Encoder (libopus), once per stream: 48 kHz stereo float input, 10 ms frames.
OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_CELT, &err);
opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
opus_encoder_ctl(enc, OPUS_SET_BITRATE(320000));
opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(4));
opus_encoder_ctl(enc, OPUS_SET_PREDICTION_DISABLED(1));
len = opus_encode_float(enc, pcm, 480, packet, 1500);      // per 10 ms frame
```

Both decoders read the same packets, each starting on a 64-byte boundary in
one contiguous buffer per stream, and produce 48 kHz stereo `float` output;
the libopus output buffer is an `alignas(64) float[960]`. Before every timed
pass of either decoder, the packet buffer is flushed from the CPU caches
(`clflush` on each cache line), so every pass starts with the packets in main
memory and neither decoder gains from an earlier pass. The libopus decoder:

```cpp
// libopus decoder: created once; each timed pass starts from a reset state.
OpusDecoder* ref = opus_decoder_create(48000, 2, &err);
opus_decoder_ctl(ref, OPUS_RESET_STATE);
alignas(64) float pcm[960];                                // 480 stereo frames
opus_decode_float(ref, packet, len, pcm, 480, 0);          // per packet; lost: (ref, nullptr, 0, pcm, 480, 0)
```

The input (`tests/bench_signal.hpp`) is 60 s cycling through four 2 s
segments:
* harmonic tones with a different pitch per channel;
* white noise;
* transients over a quiet bed;
* real music: an orchestral recording with a wide stereo image (public
  domain, fetched by `tools/fetch_deps.sh`; see `tests/data/README.md`).

`tools/profile_report.py` produces all the numbers above. For each compiler
and build profile it compiles libopus, the benchmark and the decoder program
with exactly the same flags (the decoder program is used for the
[code size](code-size.md) numbers). That libopus static library is both the
reference decoder and the benchmark's encoder.
