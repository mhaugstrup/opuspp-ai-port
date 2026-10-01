// Packet storage for the benchmark and profiling programs: all packets of a
// stream live in one arena of 64-byte blocks, each packet starting on a block
// boundary, so packet alignment is the same for every decoder and every run.
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "opk.hpp"

namespace bench {

struct alignas(64) Block {
   unsigned char bytes[64];
};

struct Packet {
   std::size_t block = 0;  // first block in Stream::arena
   std::size_t size = 0;   // 0 = lost
};

struct Stream {
   std::vector<Block> arena;
   std::vector<Packet> packets;

   const unsigned char* data(const Packet& p) const { return arena[p.block].bytes; }

   void add(const unsigned char* bytes, std::size_t size) {
      Packet p{arena.size(), size};
      arena.resize(arena.size() + (size + sizeof(Block) - 1) / sizeof(Block));
      std::copy(bytes, bytes + size, arena[p.block].bytes);
      packets.push_back(p);
   }
   void add_lost() { packets.push_back({}); }
};

// Loads an .opk file (zero-length packets are losses).
inline Stream load(const char* path) {
   const opk::Stream s = opk::load(path);
   Stream out;
   for (const auto& p : s.packets) {
      if (p.empty()) out.add_lost();
      else out.add(p.data(), p.size());
   }
   return out;
}

} // namespace bench
