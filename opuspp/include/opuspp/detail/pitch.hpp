// Correlation kernels, LPC helpers, pitch search and the comb (post-)filter
// (celt/pitch.[ch], celt/celt_lpc.c, celt/celt.c, celt/x86/pitch_sse.c).
//
// The kernels mirror the structure of the libopus SSE/NEON float kernels
// (4-lane partial sums, same accumulation order) using simd<float, 4>.
//
// Copyright (c) 2007-2008 CSIRO, 2007-2009 Xiph.Org Foundation,
// 2008-2009 Gregory Maxwell, 2014 Cisco Systems.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "celt_tables.hpp"
#include "config.hpp"
#include "math.hpp"
#include "simd.hpp"

namespace opuspp::detail {

// All kernels take 16-byte aligned `x` operands; `y` operands may have any
// float alignment and are read through aligned blocks and shuffles
// (see window() in simd.hpp), never with unaligned loads.

// Returns init + {sum_j x[j]*y[j+k]}_{k=0..3} with the partial-sum structure of
// xcorr_kernel_sse. y = ya + R with ya 16-byte aligned; x is 16-byte aligned.
template <int R>
OPUSPP_INLINE f32x4 xcorr_kernel(const float* x, const float* y, f32x4 init, int len) noexcept {
   OPUSPP_ASSERT(misalignment(x) == 0 && misalignment(y) == R && len >= 3);
   const float* ya = y - R;
   f32x4 xsum1 = init;
   f32x4 xsum2 = f32x4::zero();
   // Loop invariant: b0 holds block j, b1/b2 blocks j+4/j+8 once loaded.
   f32x4 b0 = f32x4::load_aligned(ya);
   f32x4 b1 = b0, b2 = b0;
   int j = 0;
   for (; j < len - 3; j += 4) {
      // Needs y[j..j+6] = ya[j+R..j+R+6].
      b1 = f32x4::load_aligned(ya + j + 4);
      if constexpr (R >= 2) b2 = f32x4::load_aligned(ya + j + 8);
      const f32x4 x0 = f32x4::load_aligned(x + j);
      xsum1 += f32x4::broadcast(x0[0]) * window<R>(b0, b1, b2);
      xsum2 += f32x4::broadcast(x0[1]) * window<R + 1>(b0, b1, b2);
      xsum1 += f32x4::broadcast(x0[2]) * window<R + 2>(b0, b1, b2);
      xsum2 += f32x4::broadcast(x0[3]) * window<R + 3>(b0, b1, b2);
      b0 = b1;
   }
   if (j < len) {
      // y[j..j+3] = ya[j+R..j+R+3]
      if constexpr (R > 0) b1 = f32x4::load_aligned(ya + j + 4);
      xsum1 += f32x4::broadcast(x[j]) * window<R>(b0, b1);
      if (j + 1 < len) {
         // y[j+1..j+4]
         if constexpr (R == 0) b1 = f32x4::load_aligned(ya + j + 4);
         xsum2 += f32x4::broadcast(x[j + 1]) * window<R + 1>(b0, b1);
         if (j + 2 < len) {
            // y[j+2..j+5]
            if constexpr (R == 3) b2 = f32x4::load_aligned(ya + j + 8);
            xsum1 += f32x4::broadcast(x[j + 2]) * window<R + 2>(b0, b1, b2);
         }
      }
   }
   return xsum1 + xsum2;
}

// Inner product with 4-lane partial sums (celt_inner_prod_sse); x = xa + RX,
// y = ya + RY with xa, ya 16-byte aligned.
template <int RX, int RY>
OPUSPP_INLINE float inner_prod(const float* x, const float* y, int n) noexcept {
   OPUSPP_ASSERT(misalignment(x) == RX && misalignment(y) == RY);
   f32x4 sum = f32x4::zero();
   int i = 0;
   if (n >= 4) {
      const float* xa = x - RX;
      const float* ya = y - RY;
      f32x4 xb0 = f32x4::load_aligned(xa);
      f32x4 yb0 = f32x4::load_aligned(ya);
      for (; i < n - 3; i += 4) {
         f32x4 xv = xb0, yv = yb0;
         if constexpr (RX > 0) {
            const f32x4 xb1 = f32x4::load_aligned(xa + i + 4);
            xv = window<RX>(xb0, xb1);
            xb0 = xb1;
         } else if (i + 4 < n - 3) {
            xb0 = f32x4::load_aligned(xa + i + 4);
         }
         if constexpr (RY > 0) {
            const f32x4 yb1 = f32x4::load_aligned(ya + i + 4);
            yv = window<RY>(yb0, yb1);
            yb0 = yb1;
         } else if (i + 4 < n - 3) {
            yb0 = f32x4::load_aligned(ya + i + 4);
         }
         sum += xv * yv;
      }
   }
   float xy = sum.hsum();
   OPUSPP_NO_VECTORIZE
   for (; i < n; i++) xy = xy + x[i] * y[i];
   return xy;
}

// Inner product of an aligned x with an arbitrarily aligned y.
OPUSPP_INLINE float inner_prod(const float* x, const float* y, int n) noexcept {
   return with_misalignment(y, [&]<int RY>() { return inner_prod<0, RY>(x, y, n); });
}

// Squared norm of an arbitrarily aligned vector.
OPUSPP_INLINE float inner_prod_self(const float* x, int n) noexcept {
   return with_misalignment(x, [&]<int R>() { return inner_prod<R, R>(x, x, n); });
}

// Cross-correlation of x against max_pitch shifts of y (celt_pitch_xcorr_c
// built on the vector xcorr kernel). x and xcorr are 16-byte aligned.
inline void pitch_xcorr(const float* x, const float* y, float* xcorr, int len, int max_pitch) noexcept {
   OPUSPP_ASSERT(max_pitch > 0 && misalignment(xcorr) == 0);
   with_misalignment(y, [&]<int R>() {
      int i = 0;
      for (; i < max_pitch - 3; i += 4) xcorr_kernel<R>(x, y + i, f32x4::zero(), len).store_aligned(xcorr + i);
      for (; i < max_pitch; i++) xcorr[i] = inner_prod(x, y + i, len);
   });
}

// Levinson-Durbin recursion (_celt_lpc), float version.
inline void celt_lpc(float* lpc, const float* ac, int p) noexcept {
   float error = ac[0];
   fill(lpc, 0.f, p);
   if (ac[0] > 1e-10f) {
      for (int i = 0; i < p; i++) {
         float rr = 0;
         OPUSPP_NO_VECTORIZE
         for (int j = 0; j < i; j++) rr += lpc[j] * ac[i - j];
         rr += ac[i + 1];
         const float r = -(rr / error);
         lpc[i] = r;
         OPUSPP_NO_VECTORIZE
         for (int j = 0; j < (i + 1) >> 1; j++) {
            const float tmp1 = lpc[j];
            const float tmp2 = lpc[i - 1 - j];
            lpc[j] = tmp1 + r * tmp2;
            lpc[i - 1 - j] = tmp2 + r * tmp1;
         }
         error = error - (r * r) * error;
         // Bail out once we get 30 dB gain.
         if (error <= .001f * ac[0]) break;
      }
   }
}

// FIR filter, y[i] = x[i] + sum_j num[j]*x[i-j-1] (celt_fir_c). x must have
// `Ord` samples of history before x[0]; y is 16-byte aligned. Not in-place.
template <int Ord>
inline void celt_fir(const float* x, const float* num, float* y, int n) noexcept {
   static_assert(Ord % 4 == 0);
   OPUSPP_ASSERT(misalignment(y) == 0);
   OPUSPP_ASSERT(misalignment(num) == 0);
   alignas(buffer_alignment) float rnum[Ord];
   // rnum[i] = num[Ord-1-i]
   for (int i = 0; i < Ord; i += 4) reverse(f32x4::load_aligned(num + Ord - 4 - i)).store_aligned(rnum + i);
   int i = 0;
   with_misalignment(x, [&]<int R>() {
      const float* xa = x - R;
      for (; i < n - 3; i += 4) {
         // x[i..i+3] from the aligned blocks around it.
         f32x4 init = f32x4::load_aligned(xa + i);
         if constexpr (R > 0) init = window<R>(init, f32x4::load_aligned(xa + i + 4));
         xcorr_kernel<R>(rnum, x + i - Ord, init, Ord).store_aligned(y + i);
      }
   });
   OPUSPP_NO_VECTORIZE
   for (; i < n; i++) {
      float sum = x[i];
      OPUSPP_NO_VECTORIZE
      for (int j = 0; j < Ord; j++) sum = sum + rnum[j] * x[i + j - Ord];
      y[i] = sum;
   }
}

// IIR filter, unrolled by four around the xcorr kernel (celt_iir). Works
// in-place; x and y_out are 16-byte aligned. Out of line so its 3 KB of
// scratch doesn't share the caller's stack frame (it runs once per channel
// per concealed frame).
template <int Ord>
OPUSPP_NOINLINE inline void celt_iir(const float* x, const float* den, float* y_out, int n, float* mem) noexcept {
   static_assert((Ord & 3) == 0);
   OPUSPP_ASSERT(misalignment(x) == 0 && misalignment(y_out) == 0);
   alignas(buffer_alignment) float rden[Ord];
   alignas(buffer_alignment) float y[frame_size + overlap + Ord];
   OPUSPP_ASSERT(n <= frame_size + overlap && misalignment(den) == 0 && misalignment(mem) == 0);
   // rden[i] = den[Ord-1-i], y[i] = -mem[Ord-1-i]
   for (int i = 0; i < Ord; i += 4) {
      reverse(f32x4::load_aligned(den + Ord - 4 - i)).store_aligned(rden + i);
      (-reverse(f32x4::load_aligned(mem + Ord - 4 - i))).store_aligned(y + i);
   }
   fill_aligned(y + Ord, 0.f, n);
   int i = 0;
   for (; i < n - 3; i += 4) {
      // Unroll by 4 as if it were an FIR filter, then patch up the result.
      const f32x4 sum = xcorr_kernel<0>(rden, y + i, f32x4::load_aligned(x + i), Ord);
      const float s0 = sum[0];
      const float s1 = sum[1] + (-s0) * den[0];
      const float s2 = (sum[2] + (-s1) * den[0]) + (-s0) * den[1];
      const float s3 = ((sum[3] + (-s2) * den[0]) + (-s1) * den[1]) + (-s0) * den[2];
      f32x4::set(-s0, -s1, -s2, -s3).store_aligned(y + i + Ord);
      f32x4::set(s0, s1, s2, s3).store_aligned(y_out + i);
   }
   OPUSPP_NO_VECTORIZE
   for (; i < n; i++) {
      float sum = x[i];
      OPUSPP_NO_VECTORIZE
      for (int j = 0; j < Ord; j++) sum -= rden[j] * y[i + j];
      y[i + Ord] = sum;
      y_out[i] = sum;
   }
   // mem[i] = y_out[n-1-i]
   OPUSPP_ASSERT(n % 4 == 0);
   for (i = 0; i < Ord; i += 4) reverse(f32x4::load_aligned(y_out + n - 4 - i)).store_aligned(mem + i);
}

// Autocorrelation with a symmetric window (_celt_autocorr), float. x, ac and
// the scratch xx (n floats, which libopus allocates itself) are 16-byte
// aligned; n is a multiple of 4, n <= max_period.
template <int Lag>
inline void celt_autocorr(const float* x, float* ac, const float* window, int ovl, int n,
                          float* OPUSPP_RESTRICT xx) noexcept {
   OPUSPP_ASSERT(n > 0 && n <= max_period && n % 4 == 0 && ovl % 4 == 0);
   OPUSPP_ASSERT(misalignment(xx) == 0);
   copy_aligned(xx, x, n);
   for (int i = 0; i < ovl; i += 4) {
      const f32x4 w = f32x4::load_aligned(window + i);
      (f32x4::load_aligned(x + i) * w).store_aligned(xx + i);
      // xx[n-i-1-k] = x[n-i-1-k] * window[i+k]
      const int j = n - 4 - i;
      (f32x4::load_aligned(x + j) * shuffle<3, 2, 1, 0>(w, w)).store_aligned(xx + j);
   }
   const int fast_n = n - Lag;
   pitch_xcorr(xx, xx, ac, fast_n, Lag + 1);
   for (int k = 0; k <= Lag; k++) {
      float d = 0;
      OPUSPP_NO_VECTORIZE
      for (int i = k + fast_n; i < n; i++) d = d + xx[i] * xx[i - k];
      ac[k] += d;
   }
}

// Downsamples the (stereo) decoder history by two with a whitening filter
// (pitch_downsample, factor 2).
inline void pitch_downsample(const float* const x[2], float* OPUSPP_RESTRICT x_lp, int len) noexcept {
   constexpr int factor = 2;
   constexpr int offset = factor / 2;
   alignas(buffer_alignment) float ac[8];
   float tmp = 1.f;
   float lpc[4];
   float lpc2[5];
   const float c1 = .8f;
   // x_lp[i] = .25*x[2i-1] + .25*x[2i+1] + .5*x[2i] per channel, channels summed.
   OPUSPP_ASSERT(len % 4 == 0 && misalignment(x_lp) == 0 && misalignment(x[0]) == 0 && misalignment(x[1]) == 0);
   for (int i = 1; i < 4; i++)
      x_lp[i] = .25f * x[0][factor * i - offset] + .25f * x[0][factor * i + offset] + .5f * x[0][factor * i];
   x_lp[0] = .25f * x[0][offset] + .5f * x[0][0];
   for (int i = 1; i < 4; i++)
      x_lp[i] += .25f * x[1][factor * i - offset] + .25f * x[1][factor * i + offset] + .5f * x[1][factor * i];
   x_lp[0] += .25f * x[1][offset] + .5f * x[1][0];
   {
      const f32x4 q = f32x4::broadcast(.25f), h = f32x4::broadcast(.5f);
      f32x4 prev0 = f32x4::load_aligned(x[0] + 4), prev1 = f32x4::load_aligned(x[1] + 4);
      for (int i = 4; i < len; i += 4) {
         // Blocks x[2i-4..2i-1], x[2i..2i+3], x[2i+4..2i+7] of each channel.
         auto lp = [&](const float* xc, f32x4& bm) {
            const f32x4 b0 = f32x4::load_aligned(xc + 2 * i);
            const f32x4 b1 = f32x4::load_aligned(xc + 2 * i + 4);
            const f32x4 even = shuffle<0, 2, 4, 6>(b0, b1);   // x[2i+2k]
            const f32x4 odd = shuffle<1, 3, 5, 7>(b0, b1);    // x[2i+2k+1]
            const f32x4 oddm1 = shuffle<3, 4, 5, 6>(bm, odd); // x[2i+2k-1]
            bm = b1;
            return (q * oddm1 + q * odd) + h * even;
         };
         const f32x4 ch0 = lp(x[0], prev0);
         const f32x4 ch1 = lp(x[1], prev1);
         (ch0 + ch1).store_aligned(x_lp + i);
      }
   }

   // Autocorrelation without window on the downsampled signal.
   {
      const int fast_n = len - 4;
      pitch_xcorr(x_lp, x_lp, ac, fast_n, 5);
      for (int k = 0; k <= 4; k++) {
         float d = 0;
         OPUSPP_NO_VECTORIZE
         for (int i = k + fast_n; i < len; i++) d = d + x_lp[i] * x_lp[i - k];
         ac[k] += d;
      }
   }
   // Noise floor -40 dB.
   ac[0] *= 1.0001f;
   // Lag windowing of ac[1..4] (ac[0] passes through unchanged).
   {
      const f32x4 a = f32x4::load_aligned(ac);
      const f32x4 k = f32x4::set(0.f, .008f * 1.f, .008f * 2.f, .008f * 3.f);
      shuffle<0, 5, 6, 7>(a, a - a * k * k).store_aligned(ac);
      ac[4] -= ac[4] * (.008f * 4.f) * (.008f * 4.f);
   }

   celt_lpc(lpc, ac, 4);
   for (int i = 0; i < 4; i++) {
      tmp = .9f * tmp;
      lpc[i] = lpc[i] * tmp;
   }
   // Add a zero.
   lpc2[0] = lpc[0] + .8f;
   lpc2[1] = lpc[1] + c1 * lpc[0];
   lpc2[2] = lpc[2] + c1 * lpc[1];
   lpc2[3] = lpc[3] + c1 * lpc[2];
   lpc2[4] = c1 * lpc[3];

   // celt_fir5, in place: x[i] + c0*x[i-1] + ... + c4*x[i-5] on the original
   // values (zero before the start). The two previous original blocks are
   // kept so the shifted windows can be built from aligned loads.
   {
      const f32x4 k0 = f32x4::broadcast(lpc2[0]), k1 = f32x4::broadcast(lpc2[1]);
      const f32x4 k2 = f32x4::broadcast(lpc2[2]), k3 = f32x4::broadcast(lpc2[3]);
      const f32x4 k4 = f32x4::broadcast(lpc2[4]);
      f32x4 prev2 = f32x4::zero(), prev1 = f32x4::zero();
      for (int i = 0; i < len; i += 4) {
         const f32x4 cur = f32x4::load_aligned(x_lp + i);
         f32x4 sum = cur;
         sum = sum + k0 * window<7>(prev2, prev1, cur);  // x[i-1..]
         sum = sum + k1 * window<6>(prev2, prev1, cur);  // x[i-2..]
         sum = sum + k2 * window<5>(prev2, prev1, cur);  // x[i-3..]
         sum = sum + k3 * window<4>(prev2, prev1, cur);  // x[i-4..]
         sum = sum + k4 * window<3>(prev2, prev1, cur);  // x[i-5..]
         sum.store_aligned(x_lp + i);
         prev2 = prev1;
         prev1 = cur;
      }
   }
}

inline void find_best_pitch(const float* xcorr, const float* y_, int len, int max_pitch, int* best_pitch) noexcept {
   const float* y = assume_aligned<16>(y_);
   float syy = 1;
   float best_num[2], best_den[2];
   best_num[0] = best_num[1] = -1;
   best_den[0] = best_den[1] = 0;
   best_pitch[0] = 0;
   best_pitch[1] = 1;
   OPUSPP_NO_VECTORIZE
   for (int j = 0; j < len; j++) syy = syy + y[j] * y[j];
   for (int i = 0; i < max_pitch; i++) {
      if (xcorr[i] > 0) {
         float xcorr16 = xcorr[i];
         // Avoids both underflow and overflow when squaring.
         xcorr16 *= 1e-12f;
         const float num = xcorr16 * xcorr16;
         if (num * best_den[1] > best_num[1] * syy) {
            if (num * best_den[0] > best_num[0] * syy) {
               best_num[1] = best_num[0];
               best_den[1] = best_den[0];
               best_pitch[1] = best_pitch[0];
               best_num[0] = num;
               best_den[0] = syy;
               best_pitch[0] = i;
            } else {
               best_num[1] = num;
               best_den[1] = syy;
               best_pitch[1] = i;
            }
         }
      }
      syy += y[i + len] * y[i + len] - y[i] * y[i];
      syy = max(1.f, syy);
   }
}

// Two-stage (4x then 2x decimated) open-loop pitch search (pitch_search).
template <int Len, int MaxPitch>
inline int pitch_search(const float* OPUSPP_RESTRICT x_lp, const float* OPUSPP_RESTRICT y) noexcept {
   constexpr int lag = Len + MaxPitch;
   // Sizes rounded up to whole vectors: block loads may touch the padding.
   alignas(buffer_alignment) float x_lp4[((Len >> 2) + 3) & ~3];
   alignas(buffer_alignment) float y_lp4[((lag >> 2) + 3) & ~3];
   alignas(buffer_alignment) float xcorr[((MaxPitch >> 1) + 3) & ~3];
   int best_pitch[2];
   best_pitch[0] = best_pitch[1] = 0;

   // Downsample by 2 again.
   OPUSPP_ASSERT(misalignment(x_lp) == 0 && misalignment(y) == 0);
   auto decimate = [](const float* src, float* dst, int n, int dst_size) {
      int j = 0;
      for (; j + 4 <= n; j += 4)
         shuffle<0, 2, 4, 6>(f32x4::load_aligned(src + 2 * j), f32x4::load_aligned(src + 2 * j + 4)).store_aligned(dst + j);
      for (; j < n; j++) dst[j] = src[2 * j];
      for (; j < dst_size; j++) dst[j] = 0;  // padding
   };
   decimate(x_lp, x_lp4, Len >> 2, int(sizeof(x_lp4) / sizeof(float)));
   decimate(y, y_lp4, lag >> 2, int(sizeof(y_lp4) / sizeof(float)));

   // Coarse search with 4x decimation.
   pitch_xcorr(x_lp4, y_lp4, xcorr, Len >> 2, MaxPitch >> 2);
   find_best_pitch(xcorr, y_lp4, Len >> 2, MaxPitch >> 2, best_pitch);

   // Finer search with 2x decimation.
   for (int i = 0; i < MaxPitch >> 1; i++) {
      xcorr[i] = 0;
      if (iabs(i - 2 * best_pitch[0]) > 2 && iabs(i - 2 * best_pitch[1]) > 2) continue;
      const float sum = inner_prod(x_lp, y + i, Len >> 1);
      xcorr[i] = max(-1.f, sum);
   }
   find_best_pitch(xcorr, y, Len >> 1, MaxPitch >> 1, best_pitch);

   // Refine by pseudo-interpolation.
   int offset = 0;
   if (best_pitch[0] > 0 && best_pitch[0] < (MaxPitch >> 1) - 1) {
      const float a = xcorr[best_pitch[0] - 1];
      const float b = xcorr[best_pitch[0]];
      const float c = xcorr[best_pitch[0] + 1];
      if ((c - a) > .7f * (b - a))
         offset = 1;
      else if ((a - c) > .7f * (b - c))
         offset = -1;
   }
   return 2 * best_pitch[0] - offset;
}

} // namespace opuspp::detail
