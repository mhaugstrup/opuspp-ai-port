// PVQ codeword decoding and bit allocation (celt/cwrs.c, celt/rate.[ch]).
//
// Copyright (c) 2007-2008 CSIRO, 2007-2009 Xiph.Org Foundation,
// 2007-2009 Timothy B. Terriberry.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "celt_tables.hpp"
#include "config.hpp"
#include "entdec.hpp"
#include "math.hpp"
#include "simd.hpp"

namespace opuspp::detail {

inline constexpr const std::int16_t* ebands = tables::eband5ms;

// Band widths ebands[j+1] - ebands[j] as an aligned int table (padded to
// band_stride), so vectorised allocation loops read it with aligned loads.
struct BandWidths {
   alignas(buffer_alignment) int v[band_stride];
};
// band_allocation widened to int rows padded to band_stride (aligned rows).
struct AllocTable {
   alignas(buffer_alignment) int v[11][band_stride];
};
inline constexpr AllocTable alloc_table = [] {
   AllocTable t{};
   for (int r = 0; r < 11; r++)
      for (int j = 0; j < nb_ebands; j++) t.v[r][j] = tables::band_allocation[r * nb_ebands + j];
   return t;
}();

inline constexpr BandWidths band_widths = [] {
   BandWidths t{};
   for (int j = 0; j < nb_ebands; j++) t.v[j] = tables::eband5ms[j + 1] - tables::eband5ms[j];
   return t;
}();

// ---------------------------------------------------------------------------
// cwrs.c
// ---------------------------------------------------------------------------

inline constexpr int pvq_u_row_offset[15] = {0,    176,  351,  525,  698,  870,  1041, 1131,
                                             1178, 1207, 1226, 1240, 1248, 1254, 1257};

OPUSPP_INLINE const std::uint32_t* pvq_u_row(int n) noexcept { return tables::pvq_u_data + pvq_u_row_offset[n]; }
OPUSPP_INLINE std::uint32_t pvq_u(int n, int k) noexcept { return pvq_u_row(min(n, k))[max(n, k)]; }
OPUSPP_INLINE std::uint32_t pvq_v(int n, int k) noexcept { return pvq_u(n, k) + pvq_u(n, k + 1); }

// Largest k' in [lo, hi] with row[k'] <= i, for a non-decreasing row with
// row[lo] <= i. Same result as the libopus scan down from hi
// (for (p = row[k]; p > i; p = row[k]) k--), but the scan's data-dependent
// exit was the main source of branch mispredictions at high bit rates, and a
// binary search would replace them with a chain of dependent loads. Because
// the row is monotonic, the number of scan steps equals the number of
// entries > i in the window ending at hi: eight independent compares count
// up to eight steps at once, and only longer scans loop.
OPUSPP_INLINE int last_le(const std::uint32_t* row, int lo, int hi, std::uint32_t i) noexcept {
   for (;;) {
      int steps = 0;
      // Eight scalar compares. Fully unrolled where that stays scalar (as a
      // loop, its exit costs a little); Clang with AVX2 or AVX-512 would turn
      // the unrolled compares into a gather, which is slower, so there it
      // stays a rolled, non-vectorised loop (whose fixed exit predicts well).
#if defined(__clang__) && defined(__AVX2__)
      OPUSPP_NO_VECTORIZE
#else
#pragma GCC unroll 8
#endif
      for (int j = 0; j < 8; j++) steps += int(row[max(hi - j, lo)] > i);
      hi -= steps;
      if (steps < 8) return hi;
   }
}

// Returns the index'th combination of k pulses in n dimensions; returns the
// squared norm of the vector.
inline float cwrsi(int n, int k, std::uint32_t i, int* y) noexcept {
   OPUSPP_ASSERT(k > 0 && n > 1);
   std::uint32_t p;
   int s;
   int k0;
   std::int16_t val;
   float yy = 0;
   while (n > 2) {
      std::uint32_t q;
      if (k >= n) {
         // Lots of pulses case.
         const std::uint32_t* row = pvq_u_row(n);
         p = row[k + 1];
         s = -(i >= p);
         i -= p & std::uint32_t(s);
         k0 = k;
         q = row[n];
         if (q > i) {
            k = n;
            do p = pvq_u_row(--k)[n];
            while (p > i);
         } else {
            // row[n] = q <= i: the scan for (p = row[k]; p > i; p = row[k]) k--
            // ends at the largest k' in [n, k] with row[k'] <= i.
            k = last_le(row, n, k, i);
            p = row[k];
         }
         i -= p;
         val = std::int16_t((k0 - k + s) ^ s);
         *y++ = val;
         yy = yy + float(val) * float(val);
      } else {
         // Lots of dimensions case.
         p = pvq_u_row(k)[n];
         q = pvq_u_row(k + 1)[n];
         if (p <= i && i < q) {
            i -= p;
            *y++ = 0;
         } else {
            s = -(i >= q);
            i -= q & std::uint32_t(s);
            k0 = k;
            // Short scan (usually one step): kept, as it predicts well.
            do p = pvq_u_row(--k)[n];
            while (p > i);
            i -= p;
            val = std::int16_t((k0 - k + s) ^ s);
            *y++ = val;
            yy = yy + float(val) * float(val);
         }
      }
      n--;
   }
   // n == 2
   p = 2 * std::uint32_t(k) + 1;
   s = -(i >= p);
   i -= p & std::uint32_t(s);
   k0 = k;
   k = int((i + 1) >> 1);
   if (k) i -= 2 * std::uint32_t(k) - 1;
   val = std::int16_t((k0 - k + s) ^ s);
   *y++ = val;
   yy = yy + float(val) * float(val);
   // n == 1
   s = -int(i);
   val = std::int16_t((k + s) ^ s);
   *y = val;
   yy = yy + float(val) * float(val);
   return yy;
}

inline float decode_pulses(int* y, int n, int k, RangeDecoder& dec) noexcept {
   return cwrsi(n, k, dec.uint(pvq_v(n, k)), y);
}

// ---------------------------------------------------------------------------
// rate.h
// ---------------------------------------------------------------------------

OPUSPP_INLINE int get_pulses(int i) noexcept { return i < 8 ? i : (8 + (i & 7)) << ((i >> 3) - 1); }

OPUSPP_INLINE const std::uint8_t* pulse_cache(int band, int lm_plus_1) noexcept {
   return tables::cache_bits50 + tables::cache_index50[lm_plus_1 * nb_ebands + band];
}

// bits2pulses() as in rate.h: binary search for the pulse count whose cost
// in the pulse cache row is closest to `bits`.
constexpr int bits2pulses_search(const std::uint8_t* cache, int bits) noexcept {
   int lo = 0;
   int hi = cache[0];
   bits--;
   for (int i = 0; i < 6; i++) {
      const int mid = (lo + hi + 1) >> 1;
      if (int(cache[mid]) >= bits)
         hi = mid;
      else
         lo = mid;
   }
   if (bits - (lo == 0 ? -1 : int(cache[lo])) <= int(cache[hi]) - bits) return lo;
   return hi;
}

// The search above is a chain of 6 dependent loads per call. Its result for
// every cache row and every bit count is tabulated at compile time with the
// same function, so the table is exact by construction. Cache entries are
// bytes, so for bits >= 257 the result no longer changes.
struct Bits2PulsesLut {
   static constexpr int max_bits = 257;
   static constexpr int nb_rows = 23;  // distinct rows referenced by cache_index50
   std::int8_t row[5 * nb_ebands];     // -1: no pulses possible
   alignas(buffer_alignment) std::uint8_t v[nb_rows][max_bits + 1];
};

// Not constexpr: calling it during constant evaluation is a compile error.
inline void table_generation_failed() noexcept {}

inline constexpr Bits2PulsesLut bits2pulses_lut = [] {
   Bits2PulsesLut t{};
   int offsets[Bits2PulsesLut::nb_rows] = {};
   int rows = 0;
   for (int k = 0; k < 5 * nb_ebands; k++) {
      const int off = tables::cache_index50[k];
      t.row[k] = -1;
      if (off < 0) continue;
      int r = 0;
      while (r < rows && offsets[r] != off) r++;
      if (r == rows) {
         if (rows == Bits2PulsesLut::nb_rows) table_generation_failed();  // more rows than expected
         offsets[rows++] = off;
         for (int b = 0; b <= Bits2PulsesLut::max_bits; b++)
            t.v[r][b] = static_cast<std::uint8_t>(bits2pulses_search(tables::cache_bits50 + off, b));
      }
      t.row[k] = static_cast<std::int8_t>(r);
   }
   if (rows != Bits2PulsesLut::nb_rows) table_generation_failed();  // fewer rows than expected
   return t;
}();

inline int bits2pulses(int band, int LM, int bits) noexcept {
   const int r = bits2pulses_lut.row[(LM + 1) * nb_ebands + band];
   OPUSPP_ASSERT(r >= 0);
   if (bits < 0) [[unlikely]]
      return bits2pulses_search(pulse_cache(band, LM + 1), bits);
   return bits2pulses_lut.v[r][min(bits, Bits2PulsesLut::max_bits)];
}

inline int pulses2bits(int band, int LM, int pulses) noexcept {
   const std::uint8_t* cache = pulse_cache(band, LM + 1);
   return pulses == 0 ? 0 : cache[pulses] + 1;
}

// ---------------------------------------------------------------------------
// rate.c
// ---------------------------------------------------------------------------

// Caps on the number of bits per band (init_caps) for LM = 2 and C = 1, 2,
// computed at compile time and padded to band_stride.
struct CapTable {
   alignas(buffer_alignment) int v[2][band_stride];
};

inline constexpr CapTable cap_table = [] {
   CapTable t{};
   for (int C = 1; C <= 2; C++)
      for (int i = 0; i < nb_ebands; i++) {
         const int N = (ebands[i + 1] - ebands[i]) << lm;
         t.v[C - 1][i] = (tables::cache_caps50[nb_ebands * (2 * lm + C - 1) + i] + 64) * C * N >> 2;
      }
   return t;
}();

inline void init_caps(int* cap, int C) noexcept {
   const int* src = cap_table.v[C - 1];
   for (int i = 0; i < band_stride; i += 4) i32x4::load_aligned(src + i).store_aligned(cap + i);
}

struct Allocation {
   int coded_bands;
   int intensity;
   int dual_stereo;
   std::int32_t balance;
};

inline int interp_bits2pulses(int start, int end, int skip_start, const int* bits1, const int* bits2,
                              const int* thresh, const int* cap, std::int32_t total, std::int32_t* balance_out,
                              int skip_rsv, int* intensity, int intensity_rsv, int* dual_stereo,
                              int dual_stereo_rsv, int* bits, int* ebits, int* fine_priority, int C,
                              RangeDecoder& dec) noexcept {
   // All band arrays are 16-byte aligned; the decoder always starts at band 0.
   [[assume(start == 0)]];
   bits1 = assume_aligned<buffer_alignment>(bits1);
   bits2 = assume_aligned<buffer_alignment>(bits2);
   thresh = assume_aligned<buffer_alignment>(thresh);
   cap = assume_aligned<buffer_alignment>(cap);
   bits = assume_aligned<buffer_alignment>(bits);
   ebits = assume_aligned<buffer_alignment>(ebits);
   fine_priority = assume_aligned<buffer_alignment>(fine_priority);
   constexpr int alloc_steps = 6;
   const int alloc_floor = C << bitres;
   const int stereo = C > 1;
   const int logM = lm << bitres;
   int lo = 0;
   int hi = 1 << alloc_steps;
   std::int32_t psum;
   for (int i = 0; i < alloc_steps; i++) {
      const int mid = (lo + hi) >> 1;
      psum = 0;
      int done = 0;
      for (int j = end; j-- > start;) {
         const int tmp = bits1[j] + (mid * std::int32_t(bits2[j]) >> alloc_steps);
         if (tmp >= thresh[j] || done) {
            done = 1;
            psum += min(tmp, cap[j]);
         } else if (tmp >= alloc_floor) {
            psum += alloc_floor;
         }
      }
      if (psum > total)
         hi = mid;
      else
         lo = mid;
   }
   psum = 0;
   int done = 0;
   for (int j = end; j-- > start;) {
      int tmp = bits1[j] + (std::int32_t(lo) * bits2[j] >> alloc_steps);
      if (tmp < thresh[j] && !done) {
         tmp = tmp >= alloc_floor ? alloc_floor : 0;
      } else {
         done = 1;
      }
      tmp = min(tmp, cap[j]);
      bits[j] = tmp;
      psum += tmp;
   }

   // Decide which bands to skip, working backwards from the end.
   int coded_bands;
   std::int32_t left, percoeff;
   for (coded_bands = end;; coded_bands--) {
      const int j = coded_bands - 1;
      if (j <= skip_start) {
         // Give the bit we reserved to end skipping back.
         total += skip_rsv;
         break;
      }
      left = total - psum;
      percoeff = std::int32_t(celt_udiv(std::uint32_t(left), std::uint32_t(ebands[coded_bands] - ebands[start])));
      left -= (ebands[coded_bands] - ebands[start]) * percoeff;
      const int rem = max(int(left) - (ebands[j] - ebands[start]), 0);
      const int band_width = ebands[coded_bands] - ebands[j];
      int band_bits = int(bits[j] + percoeff * band_width + rem);
      // Only code a skip decision if we're above the threshold for this band.
      if (band_bits >= max(thresh[j], alloc_floor + (1 << bitres))) {
         if (dec.bit_logp(1)) break;
         psum += 1 << bitres;
         band_bits -= 1 << bitres;
      }
      // Reclaim the bits originally allocated to this band.
      psum -= bits[j] + intensity_rsv;
      if (intensity_rsv > 0) intensity_rsv = tables::log2_frac_table[j - start];
      psum += intensity_rsv;
      if (band_bits >= alloc_floor) {
         psum += alloc_floor;
         bits[j] = alloc_floor;
      } else {
         bits[j] = 0;
      }
   }

   OPUSPP_ASSERT(coded_bands > start);
   // Code the intensity and dual stereo parameters.
   if (intensity_rsv > 0)
      *intensity = start + int(dec.uint(std::uint32_t(coded_bands + 1 - start)));
   else
      *intensity = 0;
   if (*intensity <= start) {
      total += dual_stereo_rsv;
      dual_stereo_rsv = 0;
   }
   if (dual_stereo_rsv > 0)
      *dual_stereo = dec.bit_logp(1);
   else
      *dual_stereo = 0;

   // Allocate the remaining bits.
   left = total - psum;
   percoeff = std::int32_t(celt_udiv(std::uint32_t(left), std::uint32_t(ebands[coded_bands] - ebands[start])));
   left -= (ebands[coded_bands] - ebands[start]) * percoeff;
   for (int j = start; j < coded_bands; j++) bits[j] += int(percoeff) * band_widths.v[j];
   for (int j = start; j < coded_bands; j++) {
      const int tmp = int(min(left, std::int32_t(band_widths.v[j])));
      bits[j] += tmp;
      left -= tmp;
   }

   std::int32_t balance = 0;
   int j;
   for (j = start; j < coded_bands; j++) {
      std::int32_t excess;
      const int N0 = ebands[j + 1] - ebands[j];
      const int N = N0 << lm;
      const std::int32_t bit = std::int32_t(bits[j]) + balance;
      if (N > 1) {
         excess = max(bit - cap[j], std::int32_t(0));
         bits[j] = int(bit - excess);
         // Compensate for the extra DoF in stereo.
         const int den = C * N + ((C == 2 && N > 2 && !*dual_stereo && j < *intensity) ? 1 : 0);
         const int NClogN = den * (tables::logN400[j] + logM);
         // Offset for the number of fine bits by log2(N)/2 + FINE_OFFSET.
         int offset = (NClogN >> 1) - den * fine_offset;
         // N=2 is the only point that doesn't match the curve.
         if (N == 2) offset += den << bitres >> 2;
         // Changing the offset for allocating the second and third fine energy bit.
         if (bits[j] + offset < den * 2 << bitres)
            offset += NClogN >> 2;
         else if (bits[j] + offset < den * 3 << bitres)
            offset += NClogN >> 3;
         // Divide with rounding.
         ebits[j] = max(0, bits[j] + offset + (den << (bitres - 1)));
         ebits[j] = int(celt_udiv(std::uint32_t(ebits[j]), std::uint32_t(den)) >> bitres);
         // Make sure not to bust.
         if (C * ebits[j] > (bits[j] >> bitres)) ebits[j] = bits[j] >> stereo >> bitres;
         // More than that is useless because that's about as far as PVQ can go.
         ebits[j] = min(ebits[j], max_fine_bits);
         // If we rounded down or capped this band, make it a candidate for the
         // final fine energy pass.
         fine_priority[j] = ebits[j] * (den << bitres) >= bits[j] + offset;
         // Remove the allocated fine bits; the rest are assigned to PVQ.
         bits[j] -= C * ebits[j] << bitres;
      } else {
         // For N=1, all bits go to fine energy except for a single sign bit.
         excess = max(std::int32_t(0), bit - (C << bitres));
         bits[j] = int(bit - excess);
         ebits[j] = 0;
         fine_priority[j] = 1;
      }
      // Fine energy can't take advantage of the re-balancing in
      // quant_all_bands(), so do the re-balancing here.
      if (excess > 0) {
         const int extra_fine = min(int(excess >> (stereo + bitres)), max_fine_bits - ebits[j]);
         ebits[j] += extra_fine;
         const int extra_bits = extra_fine * C << bitres;
         fine_priority[j] = extra_bits >= excess - balance;
         excess -= extra_bits;
      }
      balance = excess;
   }
   // Save any remaining bits over the cap for the rebalancing in quant_all_bands().
   *balance_out = balance;

   // The skipped bands use all their bits for fine energy (starts at the
   // runtime index coded_bands, so it stays scalar).
   OPUSPP_NO_VECTORIZE
   for (; j < end; j++) {
      ebits[j] = bits[j] >> stereo >> bitres;
      bits[j] = 0;
      fine_priority[j] = ebits[j] < 1;
   }
   return coded_bands;
}

inline Allocation compute_allocation(int start, int end, const int* offsets, const int* cap, int alloc_trim,
                                     std::int32_t total, int* pulses, int* ebits, int* fine_priority, int C,
                                     RangeDecoder& dec) noexcept {
   constexpr int nb_alloc_vectors = 11;
   [[assume(start == 0)]];
   offsets = assume_aligned<buffer_alignment>(offsets);
   cap = assume_aligned<buffer_alignment>(cap);
   Allocation a;
   alignas(buffer_alignment) int bits1[band_stride];
   alignas(buffer_alignment) int bits2[band_stride];
   alignas(buffer_alignment) int thresh[band_stride];
   alignas(buffer_alignment) int trim_offset[band_stride];

   total = max(total, std::int32_t(0));
   int skip_start = start;
   // Reserve a bit to signal the end of manually skipped bands.
   const int skip_rsv = total >= 1 << bitres ? 1 << bitres : 0;
   total -= skip_rsv;
   // Reserve bits for the intensity and dual stereo parameters.
   int intensity_rsv = 0;
   int dual_stereo_rsv = 0;
   if (C == 2) {
      intensity_rsv = tables::log2_frac_table[end - start];
      if (intensity_rsv > total) {
         intensity_rsv = 0;
      } else {
         total -= intensity_rsv;
         dual_stereo_rsv = total >= 1 << bitres ? 1 << bitres : 0;
         total -= dual_stereo_rsv;
      }
   }
   for (int j = start; j < end; j++) {
      // Below this threshold, we're sure not to allocate any PVQ bits.
      thresh[j] = max(C << bitres, (3 * band_widths.v[j] << lm << bitres) >> 4);
      // Tilt of the allocation curve.
      trim_offset[j] =
         C * band_widths.v[j] * (alloc_trim - 5 - lm) * (end - j - 1) * (1 << (lm + bitres)) >> 6;
      // Giving less resolution to single-coefficient bands.
      if (band_widths.v[j] << lm == 1) trim_offset[j] -= C << bitres;
   }
   int lo = 1;
   int hi = nb_alloc_vectors - 1;
   do {
      int done = 0;
      int psum = 0;
      const int mid = (lo + hi) >> 1;
      for (int j = end; j-- > start;) {
         const int N = band_widths.v[j];
         int bitsj = C * N * alloc_table.v[mid][j] << lm >> 2;
         if (bitsj > 0) bitsj = max(0, bitsj + trim_offset[j]);
         bitsj += offsets[j];
         if (bitsj >= thresh[j] || done) {
            done = 1;
            psum += min(bitsj, cap[j]);
         } else if (bitsj >= C << bitres) {
            psum += C << bitres;
         }
      }
      if (psum > total)
         hi = mid - 1;
      else
         lo = mid + 1;
   } while (lo <= hi);
   hi = lo--;
   for (int j = start; j < end; j++) {
      const int N = band_widths.v[j];
      int bits1j = C * N * alloc_table.v[lo][j] << lm >> 2;
      int bits2j = hi >= nb_alloc_vectors ? cap[j] : C * N * alloc_table.v[hi][j] << lm >> 2;
      if (bits1j > 0) bits1j = max(0, bits1j + trim_offset[j]);
      if (bits2j > 0) bits2j = max(0, bits2j + trim_offset[j]);
      if (lo > 0) bits1j += offsets[j];
      bits2j += offsets[j];
      if (offsets[j] > 0) skip_start = j;
      bits2j = max(0, bits2j - bits1j);
      bits1[j] = bits1j;
      bits2[j] = bits2j;
   }
   a.coded_bands = interp_bits2pulses(start, end, skip_start, bits1, bits2, thresh, cap, total, &a.balance,
                                      skip_rsv, &a.intensity, intensity_rsv, &a.dual_stereo, dual_stereo_rsv,
                                      pulses, ebits, fine_priority, C, dec);
   return a;
}

} // namespace opuspp::detail
