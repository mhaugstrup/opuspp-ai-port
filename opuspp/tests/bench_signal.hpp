// Encoder input shared by bench_encode.cpp and profile510.cpp: 48 kHz
// interleaved stereo cycling through 2 s segments, selected by a
// comma-separated list (default "tone,noise,transients,music"):
//   tone        harmonic tones with vibrato, different pitch per channel
//   noise       white noise
//   transients  short bursts over a quiet noise bed
//   music       consecutive 2 s slices of tests/data/figaro_overture.s16le,
//               wrapping around
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef OPUSPP_BENCH_AUDIO
#error "OPUSPP_BENCH_AUDIO must name the raw s16le stereo 48 kHz clip"
#endif

namespace bench {

enum class Segment { tone, noise, transients, music };

inline constexpr const char* default_segments = "tone,noise,transients,music";

// Parses a comma-separated segment list; exits with a message if invalid.
inline std::vector<Segment> parse_segments(const char* list) {
   static constexpr const char* names[] = {"tone", "noise", "transients", "music"};
   std::vector<Segment> out;
   const std::string str = list;
   std::size_t pos = 0;
   while (true) {
      const std::size_t end = std::min(str.find(',', pos), str.size());
      const std::string name = str.substr(pos, end - pos);
      int k = 0;
      while (k < 4 && name != names[k]) k++;
      if (k == 4) {
         std::fprintf(stderr, "unknown segment \"%s\" (expected a comma-separated list of "
                              "tone, noise, transients, music)\n", name.c_str());
         std::exit(2);
      }
      out.push_back(Segment(k));
      if (end == str.size()) return out;
      pos = end + 1;
   }
}

inline std::vector<float> load_music() {
   std::FILE* f = std::fopen(OPUSPP_BENCH_AUDIO, "rb");
   if (!f) {
      std::fprintf(stderr, "cannot open %s (run tools/fetch_deps.sh)\n", OPUSPP_BENCH_AUDIO);
      std::exit(1);
   }
   std::vector<float> x;
   unsigned char b[4];
   while (std::fread(b, 1, 4, f) == 4)
      for (int c = 0; c < 2; c++)
         x.push_back(float(std::int16_t(b[2 * c] | b[2 * c + 1] << 8)) * (1.0f / 32768));
   std::fclose(f);
   if (x.empty()) {
      std::fprintf(stderr, "%s is empty\n", OPUSPP_BENCH_AUDIO);
      std::exit(1);
   }
   return x;
}

inline std::vector<float> make_signal(int seconds, const std::vector<Segment>& segments) {
   bool has_music = false;
   for (Segment g : segments) has_music |= g == Segment::music;
   const std::vector<float> music = has_music ? load_music() : std::vector<float>{};
   const std::size_t music_frames = music.size() / 2;
   const int n = seconds * 48000;
   std::vector<float> x(2 * n);
   std::uint64_t s = 1;
   double ph[2][6] = {};
   std::size_t m = 0;  // position in the music clip
   for (int i = 0; i < n; i++) {
      const double t = i / 48000.0;
      const Segment seg = segments[std::size_t(t / 2) % segments.size()];
      if (seg == Segment::music) {
         x[2 * i] = music[2 * m];
         x[2 * i + 1] = music[2 * m + 1];
         m = (m + 1) % music_frames;
         continue;
      }
      for (int c = 0; c < 2; c++) {
         s = s * 6364136223846793005ULL + 1442695040888963407ULL;
         const double noise = double(std::int32_t(s >> 32)) / 2147483648.0;
         double v = 0;
         if (seg == Segment::tone) {  // harmonic tone with vibrato, different pitch per channel
            const double f0 = (c ? 180.0 : 220.0) * (1 + 0.03 * std::sin(2 * M_PI * 5 * t));
            for (int h = 0; h < 6; h++) {
               ph[c][h] += 2 * M_PI * f0 * (h + 1) / 48000;
               v += 0.25 / (h + 1) * std::sin(ph[c][h]);
            }
            v += 0.01 * noise;
         } else if (seg == Segment::noise) {
            v = 0.3 * noise;
         } else {  // transients over a quiet bed
            v = (i % 4800 < 30 ? 0.8 : 0.02) * noise;
         }
         x[2 * i + c] = float(v);
      }
   }
   return x;
}

} // namespace bench
