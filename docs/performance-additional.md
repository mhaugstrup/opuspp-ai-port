# Performance: additional results

* **The speed-up holds across bit rates, packet loss and encoder
  complexities:** like for like, 1.09× to 1.50×.
* **The bit rate matters most:** 320 kb/s gives the largest speed-up in
  every build (with GCC Balanced, tied with 128 kb/s); 128 kb/s, 510 kb/s
  and packet loss give smaller ones, by how much depends on the build.
* **The encoder complexity matters little:** complexity 0 is within 0.11×
  of complexity 4 in every cell, and complexity 2 within 0.02×.
* **The signal matters little:** the music clip alone gives the same
  speed-ups within 3 %.

The [main results](performance.md) are for the 320 kb/s stream at encoder
complexity 4. These tables add the other streams of the same benchmark run.
Each cell is the speed-up of opuspp over libopus built the same way (like
for like).

## Encoder complexity 4

| Stream | GCC Size | GCC Balanced | GCC Fast | Clang Size | Clang Balanced | Clang Fast |
|---|---|---|---|---|---|---|
| 128 kb/s | 1.17× | 1.48× | 1.28× | 1.26× | 1.28× | 1.32× |
| 320 kb/s | 1.27× | 1.48× | 1.43× | 1.42× | 1.46× | 1.42× |
| 510 kb/s | 1.18× | 1.27× | 1.26× | 1.29× | 1.41× | 1.37× |
| 320 kb/s, 10 % loss | 1.09× | 1.38× | 1.38× | 1.33× | 1.36× | 1.31× |
| **all four** | **1.17×** | **1.38×** | **1.33×** | **1.33×** | **1.39×** | **1.36×** |

## Encoder complexity 0

Complexity 0 turns off transient detection, time-frequency changes and
spreading in the encoder, so the decoder takes fewer paths.

| Stream | GCC Size | GCC Balanced | GCC Fast | Clang Size | Clang Balanced | Clang Fast |
|---|---|---|---|---|---|---|
| 128 kb/s | 1.14× | 1.46× | 1.33× | 1.26× | 1.39× | 1.37× |
| 320 kb/s | 1.25× | 1.49× | 1.45× | 1.45× | 1.50× | 1.43× |
| 510 kb/s | 1.14× | 1.28× | 1.26× | 1.31× | 1.45× | 1.38× |
| 320 kb/s, 10 % loss | 1.10× | 1.41× | 1.39× | 1.36× | 1.39× | 1.31× |
| **all four** | **1.15×** | **1.39×** | **1.35×** | **1.35×** | **1.44×** | **1.37×** |

Encoder complexity 2 gives the complexity 4 results within 0.02× (1.09× to
1.49×).

## How it was measured

The same benchmark run as [performance.md](performance.md#how-it-was-measured),
which also encodes 128 and 510 kb/s streams, a 320 kb/s stream with 10 %
random packet loss, and each stream at encoder complexity 0, 2 and 4. The
loss case drops packets independently, so most losses are isolated and each
starts a new concealment. The music-only check decodes the 15 s music
segment on its own, best of 15 passes. `tools/profile_report.py` (path
relative to `opuspp/`) prints every table on this page.
