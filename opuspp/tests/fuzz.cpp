// Robustness test: feeds corrupted, truncated and random packets to both the
// reference decoder and opuspp and checks that they stay in lockstep, plus a
// check that unsupported packet types are rejected.
#include <opus.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "opuspp/opuspp.hpp"

namespace {

struct Rng {
   std::uint64_t s;
   std::uint32_t next() {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      return std::uint32_t(s >> 33);
   }
};

int failures = 0;

void check(bool ok, const char* what, long iter) {
   if (!ok) {
      if (failures < 20) std::printf("FAIL: %s (iteration %ld)\n", what, iter);
      failures++;
   }
}

// Encodes a stream of valid packets to use as mutation seeds.
std::vector<std::vector<unsigned char>> seed_packets() {
   int err;
   std::vector<std::vector<unsigned char>> out;
   const int rates[] = {8000, 32000, 128000, 510000};
   for (int br : rates) {
      for (int ch = 1; ch <= 2; ch++) {
         OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_CELT, &err);
         opus_encoder_ctl(enc, OPUS_SET_BITRATE(br));
         opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
         opus_encoder_ctl(enc, OPUS_SET_FORCE_CHANNELS(ch));
         opus_encoder_ctl(enc, OPUS_SET_VBR(br != 32000));
         // Supported streams: encoder complexity 0-4, prediction disabled.
         opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(int(br / 1000 % 5)));
         opus_encoder_ctl(enc, OPUS_SET_PREDICTION_DISABLED(1));
         Rng rng{std::uint64_t(br + ch)};
         float pcm[960];
         double ph = 0;
         for (int f = 0; f < 200; f++) {
            for (int i = 0; i < 480; i++) {
               ph += 0.05 + 0.02 * std::sin(f * 0.1);
               pcm[2 * i] = float(0.5 * std::sin(ph) + 0.1 * (int(rng.next() % 2001) - 1000) / 1000.0);
               pcm[2 * i + 1] = float(0.4 * std::sin(1.3 * ph));
            }
            unsigned char pkt[1500];
            const int len = opus_encode_float(enc, pcm, 480, pkt, sizeof(pkt));
            out.emplace_back(pkt, pkt + len);
         }
         opus_encoder_destroy(enc);
      }
   }
   return out;
}

}  // namespace

