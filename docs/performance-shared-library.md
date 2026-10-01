# Performance against a shared libopus

* **libopus performs on par as a shared library:** 0.99× to 1.02× the speed
  of the statically linked library in every build but GCC Size.
* **GCC Size is the one larger difference:** the shared library is 1.03×
  faster. This hasn't been investigated.
* **opuspp's advantage is the same either way:** like for like, 1.24× to
  1.48× over a shared libopus, against 1.27× to 1.48× over a static one.

## Static against shared

| | GCC Size | GCC Balanced | GCC Fast | Clang Size | Clang Balanced | Clang Fast |
|---|---|---|---|---|---|---|
| libopus shared, speed relative to static | 1.03× | 1.00× | 1.02× | 1.00× | 0.99× | 1.00× |
| opuspp over static libopus | 1.27× | 1.48× | 1.43× | 1.42× | 1.46× | 1.42× |
| opuspp over shared libopus | 1.24× | 1.48× | 1.41× | 1.41× | 1.47× | 1.41× |

The first row is libopus's speed linked as a shared library relative to
the statically linked one (above 1 is faster).

## Why

A decoder makes only a few calls into the library per 10 ms packet, and the
work inside it is compiled the same way, so the call overhead of a shared
library doesn't show. Differences of a percent or two in either direction
are within what code placement alone moves results by.

## How it was measured

The same benchmark and stream as
[performance.md](performance.md#how-it-was-measured) (320 kb/s from an
encoder at complexity 4, best of 7 passes), in a second binary. For each
compiler and build profile, `tools/profile_report.py --shared` (path
relative to `opuspp/`) builds libopus twice with the same flags, as a static
library and as `libopus.so`, and links the second benchmark binary against
the shared one. Both binaries contain the same opuspp code; opuspp's times
in the two agree within 0.7 %.
