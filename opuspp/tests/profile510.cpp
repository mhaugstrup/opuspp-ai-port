// Profiling driver: decodes a pre-encoded 510 kb/s stream repeatedly with
// opuspp (or libopus with argument "ref"). Run under `perf record`.
// Usage: opuspp_profile [ref|pp] [bitrate=510000] [passes=20] [segments]
#include <opus.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "bench_signal.hpp"
#include "bench_stream.hpp"
#include "opuspp/opuspp.hpp"

int main(int argc, char** argv) {
   const bool ref = argc > 1 && std::strcmp(argv[1], "ref") == 0;
   const int bitrate = argc > 2 ? std::atoi(argv[2]) : 510000;
   const int passes = argc > 3 ? std::atoi(argv[3]) : 20;
   const int n = 20 * 48000;
   const char* segments = argc > 4 ? argv[4] : bench::default_segments;
   const std::vector<float> sig = bench::make_signal(20, bench::parse_segments(segments));
   int err;
   OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_CELT, &err);
   opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
   opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
   // opuspp supports streams from encoders at complexity 0-4 with prediction disabled.
   opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(4));
   opus_encoder_ctl(enc, OPUS_SET_PREDICTION_DISABLED(1));
   bench::Stream pkts;
   unsigned char buf[1500];
   for (int f = 0; f < n / 480; f++) {
      const int len = opus_encode_float(enc, &sig[960 * f], 480, buf, sizeof(buf));
      pkts.add(buf, std::size_t(len));
   }
   opus_encoder_destroy(enc);

   float sink = 0;
   if (ref) {
      OpusDecoder* dec = opus_decoder_create(48000, 2, &err);
      alignas(64) float pcm[960];
      for (int p = 0; p < passes; p++)
         for (const bench::Packet& pk : pkts.packets) {
            if (opus_decode_float(dec, pkts.data(pk), opus_int32(pk.size), pcm, 480, 0) != 480) return 1;
            sink += pcm[0];
         }
      opus_decoder_destroy(dec);
   } else {
      auto dec = std::make_unique<opuspp::Decoder>();
      auto out = std::make_unique<opuspp::AudioBuffer>();
      for (int p = 0; p < passes; p++)
         for (const bench::Packet& pk : pkts.packets) {
            if (dec->decode(pkts.data(pk), pk.size, *out) != opuspp::Error::ok) return 1;
            sink += out->samples[0];
         }
   }
   std::printf("%g\n", double(sink));
   return 0;
}
