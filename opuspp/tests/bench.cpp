// Decoder throughput benchmark: reference libopus (opus_decode_float) vs opuspp
// on identical pre-encoded streams (RESTRICTED_CELT, fullband, 10 ms).
// Usage: opuspp_bench [seconds=60] [passes=7] [segments=tone,noise,transients,music] [cases]
//   cases: comma-separated from 128, 256, 320, 510, loss (320 kb/s, 10 % random
//   loss) and plc (concealment only: one good packet, then all lost); default
//   128,320,510,loss. "plc" isolates the concealment path, e.g. for profiling.
//
// Each stream is encoded once into a 64-byte aligned packet arena. Every timed
// pass of either decoder starts with the arena flushed from the CPU caches, so
// neither decoder gains from packets that an earlier pass left in cache.
#include <opus.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include "bench_signal.hpp"
#include "bench_stream.hpp"
#include "opuspp/opuspp.hpp"

namespace {

bench::Stream encode(const std::vector<float>& sig, int bitrate, int complexity, int loss_percent) {
   int err;
   OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_CELT, &err);
   opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
   opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
   opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
   opus_encoder_ctl(enc, OPUS_SET_PREDICTION_DISABLED(1));  // supported streams (with complexity 0-4)
   bench::Stream out;
   std::uint32_t r = 12345;
   unsigned char buf[1500];
   for (std::size_t f = 0; f + 960 <= sig.size(); f += 960) {
      const int len = opus_encode_float(enc, &sig[f], 480, buf, sizeof(buf));
      r = r * 1664525 + 1013904223;
      // 100 % = concealment only: one good packet to prime the decoder, then all lost.
      const bool lost = loss_percent == 100 ? f > 0 : loss_percent && int((r >> 16) % 100) < loss_percent;
      if (lost) out.add_lost();
      else out.add(buf, std::size_t(len));
   }
   opus_encoder_destroy(enc);
   return out;
}

// Evicts the packet arena from all cache levels.
void flush(const bench::Stream& s) {
#if defined(__x86_64__) || defined(__i386__)
   for (const bench::Block& b : s.arena) _mm_clflush(b.bytes);
   _mm_mfence();
#else
   // No portable cache-line flush: sweep a buffer larger than the last-level cache.
   static std::vector<unsigned char> sweep(std::size_t(256) << 20);
   for (std::size_t i = 0; i < sweep.size(); i += 64) sweep[i]++;
   (void)s;
#endif
}

using clk = std::chrono::steady_clock;

// Returns the best (minimum) time of `reps` passes, in seconds. The packets
// are flushed from the caches before each pass, outside the timed region.
template <class F>
double best_of(int reps, const bench::Stream& s, F&& pass) {
   double best = 1e30;
   for (int i = 0; i < reps; i++) {
      flush(s);
      const auto t0 = clk::now();
      pass();
      best = std::min(best, std::chrono::duration<double>(clk::now() - t0).count());
   }
   return best;
}

volatile float sink;

}  // namespace

int main(int argc, char** argv) {
   const int seconds = argc > 1 ? std::atoi(argv[1]) : 60;
   const int reps = argc > 2 ? std::atoi(argv[2]) : 7;
   const char* segments = argc > 3 ? argv[3] : bench::default_segments;
   const char* case_list = argc > 4 ? argv[4] : "128,320,510,loss";

   struct Case {
      const char* key;
      const char* name;
      int bitrate;
      int loss;
   };
   static constexpr Case all_cases[] = {{"128", "128 kb/s", 128000, 0},       {"256", "256 kb/s", 256000, 0},
                                        {"320", "320 kb/s", 320000, 0},       {"510", "510 kb/s", 510000, 0},
                                        {"loss", "320k 10% loss", 320000, 10}, {"plc", "PLC only", 320000, 100}};
   std::vector<Case> cases;
   for (std::string_view rest = case_list; !rest.empty();) {
      const std::size_t comma = rest.find(',');
      const std::string_view key = rest.substr(0, comma);
      rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
      const auto it = std::find_if(std::begin(all_cases), std::end(all_cases),
                                   [&](const Case& c) { return key == c.key; });
      if (it == std::end(all_cases)) {
         std::fprintf(stderr, "unknown case '%.*s' (use 128, 256, 320, 510, loss, plc)\n", int(key.size()),
                      key.data());
         return 2;
      }
      cases.push_back(*it);
   }

   const auto sig = bench::make_signal(seconds, bench::parse_segments(segments));
   std::printf("%d s of audio per pass (%s), best of %d passes\n", seconds, segments, reps);
   std::printf("%-22s %12s %12s %9s\n", "stream", "libopus ms", "opuspp ms", "speedup");

   double total_ref = 0, total_pp = 0;
   // Encoder complexity 0, 2 and 4: no, fixed and adaptive spreading (opuspp
   // supports streams from encoders at complexity 0-4 with prediction disabled).
   for (const int cx : {0, 2, 4}) {
      double sub_ref = 0, sub_pp = 0;
      for (const Case& c : cases) {
         const bench::Stream s = encode(sig, c.bitrate, cx, c.loss);
         char name[64];
         std::snprintf(name, sizeof(name), "cx%-2d %s", cx, c.name);

         int err;
         OpusDecoder* ref = opus_decoder_create(48000, 2, &err);
         alignas(64) float pcm[960];
         const double t_ref = best_of(reps, s, [&] {
            opus_decoder_ctl(ref, OPUS_RESET_STATE);
            for (const bench::Packet& p : s.packets) {
               const int n = p.size == 0 ? opus_decode_float(ref, nullptr, 0, pcm, 480, 0)
                                         : opus_decode_float(ref, s.data(p), opus_int32(p.size), pcm, 480, 0);
               if (n != 480) std::abort();
            }
            sink = pcm[0];
         });
         opus_decoder_destroy(ref);

         auto dec = std::make_unique<opuspp::Decoder>();
         auto buf = std::make_unique<opuspp::AudioBuffer>();
         const double t_pp = best_of(reps, s, [&] {
            dec->reset();
            for (const bench::Packet& p : s.packets) {
               if (p.size == 0) dec->decode_lost(*buf);
               else if (dec->decode(s.data(p), p.size, *buf) != opuspp::Error::ok) std::abort();
            }
            sink = buf->samples[0];
         });

         sub_ref += t_ref;
         sub_pp += t_pp;
         std::printf("%-22s %12.1f %12.1f %8.2fx\n", name, 1e3 * t_ref, 1e3 * t_pp, t_ref / t_pp);
      }
      std::printf("%-22s %12.1f %12.1f %8.2fx\n\n", (std::string("cx") + std::to_string(cx) + " subtotal").c_str(),
                  1e3 * sub_ref, 1e3 * sub_pp, sub_ref / sub_pp);
      total_ref += sub_ref;
      total_pp += sub_pp;
   }
   std::printf("%-22s %12.1f %12.1f %8.2fx\n", "total", 1e3 * total_ref, 1e3 * total_pp, total_ref / total_pp);
   return 0;
}
