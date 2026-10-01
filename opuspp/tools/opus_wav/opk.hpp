// Shared by opuspp_encode and opuspp_decode: a minimal packet file format
// (.opk) and a 32-bit float stereo WAV writer.
//
// .opk layout (little-endian): "OPK1", u32 encoder lookahead (samples per
// channel), u32 original length (samples per channel), then per packet a u16
// length followed by the packet bytes. A zero length is a lost packet.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace opk {

struct Stream {
   std::uint32_t lookahead = 0;
   std::uint32_t frames = 0;  // original length, samples per channel
   std::vector<std::vector<std::uint8_t>> packets;
};

[[noreturn]] inline void die(const char* what, const char* path) {
   std::fprintf(stderr, "%s: %s\n", what, path);
   std::exit(1);
}

inline void write_u32(std::FILE* f, std::uint32_t v) {
   const std::uint8_t b[4] = {std::uint8_t(v), std::uint8_t(v >> 8), std::uint8_t(v >> 16), std::uint8_t(v >> 24)};
   std::fwrite(b, 1, 4, f);
}

inline void write_u16(std::FILE* f, std::uint16_t v) {
   const std::uint8_t b[2] = {std::uint8_t(v), std::uint8_t(v >> 8)};
   std::fwrite(b, 1, 2, f);
}

inline void save(const char* path, const Stream& s) {
   std::FILE* f = std::fopen(path, "wb");
   if (!f) die("cannot create", path);
   std::fwrite("OPK1", 1, 4, f);
   write_u32(f, s.lookahead);
   write_u32(f, s.frames);
   for (const auto& p : s.packets) {
      write_u16(f, std::uint16_t(p.size()));
      std::fwrite(p.data(), 1, p.size(), f);
   }
   std::fclose(f);
}

inline Stream load(const char* path) {
   std::FILE* f = std::fopen(path, "rb");
   if (!f) die("cannot open", path);
   auto read_u = [&](int n) {
      std::uint8_t b[4] = {};
      if (std::fread(b, 1, std::size_t(n), f) != std::size_t(n)) return std::uint32_t(0xffffffff);
      return std::uint32_t(b[0] | b[1] << 8 | b[2] << 16 | std::uint32_t(b[3]) << 24);
   };
   char magic[4];
   if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "OPK1", 4) != 0) die("not an .opk file", path);
   Stream s;
   s.lookahead = read_u(4);
   s.frames = read_u(4);
   for (std::uint32_t len; (len = read_u(2)) != 0xffffffff;) {
      std::vector<std::uint8_t> p(len);
      if (std::fread(p.data(), 1, len, f) != len) die("truncated packet in", path);
      s.packets.push_back(std::move(p));
   }
   std::fclose(f);
   return s;
}

inline void write_wav(const char* path, const float* x, std::size_t n /* floats, interleaved */) {
   std::FILE* f = std::fopen(path, "wb");
   if (!f) die("cannot create", path);
   const std::uint32_t bytes = std::uint32_t(n * 4);
   std::fwrite("RIFF", 1, 4, f);
   write_u32(f, 36 + bytes);
   std::fwrite("WAVEfmt ", 1, 8, f);
   write_u32(f, 16);
   write_u16(f, 3);  // IEEE float
   write_u16(f, 2);
   write_u32(f, 48000);
   write_u32(f, 48000 * 8);
   write_u16(f, 8);
   write_u16(f, 32);
   std::fwrite("data", 1, 4, f);
   write_u32(f, bytes);
   for (std::size_t i = 0; i < n; i++) {
      std::uint32_t v;
      std::memcpy(&v, &x[i], 4);
      write_u32(f, v);
   }
   std::fclose(f);
   std::printf("wrote %s\n", path);
}

} // namespace opk
