// Band energy dequantisation, PVQ decoding and band reconstruction
// (celt/quant_bands.c, celt/vq.c, celt/bands.c), decoder side, float build.
//
// Copyright (c) 2007-2008 CSIRO, 2007-2009 Xiph.Org Foundation,
// 2008-2009 Gregory Maxwell.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "celt_tables.hpp"
#include "config.hpp"
#include "entdec.hpp"
#include "math.hpp"
#include "pitch.hpp"
#include "rate.hpp"

namespace opuspp::detail {

inline constexpr int max_band_size = (100 - 78) << lm;  // widest band at LM=2

OPUSPP_INLINE std::uint32_t celt_lcg_rand(std::uint32_t seed) noexcept { return 1664525 * seed + 1013904223; }

// ---------------------------------------------------------------------------
// quant_bands.c
// ---------------------------------------------------------------------------

inline void unquant_coarse_energy(int start, int end, float* old_ebands, int intra, RangeDecoder& dec,
                                  int C) noexcept {
   static constexpr float pred_coef = float(21248 / 32768.);  // LM = 2
   static constexpr float beta_coef = float(12124 / 32768.);
   static constexpr float beta_intra = float(4915 / 32768.);
   static constexpr unsigned char small_energy_icdf[3] = {2, 1, 0};
   const std::uint8_t* prob_model = tables::e_prob_model[lm][intra];
   float prev[2];
   prev[0] = prev[1] = 0;
   const float coef = intra ? 0.f : pred_coef;
   const float beta = intra ? beta_intra : beta_coef;
   const std::int32_t budget = std::int32_t(dec.storage() * 8);

   for (int i = start; i < end; i++) {
      int c = 0;
      do {
         int qi;
         const std::int32_t tell = dec.tell();
         if (budget - tell >= 15) {
            const int pi = 2 * min(i, 20);
            qi = dec.laplace(unsigned(prob_model[pi]) << 7, prob_model[pi + 1] << 6);
         } else if (budget - tell >= 2) {
            qi = dec.icdf(small_energy_icdf, 2);
            qi = (qi >> 1) ^ -(qi & 1);
         } else if (budget - tell >= 1) {
            qi = -dec.bit_logp(1);
         } else {
            qi = -1;
         }
         const float q = float(qi);
         float& e = old_ebands[i + c * band_stride];
         e = max(-9.f, e);
         e = coef * e + prev[c] + q;
         prev[c] = prev[c] + q - beta * q;
      } while (++c < C);
   }
}

inline void unquant_fine_energy(int start, int end, float* old_ebands, const int* fine_quant, RangeDecoder& dec,
                                int C) noexcept {
   for (int i = start; i < end; i++) {
      const int extra = fine_quant[i];
      if (extra <= 0) continue;
      if (dec.tell() + C * extra > std::int32_t(dec.storage() * 8)) continue;
      int c = 0;
      do {
         const int q2 = int(dec.bits(unsigned(extra)));
         float offset = (float(q2) + .5f) * float(1 << (14 - extra)) * (1.f / 16384) - .5f;
         offset *= float(1 << 14) * (1.f / 16384);
         old_ebands[i + c * band_stride] += offset;
      } while (++c < C);
   }
}

inline void unquant_energy_finalise(int start, int end, float* old_ebands, const int* fine_quant,
                                    const int* fine_priority, int bits_left, RangeDecoder& dec, int C) noexcept {
   for (int prio = 0; prio < 2; prio++) {
      for (int i = start; i < end && bits_left >= C; i++) {
         if (fine_quant[i] >= max_fine_bits || fine_priority[i] != prio) continue;
         int c = 0;
         do {
            const int q2 = int(dec.bits(1));
            const float offset = (float(q2) - .5f) * float(1 << (14 - fine_quant[i] - 1)) * (1.f / 16384);
            old_ebands[i + c * band_stride] += offset;
            bits_left--;
         } while (++c < C);
      }
   }
}

// ---------------------------------------------------------------------------
// vq.c
// ---------------------------------------------------------------------------

inline void exp_rotation1(float* x, int len, int stride, float c, float s) noexcept {
   const float ms = -s;
   float* xptr = x;
   // Serial: iteration i reads what iteration i - stride wrote.
   OPUSPP_NO_VECTORIZE
   for (int i = 0; i < len - stride; i++) {
      const float x1 = xptr[0];
      const float x2 = xptr[stride];
      xptr[stride] = c * x2 + s * x1;
      *xptr++ = c * x1 + ms * x2;
   }
   xptr = &x[len - 2 * stride - 1];
   OPUSPP_NO_VECTORIZE
   for (int i = len - 2 * stride - 1; i >= 0; i--) {
      const float x1 = xptr[0];
      const float x2 = xptr[stride];
      xptr[stride] = c * x2 + s * x1;
      *xptr-- = c * x1 + ms * x2;
   }
}

// Spreading rotation, decoder direction only (exp_rotation with dir = -1).
inline void exp_rotation(float* x, int len, int stride, int K, int spread) noexcept {
   static constexpr int spread_factor[3] = {15, 10, 5};
   if (2 * K >= len || spread == spread_none) return;
   const int factor = spread_factor[spread - 1];
   const float gain = (1.f * float(len)) / float(len + factor * K);
   const float theta = .5f * (gain * gain);
   const float c = celt_cos_norm(theta);
   const float s = celt_cos_norm(1.f - theta);  // sin(theta)
   int stride2 = 0;
   if (len >= 8 * stride) {
      stride2 = 1;
      // Equivalent to computing sqrt(len/stride) with rounding.
      while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len) stride2++;
   }
   len = int(celt_udiv(std::uint32_t(len), std::uint32_t(stride)));
   for (int i = 0; i < stride; i++) {
      if (stride2) exp_rotation1(x + i * len, len, stride2, s, c);
      exp_rotation1(x + i * len, len, 1, c, s);
   }
}

// Scales x to unit norm times gain. x may have any float alignment within a
// 16-byte aligned array.
inline void renormalise_vector(float* x, int n, float gain) noexcept {
   const float e = epsilon + inner_prod_self(x, n);
   const float g = celt_rsqrt(e) * gain;
   const f32x4 gv = f32x4::broadcast(g);
   for_each_aligned(
      x, n, [&](int i) { (gv * f32x4::load_aligned(x + i)).store_aligned(x + i); },
      [&](int i) { x[i] = g * x[i]; });
}