int main(int argc, char** argv) {
   const long iterations = argc > 1 ? std::atol(argv[1]) : 200000;
   int err;
   OpusDecoder* ref = opus_decoder_create(48000, 2, &err);
   // Decodes single packets from a fresh state, to find out whether a packet
   // enables the pitch post-filter (OPUS_GET_PITCH > 0 afterwards), which
   // supported streams never do.
   OpusDecoder* probe = opus_decoder_create(48000, 2, &err);
   auto uses_postfilter = [&](const unsigned char* d, int n) {
      float tmp[960];
      opus_decoder_ctl(probe, OPUS_RESET_STATE);
      if (opus_decode_float(probe, d, n, tmp, 480, 0) != 480) return false;
      opus_int32 pitch = 0;
      opus_decoder_ctl(probe, OPUS_GET_PITCH(&pitch));
      return pitch > 0;
   };
   auto dut = std::make_unique<opuspp::Decoder>();
   const auto seeds = seed_packets();
   Rng rng{7};
   float a[960];
   opuspp::AudioBuffer b;
   long compared = 0, rejected = 0, unsupported = 0;

   for (long it = 0; it < iterations; it++) {
      std::vector<unsigned char> p = seeds[rng.next() % seeds.size()];
      switch (rng.next() % 8) {
      case 0:  // unmodified
         break;
      case 1:  // flip a few payload bits
         for (int k = 0, n = 1 + int(rng.next() % 8); k < n && p.size() > 1; k++)
            p[1 + rng.next() % (p.size() - 1)] ^= std::uint8_t(1u << (rng.next() % 8));
         break;
      case 2:  // truncate
         p.resize(1 + rng.next() % p.size());
         break;
      case 3:  // random payload of random length, keeping the TOC
         p.resize(1 + rng.next() % 300);
         for (std::size_t i = 1; i < p.size(); i++) p[i] = std::uint8_t(rng.next());
         break;
      case 4:  // code 3 framing with random padding / count byte
         if (p.size() > 1) {
            std::vector<unsigned char> q;
            q.push_back(std::uint8_t((p[0] & ~3u) | 3u));
            q.push_back(std::uint8_t(rng.next()));
            q.insert(q.end(), p.begin() + 1, p.end());
            p = q;
         }
         break;
      case 5:  // loss
         p.clear();
         break;
      case 6:  // random TOC byte
         p[0] = std::uint8_t(rng.next());
         break;
      case 7:  // TOC-only packet with a random TOC (DTX / "PLC" packets)
         p.assign(1, std::uint8_t(rng.next()));
         break;
      }
      const bool lost = p.empty();
      const opuspp::Error de = lost ? opuspp::Error::ok : dut->decode(p.data(), p.size(), b);
      if (lost) dut->decode_lost(b);
      if (de == opuspp::Error::unsupported_packet) {
         // Must be outside the supported stream: not a 10 ms frame, more than
         // one frame, or audio data in anything but a fullband CELT packet.
         const int toc = p[0];
         const bool is_10ms = (toc & 0x80) ? ((toc >> 3) & 3) == 2
                              : (toc & 0x60) == 0x60 ? (toc & 8) == 0
                                                     : ((toc >> 3) & 3) == 0;
         const bool multi = (toc & 3) == 1 || (toc & 3) == 2 || ((toc & 3) == 3 && p.size() > 1 && (p[1] & 0x3F) > 1);
         check(!is_10ms || multi || (toc >> 3) != 30 || uses_postfilter(p.data(), int(p.size())),
               "fullband 10 ms packet without post-filter reported unsupported", it);
         unsupported++;
         continue;  // the reference would decode it, so keep it out of the lockstep
      }
      const int rn = lost ? opus_decode_float(ref, nullptr, 0, a, 480, 0)
                          : opus_decode_float(ref, p.data(), opus_int32(p.size()), a, 480, 0);
      if (rn == 480) {
         check(de == opuspp::Error::ok, "reference decoded but opuspp failed", it);
         if (de == opuspp::Error::ok) {
            check(std::memcmp(a, b.samples, sizeof(a)) == 0, "output mismatch", it);
            opus_uint32 r;
            opus_decoder_ctl(ref, OPUS_GET_FINAL_RANGE(&r));
            check(r == dut->final_range(), "final range mismatch", it);
            compared++;
         }
      } else {
         // Reference rejected it: opuspp must reject it too.
         check(de != opuspp::Error::ok, "reference failed but opuspp succeeded", it);
         check(!(rn == OPUS_INVALID_PACKET && de != opuspp::Error::invalid_packet), "error code class mismatch", it);
         rejected++;
      }
   }

   // Unsupported packets: SILK, hybrid, CELT with 2.5/5/20 ms frames,
   // multi-frame packets. A fresh opuspp decoder must reject them all.
   {
      opuspp::Decoder d;
      const unsigned char payload[40] = {0x55};
      auto rejects = [&](unsigned char toc, int extra) {
         unsigned char pkt[64] = {toc};
         int n = 1;
         if ((toc & 3) == 3) pkt[n++] = std::uint8_t(extra);
         std::memcpy(pkt + n, payload, 40);
         return d.decode(pkt, std::size_t(n + 40), b) == opuspp::Error::unsupported_packet;
      };
      for (int config = 0; config < 32; config++) {
         const bool supported = config == 30;
         if (!supported) check(rejects(std::uint8_t(config << 3), 0), "unsupported config accepted", config);
      }
      check(rejects(std::uint8_t(30 << 3 | 1), 0), "code 1 accepted", 0);
      check(rejects(std::uint8_t(30 << 3 | 2), 0), "code 2 accepted", 0);
      check(rejects(std::uint8_t(30 << 3 | 3), 2), "code 3 with 2 frames accepted", 0);
   }

   // Streams from an encoder with prediction enabled (complexity 10): exactly
   // the packets that enable the post-filter must be rejected, the others
   // decoded.
   {
      OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_CELT, &err);
      opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
      opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
      opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
      Rng srng{11};
      float pcm[960];
      double ph = 0;
      int pf = 0, plain = 0;
      for (int f = 0; f < 400; f++) {
         for (int i = 0; i < 480; i++) {
            ph += 0.03;
            pcm[2 * i] = float(0.5 * std::sin(ph) + 0.05 * (int(srng.next() % 2001) - 1000) / 1000.0);
            pcm[2 * i + 1] = float(0.4 * std::sin(1.5 * ph));
         }
         unsigned char pkt[1500];
         const int len = opus_encode_float(enc, pcm, 480, pkt, sizeof(pkt));
         if (len <= 2) continue;
         const bool has_pf = uses_postfilter(pkt, len);
         opuspp::Decoder d;
         const opuspp::Error e = d.decode(pkt, std::size_t(len), b);
         check(has_pf ? e == opuspp::Error::unsupported_packet : e == opuspp::Error::ok,
               "post-filter packet handling", f);
         (has_pf ? pf : plain)++;
      }
      opus_encoder_destroy(enc);
      check(pf > 0 && plain > 0, "complexity-10 stream should mix post-filter and plain packets", 0);
   }

   std::printf("%ld iterations: %ld decoded and compared, %ld rejected by both, %ld unsupported, %d failures\n",
               iterations, compared, rejected, unsupported, failures);
   opus_decoder_destroy(ref);
   opus_decoder_destroy(probe);
   return failures ? 1 : 0;
}
