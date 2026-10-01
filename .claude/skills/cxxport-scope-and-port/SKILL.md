---
name: cxxport-scope-and-port
description: Scope the port of a versatile C audio library (codec, DSP) to one specific use case, and translate its modules to C++ bit-exactly - measure what the producer really emits, resolve the #ifdefs as the reference build does, hardcode what the use case makes constant, keep the float evaluation order, generate tables instead of pasting them. Use when starting a port, porting a module, or picking up an upstream change of the reference library.
---

# Scope the use case, then port module by module

## 1. Scope: specialise to what really happens

* **Write down the use case** as the configuration of whatever produces the
  input (for a decoder: the encoder settings; for a filter: the sample rates,
  block sizes and channel layouts used), not as a list of features to drop.
* **Measure the input space empirically.** Write a scratch program that runs
  the real producer over its whole parameter range and histograms what
  comes out (for a decoder: run the encoder over all bitrates, rate control
  modes, DTX, channel modes and tiny buffers, and histogram the packet
  headers). Expect surprises: a configuration can emit packet types nobody
  expected, for example only on packets without audio, which then turn out
  to be an error case rather than a feature to implement.
* **Verify every assumed encoder guarantee with real streams,** including
  synthetic extremes (pure tones, silence, noise, transients), not just by
  reading the encoder source or measuring typical audio. A feature gate that
  looks unconditional in one code path (say, "only at complexity 5 and
  above") can be bypassed by another (a tone detector that enables the same
  tool at any complexity). Pin the guarantee to the setting that really
  switches the tool off, and make the decoder reject anything outside it.
* **Count a path before deleting it.** That a setting disables a tool at
  the producer doesn't mean the consumer never runs the matching path:
  count how often the path runs in an instrumented scratch copy of the
  port over the test inputs. Count separately the cases where the
  controlling flag isn't transmitted at all. They take the default value,
  which can be exactly the path the setting was meant to remove. Run the
  count against the deterministic reference, so unrelated differences don't
  show up as failures.
* **Define the acceptance table:** for every input class, process, handle as
  an error case the way the reference does (for a decoder: conceal), reject
  as unsupported, or reject as malformed (exactly where the reference
  rejects). On rejection, state and output must stay untouched.
* **Hardcode** everything the scope makes constant (sample rate, frame size,
  channel count, band layout, FFT sizes) as named constants in one config
  header. Never repeat the literals.
* **Keep every branch the input can trigger,** even rare ones (in a codec:
  mono-coded frames, transients, silence frames, optional post-processing).
  Delete only branches that are impossible under the measured input space.

## 2. Read the reference as the compiler sees it

Resolve the `#ifdef`s exactly as the reference build does, then read the
preprocessed file instead of the raw source:

```sh
gcc -E -P -D<LIB>_BUILD -DVAR_ARRAYS -I<ref>/include -I<ref>/<module-dir> -I<ref> \
    <ref>/<module-dir>/<file>.c | less
```

Check which SIMD kernels the reference build actually calls (run-time
dispatch, see `cxxport-bit-exact-oracle`).

## 3. Translate

* **Keep the float evaluation order exactly:** operand order,
  parenthesisation, temporaries, and accumulation type (float, not double).
  Fixed-point-style macros (`MULT16_32_Q15` and friends) reduce to plain float
  operations in a float build: expand them by hand.
* **Integer code is ported literally** (entropy coding, bit allocation,
  combinatorial coding), with the same signedness, widths and shifts.
* **Tables are generated, never pasted.** A script extracts the initialisers
  from the preprocessed reference sources; derived tables are computed by
  `constexpr` lambdas from the generated ones. Re-running the generator must
  reproduce the committed file byte for byte.
* **No heap, no libc:** state lives in one object; scratch arrays are
  `alignas(64)` locals sized for the worst case (see `cxxport-freestanding`).
* **One header per reference module**, starting with a comment naming the
  module it ports and the reference's copyright notice.
* **Port in dependency order,** leaves first (in a transform codec, for
  example: bit allocation → band decoding → transforms → filters → the
  top-level decoder → packet parsing), with plain scalar loops, and run the
  differential test after each module.

## 4. Only then vectorise and optimise

`cxxport-portable-simd`, `cxxport-codegen-audit`, `cxxport-perf-and-measure`,
keeping it bit-exact at every step.

## Picking up an upstream change of the reference

1. Put the new release next to the old one and diff the files on the ported
   paths.
2. Port each hunk that affects the scoped path.
3. Re-run the table generator; point the build at the new reference.
4. Re-measure the input space with the new producer.
5. Run the full verify and the benchmarks; update the pinned version and
   hash in the docs.