// Four consecutive states of the CELT LCG (celt_lcg_rand) per step, using the
// jump-ahead constants seed_{k+j} = A_j * seed_k + C_j (mod 2^32). Integer
// arithmetic, so the sequence is identical to the scalar one.
struct Lcg4 {
   static constexpr std::uint32_t a = 1664525u, c = 1013904223u;
   static constexpr std::uint32_t pow_a(int k) {
      std::uint32_t r = 1;
      for (int i = 0; i < k; i++) r *= a;
      return r;
   }
   static constexpr std::uint32_t sum_c(int k) {  // c * (1 + a + ... + a^(k-1))
      std::uint32_t r = 0;
      for (int i = 0; i < k; i++) r = r * a + c;
      return r;
   }
   u32x4 s;  // the next four seeds
   explicit Lcg4(std::uint32_t seed) noexcept
      : s(u32x4::set(pow_a(1), pow_a(2), pow_a(3), pow_a(4)) * u32x4::broadcast(seed) +
          u32x4::set(sum_c(1), sum_c(2), sum_c(3), sum_c(4))) {}
   u32x4 next() noexcept {
      const u32x4 r = s;
      s = s * u32x4::broadcast(pow_a(4)) + u32x4::broadcast(sum_c(4));
      return r;
   }
};

// x[j] = (int32)seed_j >> 20 for the next n LCG states; returns the last seed.
// x may have any float alignment within a 16-byte aligned array.
inline std::uint32_t lcg_noise(float* x, int n, std::uint32_t seed) noexcept {
   int j = 0;
   const int head = min(n, (4 - misalignment(x)) & 3);
   for (; j < head; j++) {
      seed = celt_lcg_rand(seed);
      x[j] = float(std::int32_t(seed) >> 20);
   }
   if (j + 4 <= n) {
      Lcg4 lcg(seed);
      for (; j + 4 <= n; j += 4) {
         const u32x4 r = lcg.next();
         convert<float>(bit_cast<std::int32_t>(r) >> 20).store_aligned(x + j);
         seed = r[3];
      }
   }
   for (; j < n; j++) {
      seed = celt_lcg_rand(seed);
      x[j] = float(std::int32_t(seed) >> 20);
   }
   return seed;
}

// x[j] = lowband[j] +/- 1/256 with the sign from bit 15 of the next LCG
// states (folding with a bit of noise); returns the last seed.
inline std::uint32_t lcg_fold(float* x, const float* lowband, int n, std::uint32_t seed) noexcept {
   constexpr float eps = 1.0f / 256;
   int j = 0;
   const int head = min(n, (4 - misalignment(x)) & 3);
   for (; j < head; j++) {
      seed = celt_lcg_rand(seed);
      x[j] = lowband[j] + ((seed & 0x8000) ? eps : -eps);
   }
   if (j + 4 <= n) {
      with_misalignment(lowband + j, [&]<int R>() {
         // Sign bit set where bit 15 of the seed is clear: {-eps, +eps}.
         const u32x4 eps_bits = u32x4::broadcast(std::bit_cast<std::uint32_t>(eps));
         const float* la = lowband + j - R;
         Lcg4 lcg(seed);
         f32x4 b0 = f32x4::load_aligned(la);
         for (; j + 4 <= n; j += 4, la += 4) {
            f32x4 lv = b0;
            if constexpr (R > 0) {
               const f32x4 b1 = f32x4::load_aligned(la + 4);
               lv = window<R>(b0, b1);
               b0 = b1;
            } else if (j + 8 <= n) {
               b0 = f32x4::load_aligned(la + 4);
            }
            const u32x4 r = lcg.next();
            const u32x4 sign = ((r ^ u32x4::broadcast(0x8000u)) & u32x4::broadcast(0x8000u)) << 16;
            (lv + bit_cast<float>(eps_bits | sign)).store_aligned(x + j);
            seed = r[3];
         }
      });
   }
   for (; j < n; j++) {
      seed = celt_lcg_rand(seed);
      x[j] = lowband[j] + ((seed & 0x8000) ? eps : -eps);
   }
   return seed;
}

inline unsigned extract_collapse_mask(const int* iy, int n, int B) noexcept {
   if (B <= 1) return 1;
   const int n0 = int(celt_udiv(std::uint32_t(n), std::uint32_t(B)));
   unsigned collapse_mask = 0;
   int i = 0;
   do {
      unsigned tmp = 0;
      int j = 0;
      OPUSPP_NO_VECTORIZE
      do tmp |= unsigned(iy[i * n0 + j]);
      while (++j < n0);
      collapse_mask |= unsigned(tmp != 0) << i;
   } while (++i < B);
   return collapse_mask;
}

// Decodes the pulse vector and turns it into a normalised band (alg_unquant).
inline unsigned alg_unquant(float* x, int n, int K, int spread, int B, RangeDecoder& dec, float gain) noexcept {
   OPUSPP_ASSERT(K > 0 && n > 1 && n <= max_band_size);
   // iy gets the same misalignment as x, so both are aligned in the vector body.
   alignas(buffer_alignment) int iy_buf[max_band_size + 4];
   int* iy = iy_buf + misalignment(x);
   const float ryy = decode_pulses(iy, n, K, dec);
   // normalise_residual()
   const float g = celt_rsqrt(ryy) * gain;
   const f32x4 gv = f32x4::broadcast(g);
   for_each_aligned(
      x, n,
      [&](int i) {
         const i32x4 v = i32x4::load_aligned(iy + i);
         (convert<float>(v) * gv).store_aligned(x + i);
      },
      [&](int i) { x[i] = float(iy[i]) * g; });
   exp_rotation(x, n, B, K, spread);
   return extract_collapse_mask(iy, n, B);
}

// ---------------------------------------------------------------------------
// bands.c
// ---------------------------------------------------------------------------

// Bit-exact cos approximation used by the bit allocation.
constexpr OPUSPP_INLINE std::int32_t frac_mul16(std::int32_t a, std::int32_t b) noexcept {
   return (16384 + std::int32_t(std::int16_t(a)) * std::int16_t(b)) >> 15;
}

