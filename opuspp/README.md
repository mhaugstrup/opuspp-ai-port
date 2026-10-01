# opuspp

A header-only, freestanding C++23 port of the libopus 1.6.1 float decoder, cut
down to exactly one kind of stream: CELT, fullband, 10 ms frames. It is
bit-identical to libopus on that stream, about 1.3–1.5× faster (320 kb/s,
encoder complexity 4, same compiler and flags) and smaller.

This folder is self-contained and can be redistributed as is: the library,
its tests, benchmarks and tools, and the license. It was written by Claude
Code (Opus 5.5) as an experiment; the measured results and how it was made are
documented in the [opuspp-ai-port repository](https://github.com/mhaugstrup/opuspp-ai-port).

## What it decodes

opuspp handles exactly one kind of stream: the output of an encoder
configured with `OPUS_APPLICATION_RESTRICTED_CELT`,
`OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)`, 10 ms frames,
`OPUS_SET_COMPLEXITY` 0–4 and `OPUS_SET_PREDICTION_DISABLED(1)`. With
prediction disabled, the encoder never enables the pitch pre-filter, so the
decoder has no post-filter (comb filter): packets that enable it are
rejected.

| | |
|---|---|
| Input | CELT-only fullband 10 ms packets (TOC config 30), mono- or stereo-coded, one frame per packet (code 0, or code 3 with count 1, e.g. CBR padding) |
| Output | `opuspp::AudioBuffer`: 48 kHz, 2 channels, 480 frames of interleaved `float` (same scale as `opus_decode_float`, no soft clip), 64-byte (cache line) aligned |
| Loss | Pitch/noise based CELT PLC (`decode_lost`, or a null packet). Packets without audio data (0–1 payload bytes: DTX, or the encoder's "PLC" packets below 2.4 kb/s or with a buffer under 3 bytes) are concealed for any 10 ms TOC, as in libopus |
| Not supported | SILK, hybrid, NB/WB/SWB, other frame sizes, multi-frame packets, packets that enable the pitch post-filter (rejected with `Error::unsupported_packet`); FEC, DRED, OSCE, deep PLC, multistream, gain, custom modes |

With these encoder settings, libopus only ever emits config 30 for
packets with audio data (verified over 0.5–510 kb/s, VBR/CBR, DTX and forced
channels). The only other TOC it produces is hybrid fullband 10 ms
(config 14), on packets without audio. This happens when nothing has been
coded yet and the bitrate or buffer is too small, and those packets are
handled as a loss.

Everything the stream makes constant is hardcoded: the 48 kHz CELT mode,
LM = 2 (N = 480), two decoder channels, bands 0–21, no resampling or
down-mixing, only the 240- and 60-point FFTs, and only the tables those paths
touch.

How each packet is handled:

| Packet | Result |
|---|---|
| Config 30, code 0 or code 3 with count 1, at least 2 payload bytes | Decoded |
| Any 10 ms TOC with 0–1 payload bytes (DTX, no audio data) | Concealed, as in libopus |
| Null or empty packet, or `decode_lost` | Concealed |
| Any other config, code 1 or 2, or code 3 with a count other than 1 | `Error::unsupported_packet` |
| Config 30 with the pitch post-filter enabled (encoder with prediction enabled) | `Error::unsupported_packet` |
| Malformed, where libopus would reject it | `Error::invalid_packet` |

## Requirements

**To use the library:**

* A C++23 compiler. GCC 16.2 and Clang 22.1 are tested. On GCC and Clang the
  SIMD layer uses their vector extensions; other compilers get the
  plain-array backend, which gives identical results but is untested.
* Nothing else: the headers only use `<cstddef>`, `<cstdint>` and `<bit>`,
  and need no libc, libm, heap, exceptions or RTTI.
* Optionally CMake 3.21 or newer, for `add_subdirectory`.

**Only for the tests, benchmarks and tools:**

| Needed for | Requirement |
|---|---|
| Tests and benchmarks | CMake 3.21 or newer, a C compiler for libopus, the libopus 1.6.1 sources (`tools/fetch_deps.sh`) |
| `tools/fetch_deps.sh` | `curl`, `sha256sum`, `tar`, and `ffmpeg` for the benchmark clip |
| `tools/verify.sh` | Both GCC (`g++`) and Clang (`clang++`), Clang's ASan and UBSan, binutils (`nm`, `objdump`), Python 3 |
| `tools/vec_report.py`, `tools/align_report.py` | Python 3, GCC and Clang; `align_report.py` also needs `objdump` and an x86-64 target |
| `tools/profile_report.py` | Python 3, GCC and Clang, `taskset` (util-linux) for pinning |
| Profiling | Linux `perf` |
| `tools/gen_tables.py` | Python 3 and a C preprocessor (`gcc -E`) |

All of this was used on x86-64 Linux.

## Usage

```cpp
#include <opuspp/opuspp.hpp>

opuspp::Decoder dec;
opuspp::AudioBuffer stereo_pcm;

...

const auto& packets = get_received_audio_packets();
for (const auto& packet : packets) {   
   if (dec.decode(packet.data(), packet.size(), stereo_pcm) != opuspp::Error::ok) {
      //assume empty packet == lost packet
      dec.decode_lost(stereo_pcm);
   }
   audio_play(stereo_pcm);
}

```

`AudioBuffer` is exactly 3840 bytes (60 cache lines), with the first sample on
a cache-line boundary, so arrays of buffers stay aligned as well. `decode`
returns `Error::ok` once the buffer holds the next 10 ms. On `invalid_packet`
or `unsupported_packet`, the decoder state and the buffer are untouched.
`internal_error` mirrors libopus: the frame was decoded but overran its bits.
Concealment before the first decoded packet (or after `reset`) outputs
silence, as in libopus.

With CMake: `add_subdirectory(opuspp)` and link `opuspp::opuspp`, or just add
`opuspp/include/` to the include path.

## Building and testing

The library itself needs nothing but the headers. The tests and tools need
two inputs that aren't committed: libopus 1.6.1 (the reference) and the
benchmark music clip. `tools/fetch_deps.sh` downloads and hash-checks both.
libopus goes outside this folder, to `../opus-1.6.1` by default; set
`OPUSPP_LIBOPUS_DIR` (environment variable for the scripts, CMake option for
the build) to use another location.

```sh
tools/fetch_deps.sh                                    # once: libopus + music clip
cmake -S . -B build -DOPUSPP_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j && ctest --test-dir build       # compare, compare_approx, fuzz, math_check
tools/verify.sh /tmp/opuspp-verify                     # full acceptance matrix (~5 min; --quick ~1.5 min)
```

## Changing the code

The bar for every change is bit-exactness with libopus, so:

* **Keep the libopus float evaluation order:** same operand order, same
  temporaries, same summation tree. A SIMD lane must compute exactly the
  scalar libopus expression, and vectorised reductions must use the lane
  layout of the libopus SSE kernels (`celt/x86/pitch_sse.c`).
* **Compare against the SSE baseline.** The tests build libopus with
  `OPUS_X86_MAY_HAVE_AVX2=OFF`. A default x86 libopus build chooses an
  AVX2+FMA pitch kernel at run time, so its concealment output depends on
  the CPU and can't serve as a reference.
* **Keep it freestanding:** only `<cstddef>`, `<cstdint>` and `<bit>`, and no
  call that could become a libc or libm symbol. The code comments mark the
  known traps (aggregate initialisers, `sqrt`, negation).
* **Keep vector memory accesses aligned:** 0 unaligned accesses in
  `tools/align_report.py` for GCC (-O1 to -O3, -Os; x86-64 baseline, v2, v3,
  v4) and Clang (same levels; baseline, v2, v3). Clang with AVX-512 is the
  one accepted exception, because preventing it would take inline asm.
* **Keep the source architecture-neutral.** Architecture-specific paths are
  fine (selected with compiler-predefined macros), but every path must stay
  correct on other targets.

`tools/verify.sh` checks the automated part. Two checks are manual:

* `tools/vec_report.py`: every loop that isn't vectorised needs a reason
  (a serial dependency, a scalar head or tail, or explicit SIMD).
* `tools/profile_report.py`: no benchmark case may be slower than libopus.

## Benchmarks and tools

```sh
tools/profile_report.py                                # speed and code size tables (~12 min)
tools/profile_report.py --reuse-results                # reuse measurements of unchanged binaries
build/opuspp_encode . 510000 128000                    # music clip -> figaro_original.wav + figaro_<kbps>k.opk
build/opuspp_decode figaro_510k.opk out.wav            # decode with opuspp (fast math) -> float WAV
perf record build/opuspp_profile pp 510000             # profile one stream (tests/profile510.cpp)
build/opuspp_stack_usage                               # peak stack per decoding path, opuspp vs libopus
```

`profile_report.py` caches its builds in `build-profiles/`, one directory per
configuration, and rebuilds only what changed. For quick checks during
development there are also the CMake targets `opuspp_bench` (fast math) and
`opuspp_bench_ieee`: `taskset -c 2 build/opuspp_bench_ieee 60 7 [segments] [cases]`,
where segments is a comma-separated list such as `music` or `tone,music`, and
cases picks streams from `128,256,320,510,loss,plc` (`plc`: concealment only).
They use the CMake Release flags, not the profiles, so their numbers aren't
directly comparable with the tables in
[published results](https://github.com/mhaugstrup/opuspp-ai-port/blob/main/docs/performance.md).

## Design

* **Freestanding C++23, header-only.** Only `<cstddef>`, `<cstdint>` and
  `<bit>` are used.
  * `exp` and `cos` are implemented in `detail/math.hpp`. They are accurate
    to a few ulp in double, so the float results equal libm's
    (`tests/math_check.cpp` checks 20M arguments each).
  * `sqrt` uses the hardware instruction whenever the compiler can emit it
    without a libm fallback, and a correctly rounded software version
    otherwise. On x86 that means GCC's SSE scalar builtin at -O0 and -Os.
  * An object built with `-ffreestanding -fno-exceptions -fno-rtti`
    references no external symbols, at -O0 to -O3 and -Os, with GCC and
    Clang.
* **No allocation.** All state lives in `Decoder` (18 KB), which the caller
  places: static, on the stack or on the heap. Scratch buffers are on
  the stack: a peak of about 4–7 KB for normal decoding and 10–12 KB during
  concealment, depending on compiler and optimisation level
  (`opuspp_stack_usage`). The decoded spectrum is built in the caller's
  `AudioBuffer`, which is dead until the output is written. The concealment
  steps run out of line, so their scratch arrays never share a stack frame.
* **Float modes.** By default opuspp matches a default (IEEE) libopus build.
  `-DOPUSPP_FLOAT_APPROX` matches libopus built with `FLOAT_APPROX` instead,
  where `celt_exp2` is a polynomial. As in libopus, `-ffast-math` is only
  accepted together with that mode (`#error` otherwise), and the PLC's NaN
  guard tests the bit pattern so fast math can't optimise it away.
* **Alignment.** The output buffer, every internal buffer and every table are
  `alignas(64)`. Band energies and the decoder history rows are padded to
  whole vectors, and all vector memory accesses use alignment-expecting
  instructions (checked by `tools/align_report.py`; see [Changing the code](#changing-the-code)).
* **SIMD without asm or intrinsics.** `detail/simd.hpp` provides
  `simd<T, N>`, built on GCC/Clang vector extensions, which lower to SSE,
  AVX, NEON, RVV and so on. `-DOPUSPP_NO_VECTOR_EXT` (or another compiler)
  selects a plain-array backend with identical results. Every lane evaluates
  the exact scalar expression, so results stay bit-identical. On top of it:
  * the libopus SSE/NEON kernels (`xcorr_kernel`, `celt_inner_prod`), with
    the same lane layout and summation order;
  * the radix-3/4/5 FFT butterflies with compile-time twiddles, the IMDCT
    post-rotation and TDAC windowing;
  * band noise fill and folding (a 4-lane LCG with jump-ahead constants),
    Hadamard and Haar steps (shuffles and 4×4 transposes);
  * de-emphasis, denormalisation, stereo merge, renormalisation, the PLC
    loops and the energy updates.

  Sliding windows at arbitrary offsets must never be read with unaligned
  loads.
  The misalignment is resolved once per call into one of four template
  instantiations, and windows are assembled from aligned blocks with
  compile-time shuffles (like NEON `vext`). Only genuinely serial code stays
  scalar: entropy decoding, bit allocation, PVQ index decoding, LPC/IIR
  recursions and the spreading rotation.

## Layout

| File (`include/opuspp/`) | libopus origin |
|---|---|
| `opuspp.hpp` | `src/opus_decoder.c`, `src/opus.c` (packet parsing) |
| `audio_buffer.hpp` | the cache-line aligned output buffer |
| `detail/celt_decoder.hpp` | `celt/celt_decoder.c` (decode, PLC, synthesis, de-emphasis) |
| `detail/bands.hpp` | `celt/bands.c`, `celt/vq.c`, `celt/quant_bands.c` |
| `detail/rate.hpp` | `celt/rate.[ch]`, `celt/cwrs.c` |
| `detail/pitch.hpp` | `celt/pitch.[ch]`, `celt/celt_lpc.c`, `celt/x86/pitch_sse.c` |
| `detail/mdct.hpp` | `celt/kiss_fft.c`, `celt/mdct.c` |
| `detail/entdec.hpp` | `celt/entdec.c`, `celt/entcode.c`, `celt/laplace.c` |
| `detail/math.hpp` | `celt/mathops.[ch]` plus freestanding libm replacements |
| `detail/simd.hpp` | the SIMD value type |
| `detail/celt_tables.hpp` | generated by `tools/gen_tables.py` from the preprocessed libopus sources |

## License

BSD-3-Clause, same as libopus (see `LICENSE`). The algorithms, tables and
structure come from libopus, copyright 2001-2023 Xiph.Org, Skype Limited,
Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark
Borgerding, Erik de Castro Lopo, Mozilla and Amazon.
