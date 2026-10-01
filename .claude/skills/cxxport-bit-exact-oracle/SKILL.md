---
name: cxxport-bit-exact-oracle
description: Build and triage the differential test harness that proves a C++ port bit-exact against its reference C library - pinning a deterministic reference build, comparing output and internal checksums, lockstep fuzzing, sanitizers, the compiler/backend/ISA matrix, FMA and fast-math handling, and a single verify script. Use when setting up the oracle for a port, when a comparison fails, or before reporting work as done.
---

# The bit-exact oracle

## Pin a deterministic reference

* **Build the reference without run-time CPU dispatch.** Many libraries pick
  a kernel (for example an AVX2+FMA one) at run time, so the reference's own
  output can differ between CPUs, often only on a rarely used path. Build it
  with that dispatch switched off (its baseline SIMD level), and check every
  reference for this before trusting it.
* **Pin the reference by version and hash**, fetched by a script, never
  committed or edited.
* **Build matching references for every float mode** the port supports (for
  example the reference's approximation or fast-math build), and compare each
  mode against its own reference.

## The differential test

* Drive both implementations through the **public API only**, so the test
  survives refactoring and can be written before the port (with a stub).
* Generate input across the whole scoped parameter space, with the
  reference's own tools where the input is itself produced by the library
  (for a decoder: encode with the reference encoder over bitrates, rate
  control modes, complexities, channel modes, DTX and tiny buffers), and
  include the **error paths** the use case hits (for a decoder: packet loss
  patterns). Use a synthetic signal with tones, chirps, noise, clicks,
  silence and stereo differences, from a fixed seed.
* **Compare bitwise**, not by SNR, and compare an **internal checksum** per
  processed block as well where the library exposes one (for an
  entropy-coded format: the range coder's final state per packet). A checksum mismatch means the integer or
  bitstream logic diverged; an output mismatch with a matching checksum
  means float arithmetic diverged.
* Print one line per configuration (blocks, error cases, checksum
  mismatches, SNR, max error) and a final `PASS`/`FAIL` line; exit non-zero
  on failure.

## The fuzzer

Feed corrupted input (bit-flipped, truncated, random, re-framed, with random
headers, plus the error cases such as losses) to both implementations **in
lockstep**: same accept/reject decision, same output, same state afterwards.
Also assert that every out-of-scope input class is rejected. Run it under
ASan and UBSan.

## The verify script: one command decides "done"

Stages, stopping at the first failure:

1. Release build and the test suite.
2. Differential test and fuzzer under ASan+UBSan.
3. Freestanding object: `nm -u` empty (see `cxxport-freestanding`).
4. **The matrix:** differential test with GCC and Clang × -O0..-O3, -Os ×
   both SIMD backends, plus ISA levels (x86-64-v2/v3/v4) with
   `-ffp-contract=off`.
5. Alignment: 0 unaligned vector accesses (see `cxxport-codegen-audit`).

Offer a `--quick` mode (fewer levels, shorter streams) for iterating; the full
run is required before reporting. Name the manual checks the script doesn't
cover in its header comment.

## Expected, documented deviations

* **FMA contraction** (`-march=native` and friends) changes rounding slightly,
  with integer checksums still exact. The reference has the same
  sensitivity. Bit-exactness is required with `-ffp-contract=off`.
* **Fast math** is accepted only together with the reference's approximation
  mode, and NaN guards must test the bit pattern so fast math can't remove
  them.

## Triage

| Symptom | Where to look |
|---|---|
| Checksum mismatch | Integer paths (for a codec: entropy coding, bit allocation, combinatorial coding) |
| Checksum fine, output differs | Float order: find the first differing block and sample, then bisect by swapping a vector kernel for its scalar form or building the plain-array SIMD backend |
| Only error-case configurations fail | The error-handling path (for a decoder: concealment) |
| Fails only with `-march=...` | FMA contraction; check `-ffp-contract=off` |
| Fails only with one SIMD backend | The backends disagree on an operation's semantics (negation of zero, min/max of NaN, shuffles) |
| Fails at one -O level only | Undefined behaviour or reliance on evaluation order |
| ASan read overflow | A block load past an array end: pad the array to a whole vector, don't shrink the load |

## Proving replacements exact

Any helper replaced by a faster one (lookup table, hardware sqrt) is checked
against the original **over its whole input domain** in a scratch test, not
by sampling. Mind undefined inputs (an integer square root of 0, say): start
the exhaustive range where the original is defined.