constexpr inline std::int16_t bitexact_cos(std::int16_t x) noexcept {
   const std::int32_t tmp = (4096 + std::int32_t(x) * x) >> 13;
   std::int16_t x2 = std::int16_t(tmp);
   x2 = std::int16_t((32767 - x2) + frac_mul16(x2, (-7651 + frac_mul16(x2, (8277 + frac_mul16(-626, x2))))));
   return std::int16_t(1 + x2);
}

constexpr inline int bitexact_log2tan(int isin, int icos) noexcept {
   const int lc = ec_ilog(std::uint32_t(icos));
   const int ls = ec_ilog(std::uint32_t(isin));
   icos <<= 15 - lc;
   isin <<= 15 - ls;
   return (ls - lc) * (1 << 11) + frac_mul16(isin, frac_mul16(isin, -2597) + 7932) -
          frac_mul16(icos, frac_mul16(icos, -2597) + 7932);
}

// 4x4 transpose of {a, b, c, d}.
OPUSPP_INLINE void transpose4(f32x4& a, f32x4& b, f32x4& c, f32x4& d) noexcept {
   const f32x4 t0 = shuffle<0, 4, 1, 5>(a, b);
   const f32x4 t1 = shuffle<2, 6, 3, 7>(a, b);
   const f32x4 t2 = shuffle<0, 4, 1, 5>(c, d);
   const f32x4 t3 = shuffle<2, 6, 3, 7>(c, d);
   a = shuffle<0, 1, 4, 5>(t0, t2);
   b = shuffle<2, 3, 6, 7>(t0, t2);
   c = shuffle<0, 1, 4, 5>(t1, t3);
   d = shuffle<2, 3, 6, 7>(t1, t3);
}

// One level of the Haar transform on interleaved pairs (x[s*2j+i], x[s*(2j+1)+i]).
inline void haar1(float* x, int n0, int stride) noexcept {
   n0 >>= 1;
   const float k = .70710678f;
   const int n = 2 * n0 * stride;
   if (misalignment(x) == 0 && n % 4 == 0) {
      const f32x4 kv = f32x4::broadcast(k);
      if (stride % 4 == 0) {
         // Pairs of whole rows: x[2j*s + i] with x[(2j+1)*s + i].
         for (int j = 0; j < n0; j++) {
            float* r0 = x + stride * 2 * j;
            float* r1 = r0 + stride;
            for (int i = 0; i < stride; i += 4) {
               const f32x4 t1 = kv * f32x4::load_aligned(r0 + i);
               const f32x4 t2 = kv * f32x4::load_aligned(r1 + i);
               (t1 + t2).store_aligned(r0 + i);
               (t1 - t2).store_aligned(r1 + i);
            }
         }
         return;
      }
      if (stride == 1 || stride == 2) {
         // Both elements of a pair lie in the same vector.
         for (int i = 0; i < n; i += 4) {
            const f32x4 t = kv * f32x4::load_aligned(x + i);
            f32x4 r;
            if (stride == 1)  // {t0+t1, t0-t1, t2+t3, t2-t3}
               r = shuffle<0, 0, 2, 2>(t, t) + shuffle<1, 1, 3, 3>(t, t) * f32x4::set(1.f, -1.f, 1.f, -1.f);
            else  // {t0+t2, t1+t3, t0-t2, t1-t3}
               r = shuffle<0, 1, 0, 1>(t, t) + shuffle<2, 3, 2, 3>(t, t) * f32x4::set(1.f, 1.f, -1.f, -1.f);
            r.store_aligned(x + i);
         }
         return;
      }
   }
   // Fallback for misaligned data or partial vectors.
   for (int i = 0; i < stride; i++) {
      OPUSPP_NO_VECTORIZE
      for (int j = 0; j < n0; j++) {
         const float tmp1 = k * x[stride * 2 * j + i];
         const float tmp2 = k * x[stride * (2 * j + 1) + i];
         x[stride * 2 * j + i] = tmp1 + tmp2;
         x[stride * (2 * j + 1) + i] = tmp1 - tmp2;
      }
   }
}

// Indexing table for converting from natural Hadamard to ordery Hadamard.
inline constexpr int ordery_table[] = {
   1, 0, 3, 0, 2, 1, 7, 0, 4, 3, 6, 1, 5, 2, 15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5,
};

// Transposes x from n0 rows of `stride` columns into `stride` rows of n0
// (tmp[row(i)*n0 + j] = x[j*stride + i]), or back when `inverse` is set.
inline void hadamard_transpose(float* OPUSPP_RESTRICT dst, const float* OPUSPP_RESTRICT src, int n0, int stride,
                               int hadamard, bool inverse) noexcept {
   const int* ordery = ordery_table + stride - 2;
   auto row = [&](int i) { return hadamard ? ordery[i] : i; };
   if (misalignment(src) == 0 && misalignment(dst) == 0 && n0 % 4 == 0 && (stride % 4 == 0 || stride == 2)) {
      if (stride == 2) {
         for (int j = 0; j < n0; j += 4) {
            if (!inverse) {  // even/odd split
               const f32x4 a = f32x4::load_aligned(src + 2 * j), b = f32x4::load_aligned(src + 2 * j + 4);
               shuffle<0, 2, 4, 6>(a, b).store_aligned(dst + row(0) * n0 + j);
               shuffle<1, 3, 5, 7>(a, b).store_aligned(dst + row(1) * n0 + j);
            } else {  // interleave
               const f32x4 e = f32x4::load_aligned(src + row(0) * n0 + j);
               const f32x4 o = f32x4::load_aligned(src + row(1) * n0 + j);
               shuffle<0, 4, 1, 5>(e, o).store_aligned(dst + 2 * j);
               shuffle<2, 6, 3, 7>(e, o).store_aligned(dst + 2 * j + 4);
            }
         }
         return;
      }
      for (int i = 0; i < stride; i += 4)
         for (int j = 0; j < n0; j += 4) {
            if (!inverse) {
               f32x4 a = f32x4::load_aligned(src + j * stride + i);
               f32x4 b = f32x4::load_aligned(src + (j + 1) * stride + i);
               f32x4 c = f32x4::load_aligned(src + (j + 2) * stride + i);
               f32x4 d = f32x4::load_aligned(src + (j + 3) * stride + i);
               transpose4(a, b, c, d);
               a.store_aligned(dst + row(i) * n0 + j);
               b.store_aligned(dst + row(i + 1) * n0 + j);
               c.store_aligned(dst + row(i + 2) * n0 + j);
               d.store_aligned(dst + row(i + 3) * n0 + j);
            } else {
               f32x4 a = f32x4::load_aligned(src + row(i) * n0 + j);
               f32x4 b = f32x4::load_aligned(src + row(i + 1) * n0 + j);
               f32x4 c = f32x4::load_aligned(src + row(i + 2) * n0 + j);
               f32x4 d = f32x4::load_aligned(src + row(i + 3) * n0 + j);
               transpose4(a, b, c, d);
               a.store_aligned(dst + j * stride + i);
               b.store_aligned(dst + (j + 1) * stride + i);
               c.store_aligned(dst + (j + 2) * stride + i);
               d.store_aligned(dst + (j + 3) * stride + i);
            }
         }
      return;
   }
   // Fallback for misaligned data or partial vectors.
   for (int i = 0; i < stride; i++) {
      OPUSPP_NO_VECTORIZE
      for (int j = 0; j < n0; j++) {
         if (!inverse)
            dst[row(i) * n0 + j] = src[j * stride + i];
         else
            dst[j * stride + i] = src[row(i) * n0 + j];
      }
   }
}

