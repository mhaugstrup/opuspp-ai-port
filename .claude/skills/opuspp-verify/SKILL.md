---
name: opuspp-verify
description: Run the opuspp acceptance checks (bit-exact compare vs libopus, fuzz, sanitizers, freestanding symbols, build matrix, alignment) and triage failures. Use after any change to opuspp/include, tests or CMakeLists.txt, and before reporting work as done.
---

# Verify opuspp

Paths below are relative to `opuspp/`. The rules being checked are in
`opuspp/README.md` ("Changing the code"); the general method and triage table
are in the `cxxport-bit-exact-oracle` skill.

## Run

```sh
tools/verify.sh <build-dir> --quick   # about 1.5 min: -O0/-O2 only, 5 s streams, 50k fuzz
tools/verify.sh <build-dir>           # about 5 min: the full matrix; required before "done"
```

Put `<build-dir>` outside the source tree (for example, in the scratchpad).
libopus is found at `../opus-1.6.1` or via `OPUSPP_LIBOPUS_DIR`. The script
stops at the first failing stage and prints `PASS` only when everything
passes. Report the outcome faithfully; if a stage was skipped (for example,
v4 on a CPU without AVX-512), say so.

## opuspp specifics for triage

1. **ctest:** run `opuspp_compare` directly; it prints a per-configuration
   table (packets, lost, rngbad, SNR, max error). `rngbad > 0` points at
   `entdec.hpp`, `rate.hpp` and `bands.hpp`. Loss-only failures point at
   `CeltDecoder::conceal`, `plc_pitch_search` and `prefilter_and_fold`. Bisect
   float mismatches with `-DOPUSPP_NO_VECTOR_EXT`.
2. **Sanitizers:** pad the array, don't shrink the block load.
3. **Freestanding symbols:** `memset`/`memcpy` at -O0 → see
   `CeltDecoder::State::clear()` for the pattern; `sqrtf` → the guards in
   `celt_sqrt` (`detail/math.hpp`).
4. **Matrix:** `-march` failures are FMA contraction (the matrix passes
   `-ffp-contract=off`).
5. **Alignment:** use the `opuspp-codegen-audit` skill.

## Not automated

* `python3 tools/vec_report.py`: every non-vectorised loop needs a reason.
* `opuspp-bench`: no benchmark case may be slower than libopus.
