# Verification

opuspp's output is bit-identical to libopus's for every supported stream:
the same samples and the same final range-coder state on every packet,
including packet loss concealment.

| Check | How | Result |
|---|---|---|
| Bit-exact decoding | 47 encoder configurations, decoded by libopus and opuspp | identical samples and final range on every packet |
| Robustness | 300k corrupted, truncated and random packets fed to both decoders in lockstep | same accept/reject decision and output; no difference over 171k decoded frames |
| Unsupported streams | every packet outside the supported stream | rejected (confirmed independently with libopus), except packets without audio data, which are concealed as in libopus |
| Memory safety | the above under ASan and UBSan | clean |
| Build matrix | GCC and Clang, -O0 to -O3 and -Os, both SIMD backends, x86-64-v2/v3/v4 | bit-exact everywhere |
| Freestanding | an object built with `-ffreestanding -fno-exceptions -fno-rtti` | no external symbols with GCC and Clang at every level |

`opuspp/tools/verify.sh` runs all of these.

## The test streams

`tests/compare.cpp` encodes synthetic signals with libopus, using the
supported encoder settings (see the [README](../README.md#constraints)), in
47 configurations:

* 6–510 kb/s VBR, CBR and constrained VBR;
* encoder complexity 0–4;
* forced mono and stereo;
* DTX;
* 1 kb/s and a 2-byte encoder buffer (packets without audio data), and a
  3-byte buffer (the smallest packets with audio data);
* 5–30 % burst packet loss.

Each stream is decoded with both `opus_decode_float` and opuspp.
`tests/fuzz.cpp` mutates encoded packets (bit flips, truncation, random
payloads, re-framing, random headers, losses) and checks that both decoders
stay in lockstep, and that everything outside the supported stream is
rejected.

## Expected deviations

The reference is libopus built for the x86-64 baseline (SSE kernels). Other
builds differ at float-rounding level:

* **Default x86 libopus builds** pick an AVX2+FMA pitch kernel at run time
  on CPUs that support it. It only affects concealment, so the reference
  itself conceals differently on different CPUs (about 86 dB SNR against
  opuspp with 10 % loss). Normal decoding is still bit-identical.
* **FMA contraction:** compiled with FMA enabled (for example
  `-march=native`), normal frames differ at about 135 dB SNR, with the final
  range still exact. Add `-ffp-contract=off` for bit-exact output. The
  reference has the same sensitivity.
* **`OPUSPP_FLOAT_APPROX`** is bit-identical to libopus built with
  `FLOAT_APPROX` in all 47 configurations (test `compare_approx`). With
  `-ffast-math` on top, it is about 137 dB SNR on normal frames and 62 dB in
  concealment, with the final range still exact.

Paths to project files are relative to `opuspp/`.
