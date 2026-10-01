// Peak stack use of opuspp and the reference libopus decoder, measured at run
// time: each decoder runs on a thread whose stack was painted with a pattern,
// and the deepest overwritten byte gives the peak. The cost of starting the
// thread itself (an empty run) is subtracted.
// Usage: opuspp_stack_usage
#include <opus.h>
#include <pthread.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "opuspp/opuspp.hpp"

namespace {

constexpr std::size_t stack_size = 1 << 20;
constexpr unsigned char paint = 0xA5;
alignas(4096) unsigned char stack[stack_size];

template <class F>
std::size_t raw_peak(F&& work) {
   std::memset(stack, paint, sizeof stack);
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setstack(&attr, stack, sizeof stack);
   pthread_t t;
   auto fn = [](void* p) -> void* {
      (*static_cast<F*>(p))();
      return nullptr;
   };
   pthread_create(&t, &attr, fn, &work);
   pthread_join(t, nullptr);
   pthread_attr_destroy(&attr);
   std::size_t i = 0;
   while (i < sizeof stack && stack[i] == paint) i++;
   return sizeof stack - i;
}

using Packets = std::vector<std::vector<unsigned char>>;

// Tones plus a little noise, encoded with the stream settings opuspp supports.
Packets encode(int bitrate, int force_channels, int frames) {
   int err;
   OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_CELT, &err);
   opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
   opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
   opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(4));
   opus_encoder_ctl(enc, OPUS_SET_PREDICTION_DISABLED(1));
   if (force_channels) opus_encoder_ctl(enc, OPUS_SET_FORCE_CHANNELS(force_channels));
   Packets packets;
   float pcm[960];
   unsigned rng = 1;
   for (int f = 0; f < frames; f++) {
      for (int i = 0; i < 480; i++) {
         const double t = (f * 480 + i) / 48000.0;
         rng = rng * 1664525u + 1013904223u;
         const float noise = (float(rng >> 9) / 8388608.0f - 0.5f) * 0.05f;
         pcm[2 * i] = float(0.3 * std::sin(2 * M_PI * 220 * t) + 0.1 * std::sin(2 * M_PI * 660 * t)) + noise;
         pcm[2 * i + 1] = float(0.3 * std::sin(2 * M_PI * 180 * t)) + noise;
      }
      unsigned char buf[1500];
      const int n = opus_encode_float(enc, pcm, 480, buf, sizeof buf);
      packets.emplace_back(buf, buf + n);
   }
   opus_encoder_destroy(enc);
   return packets;
}

opuspp::Decoder pp_dec;
opuspp::AudioBuffer pp_out;
float ref_out[960];

// loss: 0 = none, 1 = every fifth packet lost (pitch-based concealment),
// 2 = everything lost after one second (pitch-based, then noise-based).
void run(const char* name, const Packets& packets, int loss, std::size_t base) {
   auto lost = [&](std::size_t i) { return loss == 1 ? i % 5 == 4 : loss == 2 ? i >= 100 : false; };
   const std::size_t pp = raw_peak([&] {
      pp_dec.reset();
      for (std::size_t i = 0; i < packets.size(); i++) {
         if (lost(i)) pp_dec.decode_lost(pp_out);
         else (void)pp_dec.decode(packets[i].data(), packets[i].size(), pp_out);
      }
   });
   const std::size_t ref = raw_peak([&] {
      int err;
      OpusDecoder* dec = opus_decoder_create(48000, 2, &err);
      for (std::size_t i = 0; i < packets.size(); i++) {
         const bool l = lost(i);
         const int r = opus_decode_float(dec, l ? nullptr : packets[i].data(), l ? 0 : int(packets[i].size()),
                                         ref_out, 480, 0);
         (void)r;
      }
      opus_decoder_destroy(dec);
   });
   std::printf("%-36s %8zu %8zu\n", name, pp - base, ref - base);
}

} // namespace

int main() {
   const std::size_t base = raw_peak([] {});
   const Packets s510 = encode(510000, 0, 300), s128 = encode(128000, 0, 300), s64m = encode(64000, 1, 300);
   std::printf("Peak stack in bytes (thread start-up, %zu B, subtracted)\n", base);
   std::printf("%-36s %8s %8s\n", "", "opuspp", "libopus");
   run("510 kb/s stereo", s510, 0, base);
   run("128 kb/s stereo", s128, 0, base);
   run("64 kb/s mono-coded", s64m, 0, base);
   run("128 kb/s, isolated losses", s128, 1, base);
   run("128 kb/s, long loss (noise PLC)", s128, 2, base);
}
