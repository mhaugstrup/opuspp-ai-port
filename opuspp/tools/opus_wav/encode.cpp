// Encodes the benchmark music clip with libopus (RESTRICTED_CELT,
// fullband, 10 ms, complexity 4, prediction disabled) into .opk packet files
// for opuspp_decode, and writes the original as a 32-bit float WAV.
//
// Usage: opuspp_encode <out_dir> <bitrate>...   (e.g. `. 510000 128000`)
// Writes <out_dir>/figaro_original.wav and <out_dir>/figaro_<kbps>k.opk.
#include <opus.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bench_signal.hpp"
#include "opk.hpp"

int main(int argc, char** argv) {
   if (argc < 3) {
      std::fprintf(stderr, "usage: %s <out_dir> <bitrate>...\n", argv[0]);
      return 2;
   }
   const std::string dir = argv[1];
   const std::vector<float> music = bench::load_music();
   opk::write_wav((dir + "/figaro_original.wav").c_str(), music.data(), music.size());
   for (int a = 2; a < argc; a++) {
      const int bitrate = std::atoi(argv[a]);
      int err;
      OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_CELT, &err);
      if (err != OPUS_OK) return 1;
      opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
      opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
      // opuspp supports streams from encoders at complexity 0-4 with prediction disabled.
      opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(4));
      opus_encoder_ctl(enc, OPUS_SET_PREDICTION_DISABLED(1));
      opus_int32 lookahead;
      opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
      opk::Stream s;
      s.lookahead = std::uint32_t(lookahead);
      s.frames = std::uint32_t(music.size() / 2);
      // Pad so the delayed output still covers the whole clip.
      std::vector<float> in = music;
      in.resize((in.size() / 2 + std::size_t(lookahead) + 479) / 480 * 960, 0.0f);
      unsigned char pkt[1500];
      for (std::size_t f = 0; f < in.size(); f += 960) {
         const int len = opus_encode_float(enc, &in[f], 480, pkt, sizeof(pkt));
         if (len < 0) return 1;
         s.packets.emplace_back(pkt, pkt + len);
      }
      opus_encoder_destroy(enc);
      const std::string path = dir + "/figaro_" + std::to_string(bitrate / 1000) + "k.opk";
      opk::save(path.c_str(), s);
      std::printf("wrote %s (%zu packets)\n", path.c_str(), s.packets.size());
   }
   return 0;
}