inline void deinterleave_hadamard(float* x, int n0, int stride, int hadamard) noexcept {
   alignas(buffer_alignment) float tmp[max_band_size];
   const int n = n0 * stride;
   OPUSPP_ASSERT(n <= max_band_size);
   hadamard_transpose(tmp, x, n0, stride, hadamard, false);
   copy_any(x, tmp, n);
}

inline void interleave_hadamard(float* x, int n0, int stride, int hadamard) noexcept {
   alignas(buffer_alignment) float tmp[max_band_size];
   const int n = n0 * stride;
   OPUSPP_ASSERT(n <= max_band_size);
   hadamard_transpose(tmp, x, n0, stride, hadamard, true);
   copy_any(x, tmp, n);
}

// Integer square root floor(sqrt(v)) for v < 2^24 (theta pdf decoding),
// equal to isqrt32(v): the correctly rounded float sqrt of an integer below
// 2^24 truncates to the exact root (k^2 - 1 stays distinguishable from k^2),
// and the correction below makes that a guarantee.
OPUSPP_INLINE unsigned isqrt_small(std::uint32_t v) noexcept {
   OPUSPP_ASSERT(v >= 1 && v < (1u << 24));  // isqrt32(0) is undefined too
   unsigned g = static_cast<unsigned>(celt_sqrt(static_cast<float>(v)));
   if (g * g > v) g--;
   else if ((g + 1) * (g + 1) <= v) g++;
   return g;
}

