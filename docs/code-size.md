# Code size

* **Smaller than libopus in every build:** 71–198 KB of decoder code,
  against 98–236 KB for a statically linked libopus decoder.
* **3.2–5.2× smaller than the shared library**, which an application linking
  libopus dynamically loads in full (286–718 KB).
* **Most of the saving comes from the stream definition** (no SILK, no Opus
  layer mode switching, no post-filter); part of it is spent again on speed.

Decoder code is text + data, for each build profile of
[performance.md](performance.md#build-profiles).

## Against libopus as a shared library

An application that links libopus dynamically loads the whole library,
including the encoder and SILK:

| | GCC Size | GCC Balanced | GCC Fast | Clang Size | Clang Balanced | Clang Fast |
|---|---|---|---|---|---|---|
| libopus.so (whole library) | 285.7 KB | 403.6 KB | 628.0 KB | 319.2 KB | 454.7 KB | 718.3 KB |
| program code for calling it | 0.6 KB | 0.6 KB | 0.5 KB | 0.6 KB | 0.6 KB | 0.6 KB |
| opuspp (compiled in) | 71.3 KB | 89.6 KB | 121.6 KB | 101.5 KB | 124.9 KB | 197.8 KB |
| opuspp smaller than libopus.so by | 4.01× | 4.51× | 5.16× | 3.15× | 3.64× | 3.63× |

A shared library has advantages this table doesn't show: when several
processes use it, its code pages are loaded once and shared, the system can
update it without rebuilding the application, and it offers the library's
full feature set. opuspp is compiled into each application that uses it.

## Against a statically linked libopus

Linked statically, only the libopus code the decoder can reach is included:

| | GCC Size | GCC Balanced | GCC Fast | Clang Size | Clang Balanced | Clang Fast |
|---|---|---|---|---|---|---|
| opuspp | **71.3 KB** | 89.6 KB | 121.6 KB | 101.5 KB | 124.9 KB | 197.8 KB |
| libopus | 98.4 KB | 132.8 KB | 206.7 KB | 106.0 KB | 159.5 KB | 235.7 KB |
| opuspp smaller by | 1.38× | 1.48× | **1.70×** | 1.04× | 1.28× | 1.19× |

libopus grows much more than opuspp under -O3 and `-march=native`, so the
gap is largest in the Fast profile.

## What makes opuspp smaller

The stream opuspp decodes is defined in the [README](../README.md#constraints) and
in [opuspp/README.md](../opuspp/README.md#what-it-decodes). Each part of that
definition lets opuspp leave code out (sizes at GCC Balanced):

| Part of the stream definition | What it strips |
|---|---|
| CELT-only (`OPUS_APPLICATION_RESTRICTED_CELT`) | The SILK decoder: about 37 KB in libopus |
| One mode, fullband, 10 ms, one frame per packet | The Opus layer's hybrid decoding, mode switching and redundancy: about 15 KB |
| Encoder prediction disabled | The pitch post-filter (comb filter): about 9 KB |
| 48 kHz stereo output, fixed frame size (LM = 2) | Resampling, down-mixing, all FFT sizes but 240 and 60, and the tables only other sizes use (not measured separately) |
| Encoder complexity 0–4 | Nothing: transients, time-frequency changes and every spreading mode still occur |
| No FEC, DRED, OSCE, deep-learning concealment | Nothing compared with a default libopus build, which doesn't include them either |

Against the shared library, everything the decoder never calls is stripped
as well: the encoder, the signal analysis and the multistream API.

Part of the saving is spent again on speed:

* **Code:** each sliding-window kernel exists in four alignment
  instantiations, the FFT stages are unrolled, and hot helpers are
  force-inlined.
* **Tables:** the `bits2pulses` lookup table (6 KB) and the per-stage FFT and
  IMDCT twiddles (about 9 KB) are precomputed, so opuspp's constant data is
  33 KB against libopus's 29 KB.

## How the values were collected

`tools/profile_report.py` (paths relative to `opuspp/`) builds a small
decoder program, `tools/opus_wav/decode.cpp` (read packets, decode, write a
WAV), in each build profile and with the same compiler flags for libopus and
opuspp, four ways:

* with opuspp;
* with libopus as a static library (`opus_decode_float`, the same libopus
  build as the speed benchmark);
* with libopus as a shared library (`libopus.so`, same flags);
* as a stub without any decoder.

A decoder's size is the program's text + data (Berkeley `size`) minus the
stub's, so the harness and the C++ runtime cancel out. All programs are
linked with `--gc-sections`, so only reachable code counts. For the shared
library, the size of `libopus.so` itself is reported, plus what the program
needs to call it (its size minus the stub's). Decoder state is not included:
opuspp's `Decoder` lives wherever the caller puts it, and libopus allocates
its own (see [memory.md](memory.md)).

The per-feature sizes in the table above are symbol sizes (`nm -S`) from the
GCC Balanced libopus decoder program, summed by function-name prefix
(`silk_*` for SILK, the `opus_*` decoder functions for the Opus layer); the
comb filter's size was measured the same way in opuspp before it was
removed. Code that the compiler inlined into other functions is not
counted there, so they are lower bounds.
