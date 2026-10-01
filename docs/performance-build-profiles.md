# Performance by build profile

* **With GCC, optimisation pays off more for opuspp than for libopus:**
  Balanced is 1.52× faster than Size for opuspp and 1.30× for libopus; Fast
  is 1.63× and 1.45×.
* **With Clang, the build profile hardly matters** for either decoder
  (within 4 % of Size).
* **The like-for-like advantage peaks at Balanced** with both compilers: the
  step from Balanced to Fast helps libopus more than opuspp.

## Each decoder against its own Size build

Speed relative to the same decoder built with the Size profile (the profiles
are defined in [performance.md](performance.md#build-profiles)):

| Decoder | GCC Size | GCC Balanced | GCC Fast | Clang Size | Clang Balanced | Clang Fast |
|---|---|---|---|---|---|---|
| opuspp | 1.00× | 1.52× | 1.63× | 1.00× | 1.04× | 1.04× |
| libopus | 1.00× | 1.30× | 1.45× | 1.00× | 1.01× | 1.04× |
| opuspp over libopus, like for like | 1.27× | **1.48×** | 1.43× | 1.42× | **1.46×** | 1.42× |

## Why

* **GCC -Os is slow for both decoders**, most likely because GCC inlines and
  vectorises less at -Os than at -O2; opuspp relies on both more than
  libopus does, so it gains more from -O2. This hasn't been profiled.
* **From Balanced to Fast, libopus gains more.** The likely reasons, not
  measured: opuspp's explicit SIMD is 4-wide, so `-march=native` mostly
  re-encodes the same operations; libopus's plain C gains new
  auto-vectorisation from -O3 and AVX2/AVX-512; and with `-march=native`
  libopus calls its AVX2 kernels directly instead of dispatching at run
  time.
* **With Clang**, the Size build is already within 4 % of the faster
  profiles for both decoders; why Clang's -Os stays this close hasn't been
  examined.

## How it was measured

The same benchmark run and stream as
[performance.md](performance.md#how-it-was-measured): 320 kb/s from an
encoder at complexity 4, best of 7 passes, libopus statically linked. Each
cell is the decoder's time in its Size build divided by its time in that
build; `tools/profile_report.py` (path relative to `opuspp/`) prints the
table. Results vary by a few percent between runs.
