# opuspp

**An experiment: can Claude Code, running Opus 5.5, turn the latest libopus
release into C++ specialised for one use case, and still match libopus bit
for bit?**

## The experiment

### Why

The idea came from working with Opus and audio pipelines across several
companies and products over the past 15–20 years, and seeing how much
performance can be gained by using modern C++ instead of C. Compilers have
improved a lot over those years, while SIMD has gone from 64-bit MMX to
512-bit AVX-512.

Along the way I often looked at C libraries that produce very good code when
they are compiled into the binary that uses them. Once built as a shared
library, however, the compiler can no longer optimise them for the caller.
The library has to support being called in any way the caller might think
of, with data whose alignment is only guaranteed by its data type. Some
libraries even take byte pointers, so the data may only be aligned to a
single byte.

The audio pipelines I wrote myself relied heavily on these principles to get
the best performance:

* **Meticulous care with data alignment**, since unaligned access behaves
  differently on ARM, ARM64 and x86/AMD64.
* **Iterate** between writing code and analysing the generated assembly (in
  Compiler Explorer) until the code paths are optimal.
* **Code size matters more than one would think.** Code that is faster in a
  benchmark may not be in production, when other work pollutes the caches.
* **Generic C code is rarely optimal for a specific use case**, because it
  is written to be versatile and compiled into libraries.
* **Header-only C++** generally produces better code, especially when it is
  written for one particular use case, because the compiler sees everything
  at the call site.

While experimenting with Claude, I came up with the idea of letting Claude
Opus 5.5 rewrite libopus as a more specific, templated C++ version, and of
seeing what performance gains that would bring for use cases like the ones I
worked on: wireless audio streaming with the Opus codec.

### The task given to Claude

Port the libopus 1.6.1 decoder to C++, specialised to one use case. I set the
goals and constraints, answered Claude's design questions and reviewed the
results. Claude wrote the code, tests, tools and documentation from the
libopus sources.

### Constraints

The use case was taken from Opus audio streaming in real products: low-latency
audio streaming over Wi-Fi. That fixes the stream the decoder has to handle:

* **Encoder application:** `OPUS_APPLICATION_RESTRICTED_CELT`.
* **Bandwidth:** `OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)`.
* **Frame size:** 10 ms, one frame per packet.
* **Encoder complexity:** 0–4.
* **Prediction disabled:** `OPUS_SET_PREDICTION_DISABLED(1)`. The band
  energies of every coded frame are coded without reference to the previous
  frame, which keeps recovery after a lost packet clean, and the pitch
  post-filter is never used.
* **Output:** 48 kHz stereo, 32-bit float.
* **Loss handling:** CELT packet loss concealment, but no forward error
  correction.
* **Left out:** SILK, hybrid, other bandwidths and frame sizes, the pitch
  post-filter, FEC and the deep learning extensions. Everything the stream
  makes constant is hardcoded; the details are in
  [opuspp/README.md](opuspp/README.md).

These choices suited the product that inspired the experiment: its latency
budget, the encoder complexity it could afford, and the error correction
already provided by a lower layer of the transport. Other use cases may well
need different settings.

### Implementation requirements

* **Compliant:** the output must be bit-identical to libopus, including
  packet loss concealment.
* **Header-only and freestanding C++23:** no heap allocation in the library
  (the caller decides where the decoder lives), no libm, no exceptions or
  RTTI, no assembly.
* **Portable SIMD:** the code should be as portable as possible, with minimal
  use of intrinsics and compiler built-ins, leaving code generation to the
  compiler.
* **Alignment:** meticulous attention to aligned memory accesses and cache
  lines, so the code vectorises on any platform.
* **Measured fairly:** libopus and the port are built with identical compiler
  flags, and losses are reported as clearly as wins.

## Results

The result is [`opuspp/`](opuspp/), a self-contained, redistributable
header-only library. Measured on one core of a Ryzen 5 9600X with GCC and
Clang, each build with libopus built identically:

* **Compliant:** bit-identical to libopus in 47 encoder configurations,
  including packet loss, and in a 300k-packet fuzz test.
* **Faster:** 1.27× to 1.48× at 320 kb/s, 1.09× to 1.50× over other bit
  rates, packet loss and encoder complexities.
* **Smaller:** 71–198 KB of decoder code, against 98–236 KB for a statically
  linked libopus and 286–718 KB for the shared library.
* **Less memory:** an 18.1 KB decoder state against 26.6 KB, less stack, no
  allocation by the library, and no external symbols in a freestanding
  build.

The details are in [docs/performance.md](docs/performance.md),
[docs/code-size.md](docs/code-size.md), [docs/memory.md](docs/memory.md) and
[docs/verification.md](docs/verification.md).

## How it was made

Claude worked from a written spec, a phased plan with gates, project
conventions in `CLAUDE.md` and a set of skills (port, verify, codegen audit,
perf, bench), with one script, `opuspp/tools/verify.sh`, deciding when
something was done. It ported for correctness first, then vectorised, then
audited the generated machine code, then profiled with `perf`. The lessons
are captured as reusable Claude Code skills for porting other audio C
libraries. The full story is in
[docs/how-claude-made-it.md](docs/how-claude-made-it.md).

## Repository layout

| Path | Contents |
|---|---|
| [`opuspp/`](opuspp/) | The library, its tests, benchmarks and tools; redistributable as is. Its [README](opuspp/README.md) covers usage, building, design and the source layout |
| [`docs/verification.md`](docs/verification.md) | How bit-exactness is tested, and expected deviations |
| [`docs/performance.md`](docs/performance.md) | Speed against libopus for every build profile, where the gains come from, and how it was measured |
| [`docs/performance-additional.md`](docs/performance-additional.md) | Speed at other bit rates, with packet loss and at other encoder complexities |
| [`docs/performance-build-profiles.md`](docs/performance-build-profiles.md) | How each decoder's speed changes with the build profile |
| [`docs/performance-shared-library.md`](docs/performance-shared-library.md) | libopus as a shared library against the static one, and opuspp against both |
| [`docs/code-size.md`](docs/code-size.md) | Decoder code size per build profile, and what is dropped and added |
| [`docs/memory.md`](docs/memory.md) | Decoder state, stack and output buffer |
| [`docs/how-claude-made-it.md`](docs/how-claude-made-it.md) | How Claude Code designed, ported and optimised it, with the detailed measurements and lessons |
| `CLAUDE.md`, `.claude/skills/` | The conventions Claude works with, the opuspp-specific skills (`opuspp-*`) and the generic skills for porting audio C libraries (`cxxport-*`) |
| `opus-1.6.1/` | The libopus reference sources, downloaded by `opuspp/tools/fetch_deps.sh` (not committed) |

## License

The algorithms, tables and structure come from libopus, copyright
2001-2023 Xiph.Org, Skype Limited, Octasic, Jean-Marc Valin, Timothy B.
Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo,
Mozilla and Amazon. This is a derivative work under the same BSD-3-Clause
license (see `LICENSE`).
