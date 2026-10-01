// Range decoder (celt/entdec.c, celt/entcode.c, celt/laplace.c).
//
// Copyright (c) 2001-2011 Timothy B. Terriberry, 2008-2009 Xiph.Org Foundation.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstdint>

#include "config.hpp"
#include "math.hpp"

namespace opuspp::detail {

class alignas(buffer_alignment) RangeDecoder {
public:
   static constexpr int sym_bits = 8;
   static constexpr int code_bits = 32;
   static constexpr std::uint32_t sym_max = (1U << sym_bits) - 1;
   static constexpr std::uint32_t code_top = 1U << (code_bits - 1);
   static constexpr std::uint32_t code_bot = code_top >> sym_bits;
   static constexpr int code_extra = (code_bits - 2) % sym_bits + 1;
   static constexpr int window_size = 32;
   static constexpr int uint_bits = 8;

   RangeDecoder(const std::uint8_t* buf, std::uint32_t storage) noexcept
      : buf_(buf), storage_(storage) {
      nbits_total_ = code_bits + 1 - ((code_bits - code_extra) / sym_bits) * sym_bits;
      rng_ = 1U << code_extra;
      rem_ = read_byte();
      val_ = rng_ - 1 - (rem_ >> (sym_bits - code_extra));
      normalize();
   }

   std::uint32_t storage() const noexcept { return storage_; }
   std::uint32_t range() const noexcept { return rng_; }
   bool error() const noexcept { return error_; }

   // Number of whole bits consumed so far (rounded up).
   int tell() const noexcept { return nbits_total_ - ec_ilog(rng_); }

   // Number of bits consumed, in 1/8 bit units.
   std::uint32_t tell_frac() const noexcept {
      static constexpr unsigned correction[8] = {35733, 38967, 42495, 46340,
                                                 50535, 55109, 60097, 65535};
      std::uint32_t nbits = std::uint32_t(nbits_total_) << bitres;
      int l = ec_ilog(rng_);
      std::uint32_t r = rng_ >> (l - 16);
      unsigned b = (r >> 12) - 8;
      b += r > correction[b];
      l = (l << 3) + int(b);
      return nbits - std::uint32_t(l);
   }

   // Pretends all remaining bits have been read (CELT silence frames).
   void skip_to_end(int total_bits) noexcept { nbits_total_ += total_bits - tell(); }

   unsigned decode(unsigned ft) noexcept {
      ext_ = celt_udiv(rng_, ft);
      unsigned s = unsigned(val_ / ext_);
      return ft - min(s + 1, ft);
   }

   unsigned decode_bin(unsigned bits) noexcept {
      ext_ = rng_ >> bits;
      unsigned s = unsigned(val_ / ext_);
      return (1U << bits) - min(s + 1U, 1U << bits);
   }

   void update(unsigned fl, unsigned fh, unsigned ft) noexcept {
      std::uint32_t s = ext_ * (ft - fh);
      val_ -= s;
      rng_ = fl > 0 ? ext_ * (fh - fl) : rng_ - s;
      normalize();
   }

   // Decodes a bit whose probability of being one is 1/(1<<logp).
   int bit_logp(unsigned logp) noexcept {
      std::uint32_t r = rng_;
      std::uint32_t d = val_;
      std::uint32_t s = r >> logp;
      int ret = d < s;
      if (!ret) val_ = d - s;
      rng_ = ret ? s : r - s;
      normalize();
      return ret;
   }

   template <class T>
   int icdf(const T* icdf, unsigned ftb) noexcept {
      std::uint32_t s = rng_;
      std::uint32_t d = val_;
      std::uint32_t r = s >> ftb;
      std::uint32_t t;
      int ret = -1;
      do {
         t = s;
         s = r * icdf[++ret];
      } while (d < s);
      val_ = d - s;
      rng_ = t - s;
      normalize();
      return ret;
   }

   std::uint32_t uint(std::uint32_t ft) noexcept {
      OPUSPP_ASSERT(ft > 1);
      ft--;
      int ftb = ec_ilog(ft);
      if (ftb > uint_bits) {
         ftb -= uint_bits;
         unsigned ft1 = unsigned(ft >> ftb) + 1;
         unsigned s = decode(ft1);
         update(s, s + 1, ft1);
         std::uint32_t t = std::uint32_t(s) << ftb | bits(unsigned(ftb));
         if (t <= ft) return t;
         error_ = true;
         return ft;
      }
      ft++;
      unsigned s = decode(unsigned(ft));
      update(s, s + 1, unsigned(ft));
      return s;
   }

   // Reads raw bits from the end of the buffer.
   std::uint32_t bits(unsigned n) noexcept {
      std::uint32_t window = end_window_;
      int available = nend_bits_;
      if (unsigned(available) < n) {
         do {
            window |= std::uint32_t(read_byte_from_end()) << available;
            available += sym_bits;
         } while (available <= window_size - sym_bits);
      }
      std::uint32_t ret = window & ((std::uint32_t(1) << n) - 1U);
      window >>= n;
      available -= int(n);
      end_window_ = window;
      nend_bits_ = available;
      nbits_total_ += int(n);
      return ret;
   }

   // Laplace-distributed value (celt/laplace.c: ec_laplace_decode).
   int laplace(unsigned fs, int decay) noexcept {
      constexpr unsigned minp = 1;
      constexpr int nmin = 16;
      int val = 0;
      unsigned fm = decode_bin(15);
      unsigned fl = 0;
      if (fm >= fs) {
         val++;
         fl = fs;
         // ec_laplace_get_freq1()
         unsigned ft = 32768 - minp * (2 * nmin) - fs;
         fs = ((ft * std::int32_t(16384 - decay)) >> 15) + minp;
         while (fs > minp && fm >= fl + 2 * fs) {
            fs *= 2;
            fl += fs;
            fs = ((fs - 2 * minp) * std::int32_t(decay)) >> 15;
            fs += minp;
            val++;
         }
         if (fs <= minp) {
            int di = int((fm - fl) >> 1);
            val += di;
            fl += 2 * unsigned(di) * minp;
         }
         if (fm < fl + fs)
            val = -val;
         else
            fl += fs;
      }
      update(fl, min(fl + fs, 32768U), 32768);
      return val;
   }

private:
   int read_byte() noexcept { return offs_ < storage_ ? buf_[offs_++] : 0; }

   int read_byte_from_end() noexcept {
      return end_offs_ < storage_ ? buf_[storage_ - ++end_offs_] : 0;
   }

   void normalize() noexcept {
      while (rng_ <= code_bot) {
         nbits_total_ += sym_bits;
         rng_ <<= sym_bits;
         int sym = rem_;
         rem_ = read_byte();
         sym = (sym << sym_bits | rem_) >> (sym_bits - code_extra);
         val_ = ((val_ << sym_bits) + (sym_max & ~std::uint32_t(sym))) & (code_top - 1);
      }
   }

   // Zero-initialised words first, in whole 16-byte groups.
   std::uint32_t end_offs_ = 0;
   std::uint32_t end_window_ = 0;
   int nend_bits_ = 0;
   int nbits_total_ = 0;
   std::uint32_t offs_ = 0;
   std::uint32_t rng_ = 0;
   std::uint32_t val_ = 0;
   std::uint32_t ext_ = 0;
   const std::uint8_t* buf_;
   std::uint32_t storage_;
   int rem_ = 0;
   bool error_ = false;
};

} // namespace opuspp::detail
