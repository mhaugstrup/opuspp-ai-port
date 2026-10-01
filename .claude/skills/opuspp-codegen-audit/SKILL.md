---
name: opuspp-codegen-audit
description: Audit opuspp generated code - per-loop vectorisation verdicts (GCC and Clang) and unaligned vector memory accesses per source line - and fix findings without breaking bit-exactness. Use after adding or changing loops, buffers or simd code in opuspp, or when align_report/verify.sh stage 5 fails.
---

# Codegen audit for opuspp

Paths below are relative to `opuspp/`. The method, the acceptance reasons for
scalar loops and the table of causes and fixes are in the
`cxxport-codegen-audit` skill; opuspp ships its own copies of the scripts.

```sh
python3 tools/vec_report.py                      # baseline x86-64
python3 tools/vec_report.py -march=x86-64-v3     # AVX2
python3 tools/align_report.py g++ -O2 -march=x86-64-v3
python3 tools/align_report.py clang++ -O3
```

Target: 0 unaligned for every combination in `tools/verify.sh` stage 5;
Clang with `-march=x86-64-v4` is the documented exception.

opuspp's tools for fixes:

* `with_misalignment(p, [&]<int R>() {...})` and `window<S>(a, b)` /
  `window<S>(a, b, c)` in `detail/simd.hpp` for sliding windows;
* `band_stride`, `decode_mem_stride` padding and `buffer_alignment` in
  `detail/config.hpp`;
* `assume_aligned<buffer_alignment>(p)` and `[[assume(...)]]`;
* `copy_aligned<64>`, `fill_aligned<64>`, `move_down_aligned<64>` for bulk
  moves; `CeltDecoder::State::clear()` for struct zeroing;
* `OPUSPP_NO_VECTORIZE` for loops that must stay scalar, with a comment.

SIMD width census (generic script, see `cxxport-codegen-audit`), on a Fast
build of the profiling driver:

```sh
python3 ../.claude/skills/cxxport-codegen-audit/scripts/width_report.py <build>/opuspp_profile \
    --symbols 'opuspp::' [--perf w.data]
```

At Fast on a Ryzen 5 9600X, 128-bit vector instructions take only about
10–15 % of opuspp's cycles at 128 and 510 kb/s; most time is serial integer
work, so wider element-wise kernels could save at most a few percent there.

After any fix, run `opuspp-verify` (`--quick` at least).
