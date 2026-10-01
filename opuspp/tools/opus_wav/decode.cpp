// Decodes an .opk packet file (from opuspp_encode) to a 32-bit float stereo
// WAV, with the encoder lookahead removed so the output lines up with
// figaro_original.wav sample for sample.
//
// Usage: opuspp_decode <in.opk> <out.wav>
//
// The decoder is chosen at compile time, which also makes this the program
// tools/profile_report.py measures:
//   (default)          opuspp
//   -DDECODE_LIBOPUS   libopus opus_decode_float (same output format)
//   -DDECODE_STUB      no decoder (silence): the size baseline of the harness
#include <cstdio>
#include <vector>

#include "opk.hpp"

#if defined(DECODE_LIBOPUS)
#include <opus.h>
#elif !defined(DECODE_STUB)
#include "opuspp/opuspp.hpp"
#endif

int main(int argc, char** argv) {
   if (argc != 3) {
      std::fprintf(stderr, "usage: %s <in.opk> <out.wav>\n", argv[0]);
      return 2;
   }
   const opk::Stream s = opk::load(argv[1]);
   std::vector<float> out(s.packets.size() * 960);
   float* pcm = out.data();

#if defined(DECODE_LIBOPUS)
   int err;
   OpusDecoder* dec = opus_decoder_create(48000, 2, &err);
   if (err != OPUS_OK) return 1;
   for (const auto& p : s.packets) {
      const int n = p.empty() ? opus_decode_float(dec, nullptr, 0, pcm, 480, 0)
                              : opus_decode_float(dec, p.data(), opus_int32(p.size()), pcm, 480, 0);
      if (n != 480) return 1;
      pcm += 960;
   }
   opus_decoder_destroy(dec);
#elif !defined(DECODE_STUB)
   static opuspp::Decoder dec;
   static opuspp::AudioBuffer buf;
   for (const auto& p : s.packets) {
      if (p.empty())
         dec.decode_lost(buf);
      else if (dec.decode(p.data(), p.size(), buf) != opuspp::Error::ok)
         return 1;
      for (float x : buf) *pcm++ = x;
   }
#endif

   const std::size_t skip = 2 * std::size_t(s.lookahead);
   const std::size_t n = 2 * std::size_t(s.frames);
   if (out.size() < skip + n) return 1;
   opk::write_wav(argv[2], out.data() + skip, n);
   return 0;
}
