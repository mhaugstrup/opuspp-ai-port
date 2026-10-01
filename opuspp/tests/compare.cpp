// Differential test: encodes synthetic signals with the reference libopus
// encoder (OPUS_APPLICATION_RESTRICTED_CELT, fullband, 10 ms frames) under many
// configurations, then decodes every stream with both the reference decoder
// (opus_decode_float) and opuspp and compares output and final range.
//
// Build (hosted): see tests/CMakeLists.txt.
#include <opus.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "opuspp/opuspp.hpp"

namespace {

constexpr int fs = 48000;
constexpr int frame = 480;

struct Rng {
   std::uint64_t s;
   std::uint32_t next() {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      return std::uint32_t(s >> 33);
   }
   double uniform() { return next() / 2147483648.0 - 1.0; }
};

// Test signal: tones with vibrato, chirps, noise, clicks, silence and
// stereo differences, cycling through segments.
std::vector<float> make_signal(int seconds, std::uint64_t seed) {
   const int n = seconds * fs;
   std::vector<float> x(2 * n);
   Rng rng{seed};
   double ph[2][6] = {};
   for (int i = 0; i < n; i++) {
      const double t = double(i) / fs;
      const int seg = int(t / 1.5) % 7;
      for (int c = 0; c < 2; c++) {
         double v = 0;
         switch (seg) {
         case 0:  // harmonic tone with vibrato
         case 5: {
            const double f0 = (c ? 180.0 : 220.0) * (1.0 + 0.03 * std::sin(2 * M_PI * 5 * t));
            for (int h = 0; h < 6; h++) {
               ph[c][h] += 2 * M_PI * f0 * (h + 1) / fs;
               v += 0.25 / (h + 1) * std::sin(ph[c][h]);
            }
            break;
         }
         case 1:  // exponential chirp
            ph[c][0] += 2 * M_PI * 100.0 * std::pow(180.0, std::fmod(t, 1.5) / 1.5) / fs;
            v = 0.5 * std::sin(ph[c][0] + c);
            break;
         case 2:  // white noise, different per channel
            v = 0.3 * rng.uniform();
            break;
         case 3:  // clicks / transients
            v = (i % 4800 < 30) ? 0.8 * rng.uniform() : 0.001 * rng.uniform();
            break;
         case 4:  // digital silence
            v = 0;
            break;
         case 6:  // near mono: shared noise-modulated tone
            ph[0][1] += 2 * M_PI * 440.0 / fs;
            v = 0.4 * std::sin(ph[0][1]) * (0.6 + 0.4 * std::sin(2 * M_PI * 3 * t)) + 0.02 * rng.uniform();
            break;
         }
         x[2 * i + c] = float(v);
      }
   }
   return x;
}

struct Config {
   std::string name;
   int bitrate;
   int vbr;
   int cvbr;
   int complexity;
   int force_channels;   // OPUS_AUTO or 1/2
   int max_packet;       // output buffer size given to the encoder
   int loss_percent;     // simulated loss applied at the decoder
   int dtx;
   int packet_loss_hint; // OPUS_SET_PACKET_LOSS_PERC
};

struct Result {
   long long samples = 0;
   double err2 = 0, sig2 = 0, max_err = 0;
   int packets = 0, lost = 0, range_mismatch = 0, errors = 0;
};

Result run(const Config& cfg, const std::vector<float>& sig) {
   Result r;
   int err;
   OpusEncoder* enc = opus_encoder_create(fs, 2, OPUS_APPLICATION_RESTRICTED_CELT, &err);
   OpusDecoder* ref = opus_decoder_create(fs, 2, &err);
   auto dut = std::make_unique<opuspp::Decoder>();
   opus_encoder_ctl(enc, OPUS_SET_BITRATE(cfg.bitrate));
   opus_encoder_ctl(enc, OPUS_SET_VBR(cfg.vbr));
   opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(cfg.cvbr));
   opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(cfg.complexity));
   opus_encoder_ctl(enc, OPUS_SET_FORCE_CHANNELS(cfg.force_channels));
   opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
   // Supported streams: encoder complexity 0-4 with prediction disabled
   // (which rules out the pitch post-filter).
   opus_encoder_ctl(enc, OPUS_SET_PREDICTION_DISABLED(1));
   opus_encoder_ctl(enc, OPUS_SET_DTX(cfg.dtx));
   opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(cfg.packet_loss_hint));

   Rng loss{0x1234u + std::uint64_t(cfg.bitrate)};
   const int nframes = int(sig.size() / 2 / frame);
   unsigned char pkt[1500];
   float a[2 * frame];
   opuspp::AudioBuffer b;
   int burst = 0;
   for (int f = 0; f < nframes; f++) {
      const int len = opus_encode_float(enc, &sig[2 * frame * f], frame, pkt, cfg.max_packet);
      if (len < 0) {
         std::printf("encode error %d\n", len);
         std::exit(1);
      }
      bool lost = false;
      if (cfg.loss_percent > 0) {
         if (burst > 0) {
            lost = true;
            burst--;
         } else if (int(loss.next() % 100) < cfg.loss_percent) {
            lost = true;
            burst = int(loss.next() % 4);  // bursts of up to 4 lost packets
         }
      }
      int rn;
      opuspp::Error de;
      if (lost) {
         rn = opus_decode_float(ref, nullptr, 0, a, frame, 0);
         dut->decode_lost(b);
         de = opuspp::Error::ok;
         r.lost++;
      } else {
         rn = opus_decode_float(ref, pkt, len, a, frame, 0);
         de = dut->decode(pkt, std::size_t(len), b);
      }
      r.packets++;
      if (rn != frame || de != opuspp::Error::ok) {
         std::printf("  frame %d: ref=%d opuspp=%d\n", f, rn, int(de));
         r.errors++;
         continue;
      }
      opus_uint32 rng_ref;
      opus_decoder_ctl(ref, OPUS_GET_FINAL_RANGE(&rng_ref));
      if (rng_ref != dut->final_range()) r.range_mismatch++;
      for (int i = 0; i < 2 * frame; i++) {
         const double d = double(a[i]) - double(b.samples[i]);
         r.err2 += d * d;
         r.sig2 += double(a[i]) * double(a[i]);
         r.max_err = std::fmax(r.max_err, std::fabs(d));
      }
      r.samples += 2 * frame;
   }
   opus_encoder_destroy(enc);
   opus_decoder_destroy(ref);
   return r;
}

}  // namespace

