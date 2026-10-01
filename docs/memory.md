# Memory

* **Optional heap:** opuspp's whole state is one 18.1 KB `Decoder` object, against
  26.6 KB for a stereo libopus decoder.
* **Less stack in normal decoding:** 3.8–6.5 KB per stereo-coded packet,
  against 8.4–9.5 KB for libopus built the same way. Only for mono-coded
  packets with GCC Balanced and Fast does libopus need less.
* **Packet loss concealment:** with GCC within 0.5 KB of libopus, with Clang
  1.9–2.4 KB less (9.6–12.1 KB against 9.5–14.3 KB).

| | opuspp | libopus |
|---|---|---|
| Decoder state | 18,560 bytes (`sizeof(opuspp::Decoder)`) | 27,236 bytes (`opus_decoder_get_size(2)`) |
| Heap | optionally for the decoder object | optionally for the decoder state |
| Peak stack, normal decoding | 3.8–6.5 KB | 8.4–9.5 KB |
| Peak stack, with concealment | 9.6–12.1 KB | 9.5–14.3 KB |
| Output | caller-owned `AudioBuffer`, 3840 bytes, 64-byte aligned, also used as scratch | caller-owned `float` array, alignment up to the caller |

## Stack per decoding path

Peak stack in bytes, opuspp / libopus, both built with the build profile of
[performance.md](performance.md#build-profiles):

| Path | GCC Size | GCC Balanced | GCC Fast | Clang Size | Clang Balanced | Clang Fast |
|---|---|---|---|---|---|---|
| 510 kb/s stereo | 3,920 / 8,616 | 5,504 / 8,616 | 6,648 / 9,720 | 5,736 / 8,620 | 5,568 / 8,676 | 5,328 / 9,064 |
| 128 kb/s stereo | 3,952 / 8,640 | 5,504 / 8,624 | 6,648 / 9,728 | 5,736 / 8,672 | 5,584 / 8,784 | 5,328 / 9,136 |
| 64 kb/s mono-coded | 3,920 / 5,240 | 5,504 / 5,224 | 6,648 / 6,072 | 4,968 / 5,212 | 4,736 / 6,068 | 4,432 / 5,640 |
| 128 kb/s with isolated losses (pitch concealment) | 9,880 / 9,824 | 10,272 / 9,824 | 10,080 / 10,112 | 12,064 / 14,192 | 12,384 / 14,416 | 12,192 / 14,656 |
| 128 kb/s, then a long loss (noise concealment) | 9,880 / 9,760 | 10,272 / 9,792 | 10,080 / 10,048 | 12,064 / 14,144 | 12,384 / 14,368 | 12,192 / 14,608 |

Normal decoding of stereo-coded packets needs less stack than libopus in
every build. Decoding a packet first produces its frequency-domain
coefficients (960 floats, 3.75 KB), which are then transformed into audio.
libopus keeps them in a stack buffer; opuspp keeps them in the caller's
`AudioBuffer`, which has exactly that size and isn't needed until the final
audio is written to it. libopus needs less for mono-coded packets, which
halve that buffer, so with GCC Balanced and Fast it is below opuspp there.

Packet loss concealment needs more stack than normal decoding because it has
no packet to decode: it rebuilds the audio from up to 1,024 samples of the
decoder's history per channel (pitch search, excitation and filtering), where
normal decoding works on one 480-sample frame. How opuspp's concealment
scratch is laid out is described in
[how-claude-made-it.md](how-claude-made-it.md#stack-and-concealment-scratch).

## How it was measured

State sizes: `sizeof(opuspp::Decoder)` and `opus_decoder_get_size(2)` on
x86-64, the same in every build; libopus in its default configuration
(which includes the SILK decoder state).

Stack: `tools/profile_report.py` (paths relative to `opuspp/`) builds
`tests/stack_usage.cpp` in each build profile, with the same flags and the
same libopus build (default configuration, variable-length arrays) as the
speed and code size numbers. It runs each decoder on a thread whose stack
was painted with a pattern; the deepest overwritten byte gives the peak,
and the cost of starting the thread (an empty run) is subtracted.
