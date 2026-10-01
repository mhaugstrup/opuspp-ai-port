---
name: opuspp-port
description: Port a libopus 1.6.1 C module (or an upstream libopus change) into the opuspp headers bit-exactly, specialised to the fixed stream (CELT, fullband, LM=2, 48 kHz, stereo output). Use when writing or changing a detail/*.hpp module or upgrading the libopus version.
---

# Port libopus code into opuspp

Paths below are relative to `opuspp/`. The general method (scope, read the
preprocessed reference, translate keeping the float order, generate tables,
port in dependency order) is in the `cxxport-scope-and-port` skill; SIMD rules
are in `cxxport-portable-simd`.

## Read the reference as the build sees it

Float, no FIXED_POINT, no CUSTOM_MODES, no QEXT, no DRED/OSCE/deep PLC:

```sh
gcc -E -P -DOPUS_BUILD -DVAR_ARRAYS -I../opus-1.6.1/include -I../opus-1.6.1/celt -I../opus-1.6.1 \
    ../opus-1.6.1/celt/<file>.c | less
```

For the SSE kernels, read `celt/x86/pitch_sse.c` / `pitch_sse.h`; the
reference build has AVX2 off.

## opuspp specifics

* Constants from `detail/config.hpp`: `LM = 2`, `N = 480`,
  `shortMdctSize = 120`, `overlap = 120`, `nbEBands = 21`, `end = 21`,
  `C ∈ {1, 2}` (stream channels) while `CC = 2`, 48 kHz, no downsampling.
* The stream contract and packet acceptance table are in `opuspp/README.md`
  ("What it decodes"): encoder complexity 0–4 with prediction disabled, so
  no pitch post-filter. Keep every branch such an encoder can trigger:
  mono-coded packets, intensity and dual stereo, transients, anti-collapse,
  silence frames.
* Tables: extend `tools/gen_tables.py`; derived tables are `constexpr`
  lambdas that report failure through `table_generation_failed()`.
* Negation `x * -1`; `vmax`/`vmin` with MAX32/MIN32 semantics; windows via
  `with_misalignment` + `window<S>`.
* Each header starts with its libopus origin and the BSD-3-Clause notice.

Then `opuspp-verify` (`--quick`, then full) before any vectorising or
optimising.

## Upgrading libopus (1.6.1 → newer)

1. Unpack the new release next to `opus-1.6.1/` at the repository root and
   diff the decoder paths
   (`celt/{celt_decoder,bands,vq,quant_bands,rate,cwrs,entdec,entcode,laplace,pitch,celt_lpc,celt,kiss_fft,mdct,mathops}.[ch]`,
   `celt/x86/pitch_sse*`, `src/opus_decoder.c`, `src/opus.c`).
2. Port each hunk that affects the float CELT path under the stream contract.
3. Re-run `tools/gen_tables.py <new-dir>`; point `OPUSPP_LIBOPUS_DIR` (or the
   default path) at the new release.
4. Re-verify the stream contract (config 30 for every packet with audio) with
   the new encoder.
5. Update the pinned version and hash in `tools/fetch_deps.sh`, run the full
   `opuspp-verify`, then `opuspp-bench`, and update the docs.