int main(int argc, char** argv) {
   const int seconds = argc > 1 ? std::atoi(argv[1]) : 21;
   const auto sig = make_signal(seconds, 42);
   std::vector<Config> cfgs;
   const int rates[] = {6000, 12000, 16000, 24000, 32000, 48000, 64000, 96000, 128000, 192000, 256000, 510000};
   for (int br : rates) {
      cfgs.push_back({"vbr " + std::to_string(br), br, 1, 0, 4, OPUS_AUTO, 1275, 0, 0, 0});
      cfgs.push_back({"cbr " + std::to_string(br), br, 0, 0, 4, OPUS_AUTO, 1275, 0, 0, 0});
   }
   // Supported encoders run at complexity 0-4 (no post-filter): 0 has no
   // spreading or transients, 1-2 fixed spreading, 3-4 adaptive spreading.
   for (int cx = 0; cx <= 4; cx++)
      cfgs.push_back({"cvbr cx" + std::to_string(cx), 64000, 1, 1, cx, OPUS_AUTO, 1275, 0, 0, 0});
   cfgs.push_back({"vbr 320k cx1", 320000, 1, 0, 1, OPUS_AUTO, 1275, 0, 0, 0});
   cfgs.push_back({"mono 32k", 32000, 1, 0, 4, 1, 1275, 0, 0, 0});
   cfgs.push_back({"mono 96k", 96000, 1, 0, 4, 1, 1275, 0, 0, 0});
   cfgs.push_back({"stereo 24k", 24000, 1, 0, 4, 2, 1275, 0, 0, 0});
   // Encoder edge cases that emit packets without audio data.
   cfgs.push_back({"1 kb/s (no data)", 1000, 1, 0, 4, OPUS_AUTO, 1275, 0, 0, 0});
   cfgs.push_back({"2-byte buffer", 64000, 0, 0, 4, OPUS_AUTO, 2, 0, 0, 0});
   cfgs.push_back({"3-byte buffer", 64000, 0, 0, 4, OPUS_AUTO, 3, 0, 0, 0});
   cfgs.push_back({"vbr 32k cx2", 32000, 1, 0, 2, OPUS_AUTO, 1275, 0, 0, 0});
   cfgs.push_back({"dtx 24k", 24000, 1, 0, 4, OPUS_AUTO, 1275, 0, 1, 0});
   for (int lp : {5, 15, 30}) {
      cfgs.push_back({"loss" + std::to_string(lp) + " 64k", 64000, 1, 0, 4, OPUS_AUTO, 1275, lp, 0, lp});
      cfgs.push_back({"loss" + std::to_string(lp) + " 16k", 16000, 1, 0, 4, OPUS_AUTO, 1275, lp, 0, lp});
      cfgs.push_back({"loss" + std::to_string(lp) + " mono", 32000, 0, 0, 4, 1, 1275, lp, 0, lp});
   }

   int failures = 0;
   std::printf("%-18s %8s %6s %7s %10s %12s %s\n", "config", "packets", "lost", "rngbad", "SNR dB", "max |err|",
               "errors");
   for (const auto& cfg : cfgs) {
      const Result r = run(cfg, sig);
      const double snr = r.err2 > 0 ? 10 * std::log10(r.sig2 / r.err2) : INFINITY;
      std::printf("%-18s %8d %6d %7d %10.2f %12.3g %d\n", cfg.name.c_str(), r.packets, r.lost, r.range_mismatch, snr,
                  r.max_err, r.errors);
      // Final range must match exactly; audio must match to float precision.
      if (r.range_mismatch || r.errors || r.max_err > 1e-4) failures++;
   }
   std::printf("%s (%d failing configs)\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