inline int compute_qn(int n, int b, int offset, int pulse_cap, int stereo) noexcept {
   static constexpr std::int16_t exp2_table8[8] = {16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
   int n2 = 2 * n - 1;
   if (stereo && n == 2) n2--;
   // The upper limit ensures that in a stereo split with itheta==16384, we'll
   // always have enough bits left over to code at least one pulse in the side.
   int qb = celt_sudiv(b + n2 * offset, n2);
   qb = min(b - pulse_cap - (4 << bitres), qb);
   qb = min(8 << bitres, qb);
   int qn;
   if (qb < (1 << bitres >> 1)) {
      qn = 1;
   } else {
      qn = exp2_table8[qb & 0x7] >> (14 - (qb >> bitres));
      qn = (qn + 1) >> 1 << 1;
   }
   OPUSPP_ASSERT(qn <= 256);
   return qn;
}

struct BandCtx {
   int i;
   int intensity;
   int spread;
   int tf_change;
   RangeDecoder* ec;
   std::int32_t remaining_bits;
   std::uint32_t seed;
   int disable_inv;
   int avoid_split_noise;
};

struct SplitCtx {
   int inv;
   int imid;
   int iside;
   int delta;
   int itheta;
   int qalloc;
};

inline void compute_theta(BandCtx& ctx, SplitCtx& sctx, int n, int* b, int B, int B0, int LM, int stereo,
                          int* fill) noexcept {
   constexpr int qtheta_offset = 4;
   constexpr int qtheta_offset_twophase = 16;
   RangeDecoder& ec = *ctx.ec;
   const int i = ctx.i;
   int itheta = 0;
   int inv = 0;

   // Decide on the resolution to give to the split parameter theta.
   const int pulse_cap = tables::logN400[i] + LM * (1 << bitres);
   const int offset = (pulse_cap >> 1) - (stereo && n == 2 ? qtheta_offset_twophase : qtheta_offset);
   int qn = compute_qn(n, *b, offset, pulse_cap, stereo);
   if (stereo && i >= ctx.intensity) qn = 1;
   const std::int32_t tell = std::int32_t(ec.tell_frac());
   if (qn != 1) {
      // Entropy decoding of the angle. We use a uniform pdf for the time
      // split, a step for stereo, and a triangular one for the rest.
      if (stereo && n > 2) {
         const int p0 = 3;
         const int x0 = qn / 2;
         const int ft = p0 * (x0 + 1) + x0;
         const int fs = int(ec.decode(unsigned(ft)));
         const int x = fs < (x0 + 1) * p0 ? fs / p0 : x0 + 1 + (fs - (x0 + 1) * p0);
         ec.update(unsigned(x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0),
                   unsigned(x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0), unsigned(ft));
         itheta = x;
      } else if (B0 > 1 || stereo) {
         itheta = int(ec.uint(std::uint32_t(qn + 1)));
      } else {
         int fs = 1, fl = 0;
         const int ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
         const int fm = int(ec.decode(unsigned(ft)));
         if (fm < ((qn >> 1) * ((qn >> 1) + 1) >> 1)) {
            itheta = int((isqrt_small(8 * std::uint32_t(fm) + 1) - 1) >> 1);
            fs = itheta + 1;
            fl = itheta * (itheta + 1) >> 1;
         } else {
            itheta = int((2 * (qn + 1) - int(isqrt_small(8 * std::uint32_t(ft - fm - 1) + 1))) >> 1);
            fs = qn + 1 - itheta;
            fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
         }
         ec.update(unsigned(fl), unsigned(fl + fs), unsigned(ft));
      }
      OPUSPP_ASSERT(itheta >= 0);
      itheta = int(celt_udiv(std::uint32_t(itheta) * 16384, std::uint32_t(qn)));
   } else if (stereo) {
      if (*b > 2 << bitres && ctx.remaining_bits > 2 << bitres)
         inv = ec.bit_logp(2);
      else
         inv = 0;
      // inv flag override to avoid problems with downmixing.
      if (ctx.disable_inv) inv = 0;
      itheta = 0;
   }
   const int qalloc = int(std::int32_t(ec.tell_frac()) - tell);
   *b -= qalloc;

   int imid, iside, delta;
   if (itheta == 0) {
      imid = 32767;
      iside = 0;
      *fill &= (1 << B) - 1;
      delta = -16384;
   } else if (itheta == 16384) {
      imid = 0;
      iside = 32767;
      *fill &= ((1 << B) - 1) << B;
      delta = 16384;
   } else {
      imid = bitexact_cos(std::int16_t(itheta));
      iside = bitexact_cos(std::int16_t(16384 - itheta));
      // This is the mid vs side allocation that minimizes squared error in that band.
      delta = frac_mul16((n - 1) << 7, bitexact_log2tan(iside, imid));
   }
   sctx.inv = inv;
   sctx.imid = imid;
   sctx.iside = iside;
   sctx.delta = delta;
   sctx.itheta = itheta;
   sctx.qalloc = qalloc;
}

inline unsigned quant_band_n1(BandCtx& ctx, float* x, float* y, float* lowband_out) noexcept {
   const int stereo = y != nullptr;
   float* xp = x;
   int c = 0;
   do {
      int sign = 0;
      if (ctx.remaining_bits >= 1 << bitres) {
         sign = int(ctx.ec->bits(1));
         ctx.remaining_bits -= 1 << bitres;
      }
      xp[0] = sign ? -1.f : 1.f;
      xp = y;
   } while (++c < 1 + stereo);
   if (lowband_out) lowband_out[0] = x[0];
   return 1;
}

// Decodes a mono partition, recursively splitting it in two (quant_partition).
inline unsigned quant_partition(BandCtx& ctx, float* x, int n, int b, int B, float* lowband, int LM, float gain,
                                int fill) noexcept {
   const int i = ctx.i;
   const int B0 = B;
   unsigned cm = 0;

   // If we need 1.5 more bit than we can produce, split the band in two.
   const std::uint8_t* cache = pulse_cache(i, LM + 1);
   if (LM != -1 && b > cache[cache[0]] + 12 && n > 2) {
      SplitCtx sctx;
      float* next_lowband2 = nullptr;
      n >>= 1;
      float* y = x + n;
      LM -= 1;
      if (B == 1) fill = (fill & 1) | (fill << 1);
      B = (B + 1) >> 1;

      compute_theta(ctx, sctx, n, &b, B, B0, LM, 0, &fill);
      const int itheta = sctx.itheta;
      int delta = sctx.delta;
      const float mid = (1.f / 32768) * float(sctx.imid);
      const float side = (1.f / 32768) * float(sctx.iside);

      // Give more bits to low-energy MDCTs than they would otherwise deserve.
      if (B0 > 1 && (itheta & 0x3fff)) {
         if (itheta > 8192)
            // Rough approximation for pre-echo masking.
            delta -= delta >> (4 - LM);
         else
            // Corresponds to a forward-masking slope of 1.5 dB per 10 ms.
            delta = min(0, delta + (n << bitres >> (5 - LM)));
      }
      int mbits = max(0, min(b, (b - delta) / 2));
      int sbits = b - mbits;
      ctx.remaining_bits -= sctx.qalloc;

      if (lowband) next_lowband2 = lowband + n;  // >32-bit split case

      std::int32_t rebalance = ctx.remaining_bits;
      if (mbits >= sbits) {
         cm = quant_partition(ctx, x, n, mbits, B, lowband, LM, gain * mid, fill);
         rebalance = mbits - (rebalance - ctx.remaining_bits);
         if (rebalance > 3 << bitres && itheta != 0) sbits += int(rebalance) - (3 << bitres);
         cm |= quant_partition(ctx, y, n, sbits, B, next_lowband2, LM, gain * side, fill >> B) << (B0 >> 1);
      } else {
         cm = quant_partition(ctx, y, n, sbits, B, next_lowband2, LM, gain * side, fill >> B) << (B0 >> 1);
         rebalance = sbits - (rebalance - ctx.remaining_bits);
         if (rebalance > 3 << bitres && itheta != 16384) mbits += int(rebalance) - (3 << bitres);
         cm |= quant_partition(ctx, x, n, mbits, B, lowband, LM, gain * mid, fill);
      }
      return cm;
   }

   // This is the basic no-split case.
   int q = bits2pulses(i, LM, b);
   int curr_bits = pulses2bits(i, LM, q);
   ctx.remaining_bits -= curr_bits;
   // Ensures we can never bust the budget.
   while (ctx.remaining_bits < 0 && q > 0) {
      ctx.remaining_bits += curr_bits;
      q--;
      curr_bits = pulses2bits(i, LM, q);
      ctx.remaining_bits -= curr_bits;
   }

   if (q != 0) {
      const int K = get_pulses(q);
      return alg_unquant(x, n, K, ctx.spread, B, *ctx.ec, gain);
   }

   // If there's no pulse, fill the band anyway.
   const unsigned cm_mask = unsigned(1UL << B) - 1;
   fill &= int(cm_mask);
   if (!fill) {
      for_each_aligned(
         x, n, [&](int j) { f32x4::zero().store_aligned(x + j); }, [&](int j) { x[j] = 0; });
      return 0;
   }
   if (lowband == nullptr) {
      // Noise.
      ctx.seed = lcg_noise(x, n, ctx.seed);
      cm = cm_mask;
   } else {
      // Folded spectrum, about 48 dB below the "normal" folding level.
      ctx.seed = lcg_fold(x, lowband, n, ctx.seed);
      cm = unsigned(fill);
   }
   renormalise_vector(x, n, gain);
   return cm;
}

// Decodes a band for the mono case (quant_band). Always inlined: it is not
// recursive, and as a call its many arguments would be passed on the stack.
OPUSPP_INLINE unsigned quant_band(BandCtx& ctx, float* x, int n, int b, int B, float* lowband, int LM,
                           float* lowband_out, float gain, float* lowband_scratch, int fill) noexcept {
   static constexpr unsigned char bit_interleave_table[16] = {0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3};
   static constexpr unsigned char bit_deinterleave_table[16] = {0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
                                                                0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF};
   const int n0 = n;
   int n_b = n;
   int B0 = B;
   int time_divide = 0;
   int recombine = 0;
   const int long_blocks = B0 == 1;
   int tf_change = ctx.tf_change;

   n_b = int(celt_udiv(std::uint32_t(n_b), std::uint32_t(B)));

   // Special case for one sample.
   if (n == 1) return quant_band_n1(ctx, x, nullptr, lowband_out);

   if (tf_change > 0) recombine = tf_change;
   // Band recombining to increase frequency resolution.
   if (lowband_scratch && lowband && (recombine || ((n_b & 1) == 0 && tf_change < 0) || B0 > 1)) {
      copy_any(lowband_scratch, lowband, n);
      lowband = lowband_scratch;
   }
   for (int k = 0; k < recombine; k++) {
      if (lowband) haar1(lowband, n >> k, 1 << k);
      fill = bit_interleave_table[fill & 0xF] | bit_interleave_table[fill >> 4] << 2;
   }
   B >>= recombine;
   n_b <<= recombine;

   // Increasing the time resolution.
   while ((n_b & 1) == 0 && tf_change < 0) {
      if (lowband) haar1(lowband, n_b, B);
      fill |= fill << B;
      B <<= 1;
      n_b >>= 1;
      time_divide++;
      tf_change++;
   }
   B0 = B;
   const int n_b0 = n_b;

   // Reorganize the samples in time order instead of frequency order.
   if (B0 > 1 && lowband) deinterleave_hadamard(lowband, n_b >> recombine, B0 << recombine, long_blocks);

   unsigned cm = quant_partition(ctx, x, n, b, B, lowband, LM, gain, fill);

   // Undo the sample reorganization going from time order to frequency order.
   if (B0 > 1) interleave_hadamard(x, n_b >> recombine, B0 << recombine, long_blocks);

   // Undo time-freq changes that we did earlier.
   n_b = n_b0;
   B = B0;
   for (int k = 0; k < time_divide; k++) {
      B >>= 1;
      n_b <<= 1;
      cm |= cm >> B;
      haar1(x, n_b, B);
   }
   for (int k = 0; k < recombine; k++) {
      cm = bit_deinterleave_table[cm];
      haar1(x, n0 >> k, 1 << k);
   }
   B <<= recombine;

   // Scale output for later folding.
   if (lowband_out) {
      // Both are band starts at LM = 2: 16-byte aligned, n0 a multiple of 4.
      OPUSPP_ASSERT(misalignment(x) == 0 && misalignment(lowband_out) == 0 && n0 % 4 == 0);
      const f32x4 nrm = f32x4::broadcast(celt_sqrt(float(n0)));
      for (int j = 0; j < n0; j += 4) (nrm * f32x4::load_aligned(x + j)).store_aligned(lowband_out + j);
   }
   cm &= (1 << B) - 1;
   return cm;
}

// x and y are band starts (16-byte aligned, n a multiple of 4).
inline void stereo_merge(float* x, float* y, float mid, int n) noexcept {
   OPUSPP_ASSERT(misalignment(x) == 0 && misalignment(y) == 0 && n % 4 == 0);
   // Compute the norm of X+Y and X-Y as |X|^2 + |Y|^2 +/- sum(xy).
   float xp = inner_prod<0, 0>(y, x, n);
   const float side = inner_prod<0, 0>(y, y, n);
   // Compensating for the mid normalization.
   xp = mid * xp;
   const float el = mid * mid + side - 2 * xp;
   const float er = mid * mid + side + 2 * xp;
   if (er < 6e-4f || el < 6e-4f) {
      copy_aligned(y, x, n);
      return;
   }
   const f32x4 lgain = f32x4::broadcast(celt_rsqrt(el));
   const f32x4 rgain = f32x4::broadcast(celt_rsqrt(er));
   const f32x4 midv = f32x4::broadcast(mid);
   for (int j = 0; j < n; j += 4) {
      // Apply mid scaling (side is already scaled).
      const f32x4 l = midv * f32x4::load_aligned(x + j);
      const f32x4 r = f32x4::load_aligned(y + j);
      (lgain * (l - r)).store_aligned(x + j);
      (rgain * (l + r)).store_aligned(y + j);
   }
}

// Decodes a band for the stereo case (quant_band_stereo).
inline unsigned quant_band_stereo(BandCtx& ctx, float* x, float* y, int n, int b, int B, float* lowband, int LM,
                                  float* lowband_out, float* lowband_scratch, int fill) noexcept {
   // Special case for one sample.
   if (n == 1) return quant_band_n1(ctx, x, y, lowband_out);

   const int orig_fill = fill;
   SplitCtx sctx;
   compute_theta(ctx, sctx, n, &b, B, B, LM, 1, &fill);
   const int inv = sctx.inv;
   const int delta = sctx.delta;
   const int itheta = sctx.itheta;
   const int qalloc = sctx.qalloc;
   const float mid = (1.f / 32768) * float(sctx.imid);
   const float side = (1.f / 32768) * float(sctx.iside);
   unsigned cm = 0;

   if (n == 2) {
      // Special case for N=2 that only works for stereo and takes advantage of
      // the fact that mid and side are orthogonal to code the side with one bit.
      int mbits = b;
      int sbits = 0;
      if (itheta != 0 && itheta != 16384) sbits = 1 << bitres;
      mbits -= sbits;
      const int c = itheta > 8192;
      ctx.remaining_bits -= qalloc + sbits;
      float* x2 = c ? y : x;
      float* y2 = c ? x : y;
      int sign = 0;
      if (sbits) sign = int(ctx.ec->bits(1));
      sign = 1 - 2 * sign;
      // We use orig_fill here because we want to fold the side, but if
      // itheta==16384, we'll have cleared the low bits of fill.
      cm = quant_band(ctx, x2, n, mbits, B, lowband, LM, lowband_out, 1.f, lowband_scratch, orig_fill);
      // We don't split N=2 bands, so cm is either 1 or 0 (for a fold-collapse).
      y2[0] = float(-sign) * x2[1];
      y2[1] = float(sign) * x2[0];
      x[0] = mid * x[0];
      x[1] = mid * x[1];
      y[0] = side * y[0];
      y[1] = side * y[1];
      float tmp = x[0];
      x[0] = tmp - y[0];
      y[0] = tmp + y[0];
      tmp = x[1];
      x[1] = tmp - y[1];
      y[1] = tmp + y[1];
   } else {
      // "Normal" split code.
      int mbits = max(0, min(b, (b - delta) / 2));
      int sbits = b - mbits;
      ctx.remaining_bits -= qalloc;
      std::int32_t rebalance = ctx.remaining_bits;
      if (mbits >= sbits) {
         // In stereo mode, we do not apply a scaling to the mid because we
         // need the normalized mid for folding later.
         cm = quant_band(ctx, x, n, mbits, B, lowband, LM, lowband_out, 1.f, lowband_scratch, fill);
         rebalance = mbits - (rebalance - ctx.remaining_bits);
         if (rebalance > 3 << bitres && itheta != 0) sbits += int(rebalance) - (3 << bitres);
         // For a stereo split, the high bits of fill are always zero, so no
         // folding will be done to the side.
         cm |= quant_band(ctx, y, n, sbits, B, nullptr, LM, nullptr, side, nullptr, fill >> B);
      } else {
         cm = quant_band(ctx, y, n, sbits, B, nullptr, LM, nullptr, side, nullptr, fill >> B);
         rebalance = sbits - (rebalance - ctx.remaining_bits);
         if (rebalance > 3 << bitres && itheta != 16384) mbits += int(rebalance) - (3 << bitres);
         cm |= quant_band(ctx, x, n, mbits, B, lowband, LM, lowband_out, 1.f, lowband_scratch, fill);
      }
   }

   if (n != 2) stereo_merge(x, y, mid, n);
   if (inv) {
      if (n % 4 == 0) {
         for (int j = 0; j < n; j += 4) (-f32x4::load_aligned(y + j)).store_aligned(y + j);
      } else {
         OPUSPP_NO_VECTORIZE
         for (int j = 0; j < n; j++) y[j] = -y[j];
      }
   }
   return cm;
}

inline void special_hybrid_folding(float* norm, float* norm2, int start, int M, int dual_stereo) noexcept {
   const int n1 = M * (ebands[start + 1] - ebands[start]);
   const int n2 = M * (ebands[start + 2] - ebands[start + 1]);
   // Duplicate enough of the first band folding data to be able to fold the
   // second band. Copies no data for CELT-only mode.
   if (n2 > n1) {
      copy(&norm[n1], &norm[2 * n1 - n2], n2 - n1);
      if (dual_stereo) copy(&norm2[n1], &norm2[2 * n1 - n2], n2 - n1);
   }
}

// Decodes all the bands of a frame (quant_all_bands with encode = 0).
inline void quant_all_bands(int start, int end, float* X_, float* Y_, unsigned char* collapse_masks,
                            const int* pulses, int short_blocks, int spread, int dual_stereo, int intensity,
                            const int* tf_res, std::int32_t total_bits, std::int32_t balance, RangeDecoder& ec,
                            int coded_bands, std::uint32_t* seed, int disable_inv) noexcept {
   constexpr int M = 1 << lm;
   constexpr int norm_size = M * 78;  // M*eBands[nbEBands-1]
   const int B = short_blocks ? M : 1;
   const int C = Y_ != nullptr ? 2 : 1;
   const int norm_offset = M * ebands[start];
   alignas(buffer_alignment) float norm_buf[2 * norm_size];
   float* norm = norm_buf;
   float* norm2 = norm + M * ebands[nb_ebands - 1] - norm_offset;
   // For decoding, the last band is used as scratch space because its data
   // is not needed until it's decoded.
   float* lowband_scratch = X_ + M * ebands[nb_ebands - 1];
   int lowband_offset = 0;
   int update_lowband = 1;

   BandCtx ctx;
   ctx.i = 0;
   ctx.tf_change = 0;
   ctx.remaining_bits = 0;
   ctx.ec = &ec;
   ctx.intensity = intensity;
   ctx.seed = *seed;
   ctx.spread = spread;
   ctx.disable_inv = disable_inv;
   // Avoid injecting noise in the first band on transients.
   ctx.avoid_split_noise = B > 1;

   for (int i = start; i < end; i++) {
      ctx.i = i;
      const int last = i == end - 1;
      float* X = X_ + M * ebands[i];
      float* Y = Y_ != nullptr ? Y_ + M * ebands[i] : nullptr;
      const int N = M * ebands[i + 1] - M * ebands[i];
      const std::int32_t tell = std::int32_t(ec.tell_frac());

      // Compute how many bits we want to allocate to this band.
      if (i != start) balance -= tell;
      const std::int32_t remaining_bits = total_bits - tell - 1;
      ctx.remaining_bits = remaining_bits;
      int b;
      if (i <= coded_bands - 1) {
         const std::int32_t curr_balance = celt_sudiv(balance, min(3, coded_bands - i));
         b = int(max(std::int32_t(0), min(std::int32_t(16383), min(remaining_bits + 1, pulses[i] + curr_balance))));
      } else {
         b = 0;
      }

      if ((M * ebands[i] - N >= M * ebands[start] || i == start + 1) && (update_lowband || lowband_offset == 0))
         lowband_offset = i;
      if (i == start + 1) special_hybrid_folding(norm, norm2, start, M, dual_stereo);

      const int tf_change = tf_res[i];
      ctx.tf_change = tf_change;
      if (last) lowband_scratch = nullptr;

      // Get a conservative estimate of the collapse_mask's for the bands
      // we're going to be folding from.
      int effective_lowband = -1;
      unsigned x_cm, y_cm;
      if (lowband_offset != 0 && (spread != spread_aggressive || B > 1 || tf_change < 0)) {
         // This ensures we never repeat spectral content within one band.
         effective_lowband = max(0, M * ebands[lowband_offset] - norm_offset - N);
         int fold_start = lowband_offset;
         while (M * ebands[--fold_start] > effective_lowband + norm_offset) {
         }
         int fold_end = lowband_offset - 1;
         while (++fold_end < i && M * ebands[fold_end] < effective_lowband + norm_offset + N) {
         }
         x_cm = y_cm = 0;
         int fold_i = fold_start;
         OPUSPP_NO_VECTORIZE
         do {
            x_cm |= collapse_masks[fold_i * C + 0];
            y_cm |= collapse_masks[fold_i * C + C - 1];
         } while (++fold_i < fold_end);
      } else {
         // Otherwise, we'll be using the LCG to fold, so all blocks will
         // (almost always) be non-zero.
         x_cm = y_cm = (1u << B) - 1;
      }

      if (dual_stereo && i == intensity) {
         // Switch off dual stereo to do intensity.
         dual_stereo = 0;
         // norm and norm2 are 16-byte aligned; band offsets are multiples of 4.
         const f32x4 half = f32x4::broadcast(.5f);
         for (int j = 0; j < M * ebands[i] - norm_offset; j += 4)
            (half * (f32x4::load_aligned(norm + j) + f32x4::load_aligned(norm2 + j))).store_aligned(norm + j);
      }
      if (dual_stereo) {
         x_cm = quant_band(ctx, X, N, b / 2, B, effective_lowband != -1 ? norm + effective_lowband : nullptr, lm,
                           last ? nullptr : norm + M * ebands[i] - norm_offset, 1.f, lowband_scratch, int(x_cm));
         y_cm = quant_band(ctx, Y, N, b / 2, B, effective_lowband != -1 ? norm2 + effective_lowband : nullptr, lm,
                           last ? nullptr : norm2 + M * ebands[i] - norm_offset, 1.f, lowband_scratch, int(y_cm));
      } else {
         if (Y != nullptr) {
            x_cm = quant_band_stereo(ctx, X, Y, N, b, B,
                                     effective_lowband != -1 ? norm + effective_lowband : nullptr, lm,
                                     last ? nullptr : norm + M * ebands[i] - norm_offset, lowband_scratch,
                                     int(x_cm | y_cm));
         } else {
            x_cm = quant_band(ctx, X, N, b, B, effective_lowband != -1 ? norm + effective_lowband : nullptr, lm,
                              last ? nullptr : norm + M * ebands[i] - norm_offset, 1.f, lowband_scratch,
                              int(x_cm | y_cm));
         }
         y_cm = x_cm;
      }
      collapse_masks[i * C + 0] = static_cast<unsigned char>(x_cm);
      collapse_masks[i * C + C - 1] = static_cast<unsigned char>(y_cm);
      balance += pulses[i] + tell;

      // Update the folding position only as long as we have 1 bit/sample depth.
      update_lowband = b > (N << bitres);
      // We only need to avoid noise on a split for the first band. After
      // that, we have folding.
      ctx.avoid_split_noise = 0;
   }
   *seed = ctx.seed;
}

// Prevents energy collapse for transients with multiple short MDCTs.
inline void anti_collapse(float* X_, const unsigned char* collapse_masks, int C, int size, int start, int end,
                          const float* log_e, const float* prev1_log_e, const float* prev2_log_e,
                          const int* pulses, std::uint32_t seed) noexcept {
   for (int i = start; i < end; i++) {
      const int N0 = ebands[i + 1] - ebands[i];
      // depth in 1/8 bits
      const int depth = int(celt_udiv(std::uint32_t(1 + pulses[i]), std::uint32_t(N0)) >> lm);
      const float thresh = .5f * celt_exp2(-.125f * float(depth));
      const float sqrt_1 = celt_rsqrt(float(N0 << lm));
      int c = 0;
      do {
         float prev1 = prev1_log_e[c * band_stride + i];
         float prev2 = prev2_log_e[c * band_stride + i];
         if (C == 1) {
            prev1 = max(prev1, prev1_log_e[band_stride + i]);
            prev2 = max(prev2, prev2_log_e[band_stride + i]);
         }
         float ediff = log_e[c * band_stride + i] - min(prev1, prev2);
         ediff = max(0.f, ediff);
         // r needs to be multiplied by 2 or 2*sqrt(2) depending on LM because
         // short blocks don't have the same energy as long.
         float r = 2.f * celt_exp2(-ediff);
         r = min(thresh, r);
         r = r * sqrt_1;
         float* X = X_ + c * size + (ebands[i] << lm);
         int renormalize = 0;
         for (int k = 0; k < 1 << lm; k++) {
            // Detect collapse.
            if (!(collapse_masks[i * C + c] & 1 << k)) {
               // Fill with noise.
               for (int j = 0; j < N0; j++) {
                  seed = celt_lcg_rand(seed);
                  X[(j << lm) + k] = (seed & 0x8000 ? r : -r);
               }
               renormalize = 1;
            }
         }
         // We just added some energy, so we need to renormalise.
         if (renormalize) renormalise_vector(X, N0 << lm, 1.f);
      } while (++c < C);
   }
}

// Converts normalised bands back to MDCT coefficients (denormalise_bands).
// X and freq are 16-byte aligned; at LM = 2 every band spans a multiple of 4
// coefficients, so each band is processed in whole aligned vectors.
inline void denormalise_bands(const float* X, float* OPUSPP_RESTRICT freq, const float* band_log_e, int start,
                              int end, int silence) noexcept {
   constexpr int M = 1 << lm;
   constexpr int N = frame_size;
   static_assert(M % 4 == 0);
   OPUSPP_ASSERT(misalignment(X) == 0 && misalignment(freq) == 0);
   int bound = M * ebands[end];
   if (silence) {
      bound = 0;
      start = end = 0;
   }
   fill_aligned<64>(freq, 0.f, M * ebands[start]);
   for (int i = start; i < end; i++) {
      const float lg = band_log_e[i] + tables::e_means[i];
      const f32x4 g = f32x4::broadcast(celt_exp2(min(32.f, lg)));
      for (int j = M * ebands[i]; j < M * ebands[i + 1]; j += 4)
         (f32x4::load_aligned(X + j) * g).store_aligned(freq + j);
   }
   fill_aligned<64>(freq + bound, 0.f, N - bound);  // bound is 0 or 400 floats
}

} // namespace opuspp::detail
