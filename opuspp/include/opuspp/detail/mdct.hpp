// Mixed-radix FFT and inverse MDCT (celt/kiss_fft.c, celt/mdct.c), float only.
//
// Only the transform sizes reachable with 10 ms frames are instantiated:
// the 480-point MDCT (FFT size 240, long blocks) and the 120-point MDCT
// (FFT size 60, four short blocks for transients). All stage parameters are
// compile-time constants, and the butterflies process two complex values per
// simd<float, 4> with per-stage twiddle tables laid out for aligned loads.
// Every lane evaluates exactly the scalar expressions of kiss_fft.c, so the
// output is bit-identical to the reference.
//
// Copyright (c) 2003-2004 Mark Borgerding, 2005-2007 Xiph.Org Foundation,
// 2008 Gregory Maxwell, 2007-2008 CSIRO.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "celt_tables.hpp"
#include "config.hpp"
#include "simd.hpp"

namespace opuspp::detail {

// ---------------------------------------------------------------------------
// Compile-time twiddle tables
// ---------------------------------------------------------------------------

// Twiddles tw[k*j*FS] for k = 1..K and j = 0..M-1, duplicated per complex lane:
// re[k-1] = {r(j0), r(j0), r(j1), r(j1), ...}, im[k-1] likewise.
template <int K, int M, int FS>
struct StageTwiddles {
   alignas(buffer_alignment) float re[K][2 * M];
   alignas(buffer_alignment) float im[K][2 * M];
};

template <int K, int M, int FS>
inline constexpr StageTwiddles<K, M, FS> stage_twiddles = [] {
   StageTwiddles<K, M, FS> t{};
   for (int k = 1; k <= K; k++)
      for (int j = 0; j < M; j++) {
         const tables::Complex c = tables::fft_twiddles[k * j * FS];
         t.re[k - 1][2 * j] = t.re[k - 1][2 * j + 1] = c.r;
         t.im[k - 1][2 * j] = t.im[k - 1][2 * j + 1] = c.i;
      }
   return t;
}();

// ---------------------------------------------------------------------------
// Butterflies. `f` points to interleaved complex data, 16-byte aligned; a
// vector holds the two complex values at an even index.
// ---------------------------------------------------------------------------

OPUSPP_INLINE f32x4 cload(const float* f, int q) noexcept { return f32x4::load_aligned(f + 2 * q); }
OPUSPP_INLINE void cstore(float* f, int q, f32x4 v) noexcept { v.store_aligned(f + 2 * q); }

// Radix-4 first stage with all twiddles equal to one (kf_bfly4, m == 1):
// N groups of four consecutive complex values.
template <int N>
OPUSPP_INLINE void kf_bfly4_m1(float* f) noexcept {
   for (int i = 0; i < N; i++, f += 8) {
      const f32x4 v0 = f32x4::load_aligned(f);      // {F0, F1}
      const f32x4 v1 = f32x4::load_aligned(f + 4);  // {F2, F3}
      const f32x4 a = v0 + v1;                      // {F0+F2, F1+F3} = {F0', scratch1}
      const f32x4 d = v0 - v1;                      // {F0-F2, F1-F3} = {scratch0, scratch1'}
      const f32x4 p = shuffle<0, 1, 4, 5>(a, d);             // {F0', scratch0}
      const f32x4 q = shuffle<2, 3, 6, 7>(a, mul_neg_i(d));  // {scratch1, -i*scratch1'}
      (p + q).store_aligned(f);                     // {F0 = F0'+s1, F1 = s0 + (s1'.i, -s1'.r)}
      (p - q).store_aligned(f + 4);                 // {F2 = F0'-s1, F3 = s0 - (s1'.i, -s1'.r)}
   }
}

template <int FS, int M, int N, int MM>
OPUSPP_INLINE void kf_bfly4(float* f0) noexcept {
   static_assert(M % 2 == 0 && (N == 1 || MM % 2 == 0));
   constexpr const auto& tw = stage_twiddles<3, M, FS>;
   for (int i = 0; i < N; i++) {
      float* f = f0 + 2 * i * MM;
      for (int j = 0; j < M; j += 2) {
         const f32x4 s0 = cmul(cload(f, M + j), f32x4::load_aligned(&tw.re[0][2 * j]),
                               f32x4::load_aligned(&tw.im[0][2 * j]));
         const f32x4 s1 = cmul(cload(f, 2 * M + j), f32x4::load_aligned(&tw.re[1][2 * j]),
                               f32x4::load_aligned(&tw.im[1][2 * j]));
         const f32x4 s2 = cmul(cload(f, 3 * M + j), f32x4::load_aligned(&tw.re[2][2 * j]),
                               f32x4::load_aligned(&tw.im[2][2 * j]));
         f32x4 x = cload(f, j);
         const f32x4 s5 = x - s1;
         x = x + s1;
         const f32x4 s3 = s0 + s2;
         const f32x4 s4 = s0 - s2;
         cstore(f, 2 * M + j, x - s3);
         cstore(f, j, x + s3);
         // Fout[m] = {s5.r + s4.i, s5.i - s4.r}, Fout[m3] = {s5.r - s4.i, s5.i + s4.r}
         cstore(f, M + j, s5 + mul_neg_i(s4));
         cstore(f, 3 * M + j, s5 - mul_neg_i(s4));
      }
   }
}

template <int FS, int M, int N, int MM>
OPUSPP_INLINE void kf_bfly3(float* f0) noexcept {
   static_assert(M % 2 == 0 && (N == 1 || MM % 2 == 0));
   constexpr const auto& tw = stage_twiddles<2, M, FS>;
   const f32x4 epi3_i = f32x4::broadcast(tables::fft_twiddles[FS * M].i);
   const f32x4 half = f32x4::broadcast(.5f);
   for (int i = 0; i < N; i++) {
      float* f = f0 + 2 * i * MM;
      for (int j = 0; j < M; j += 2) {
         const f32x4 s1 = cmul(cload(f, M + j), f32x4::load_aligned(&tw.re[0][2 * j]),
                               f32x4::load_aligned(&tw.im[0][2 * j]));
         const f32x4 s2 = cmul(cload(f, 2 * M + j), f32x4::load_aligned(&tw.re[1][2 * j]),
                               f32x4::load_aligned(&tw.im[1][2 * j]));
         const f32x4 s3 = s1 + s2;
         const f32x4 s0 = (s1 - s2) * epi3_i;
         const f32x4 x = cload(f, j);
         const f32x4 fm = x - s3 * half;
         cstore(f, j, x + s3);
         // Fout[m2] = {Fm.r + s0.i, Fm.i - s0.r}, Fout[m] = {Fm.r - s0.i, Fm.i + s0.r}
         cstore(f, 2 * M + j, fm + mul_neg_i(s0));
         cstore(f, M + j, fm - mul_neg_i(s0));
      }
   }
}

template <int FS, int M, int N, int MM>
OPUSPP_INLINE void kf_bfly5(float* f0) noexcept {
   static_assert(M % 2 == 0 && (N == 1 || MM % 2 == 0));
   constexpr const auto& tw = stage_twiddles<4, M, FS>;
   constexpr tables::Complex ya = tables::fft_twiddles[FS * M];
   constexpr tables::Complex yb = tables::fft_twiddles[FS * 2 * M];
   const f32x4 yar = f32x4::broadcast(ya.r), yai = f32x4::broadcast(ya.i);
   const f32x4 ybr = f32x4::broadcast(yb.r), ybi = f32x4::broadcast(yb.i);
   const f32x4 conj = f32x4::set(1.f, -1.f, 1.f, -1.f);
   for (int i = 0; i < N; i++) {
      float* f = f0 + 2 * i * MM;
      for (int u = 0; u < M; u += 2) {
         const f32x4 s0 = cload(f, u);
         const f32x4 s1 = cmul(cload(f, M + u), f32x4::load_aligned(&tw.re[0][2 * u]),
                               f32x4::load_aligned(&tw.im[0][2 * u]));
         const f32x4 s2 = cmul(cload(f, 2 * M + u), f32x4::load_aligned(&tw.re[1][2 * u]),
                               f32x4::load_aligned(&tw.im[1][2 * u]));
         const f32x4 s3 = cmul(cload(f, 3 * M + u), f32x4::load_aligned(&tw.re[2][2 * u]),
                               f32x4::load_aligned(&tw.im[2][2 * u]));
         const f32x4 s4 = cmul(cload(f, 4 * M + u), f32x4::load_aligned(&tw.re[3][2 * u]),
                               f32x4::load_aligned(&tw.im[3][2 * u]));
         const f32x4 s7 = s1 + s4;
         const f32x4 s10 = s1 - s4;
         const f32x4 s8 = s2 + s3;
         const f32x4 s9 = s2 - s3;
         cstore(f, u, s0 + (s7 + s8));
         const f32x4 s5 = s0 + (s7 * yar + s8 * ybr);
         // s6 = {s10.i*ya.i + s9.i*yb.i, -(s10.r*ya.i + s9.r*yb.i)}
         const f32x4 s6 = (swap_pairs(s10) * yai + swap_pairs(s9) * ybi) * conj;
         cstore(f, M + u, s5 - s6);
         cstore(f, 4 * M + u, s5 + s6);
         const f32x4 s11 = s0 + (s7 * ybr + s8 * yar);
         // s12 = {s9.i*ya.i - s10.i*yb.i, s10.r*yb.i - s9.r*ya.i}; -(a-b) == b-a exactly.
         const f32x4 s12 = (swap_pairs(s9) * yai - swap_pairs(s10) * ybi) * conj;
         cstore(f, 2 * M + u, s11 + s12);
         cstore(f, 3 * M + u, s11 - s12);
      }
   }
}

// FFT of the 240- and 60-point transforms on bit-reversed input (opus_fft_impl
// with the stage sequence of the static mode unrolled at compile time; the
// twiddle stride is fstride << shift).
OPUSPP_INLINE void fft240(float* f) noexcept {
   kf_bfly4_m1<60>(f);
   kf_bfly4<15 << 1, 4, 15, 16>(f);
   kf_bfly3<5 << 1, 16, 5, 48>(f);
   kf_bfly5<1 << 1, 48, 1, 1>(f);
}

OPUSPP_INLINE void fft60(float* f) noexcept {
   kf_bfly4_m1<15>(f);
   kf_bfly3<5 << 3, 4, 5, 12>(f);
   kf_bfly5<1 << 3, 12, 1, 1>(f);
}

// ---------------------------------------------------------------------------
// Inverse MDCT
// ---------------------------------------------------------------------------

// Post-rotation twiddles in lane order: element c uses t0 = trig[c] and
// t1 = trig[N4 + c]; p = {t0, -t0}, q = {t1, t1} per complex lane.
template <int Shift>
struct PostTwiddles {
   static constexpr int n = 1920 >> Shift;
   static constexpr int n4 = n >> 2;
   alignas(buffer_alignment) float p[2 * n4];
   alignas(buffer_alignment) float q[2 * n4];
};

template <int Shift>
inline constexpr PostTwiddles<Shift> post_twiddles = [] {
   PostTwiddles<Shift> t{};
   int offset = 0;
   for (int i = 0, n = 1920; i < Shift; i++) {
      n >>= 1;
      offset += n;
   }
   const float* trig = tables::mdct_twiddles960 + offset;
   for (int c = 0; c < t.n4; c++) {
      t.p[2 * c] = trig[c];
      t.p[2 * c + 1] = -trig[c];
      t.q[2 * c] = t.q[2 * c + 1] = trig[t.n4 + c];
   }
   return t;
}();

// Inverse MDCT with windowed overlap (clt_mdct_backward). Shift selects the
// transform size: 1 for the long 480-coefficient block, 3 for 120-coefficient
// short blocks. `stride` is the interleave of the input coefficients. `out`
// must be 16-byte aligned.
template <int Shift>
inline void mdct_backward(const float* in, float* OPUSPP_RESTRICT out_, int stride) noexcept {
   static_assert(Shift == 1 || Shift == 3);
   constexpr int N = 1920 >> Shift;
   constexpr int N2 = N >> 1;
   constexpr int N4 = N >> 2;
   constexpr int trig_offset = Shift == 1 ? 960 : 960 + 480 + 240;
   const float* trig = tables::mdct_twiddles960 + trig_offset;
   const std::int16_t* bitrev = Shift == 1 ? tables::fft_bitrev240 : tables::fft_bitrev60;
   float* OPUSPP_RESTRICT out = assume_aligned<16>(out_);
   float* OPUSPP_RESTRICT fft_buf = out + (overlap >> 1);

   // Pre-rotate, storing directly in bit-reversed order (a gather/scatter).
   {
      const float* OPUSPP_RESTRICT xp1 = in;
      const float* OPUSPP_RESTRICT xp2 = in + stride * (N2 - 1);
      for (int i = 0; i < N4; i++) {
         const int rev = bitrev[i];
         const float x1 = *xp1;
         const float x2 = *xp2;
         const float yr = x2 * trig[i] + x1 * trig[N4 + i];
         const float yi = x1 * trig[i] - x2 * trig[N4 + i];
         // Real and imaginary parts swap because an FFT is used for an IFFT.
         fft_buf[2 * rev + 1] = yr;
         fft_buf[2 * rev] = yi;
         xp1 += 2 * stride;
         xp2 -= 2 * stride;
      }
   }

   if constexpr (Shift == 1)
      fft240(fft_buf);
   else
      fft60(fft_buf);

   // Post-rotate and de-shuffle from both ends of the buffer at once. Element
   // c (stored as {im, re}) becomes yr = re*t0 + im*t1, yi = re*t1 - im*t0;
   // yr stays in place and yi moves to the mirrored element N4-1-c.
   {
      constexpr const auto& tw = post_twiddles<Shift>;
      constexpr int pairs = (N4 + 1) >> 1;  // mirrored element pairs
      int c = 0;
      for (; c + 1 < pairs; c += 2) {
         const int b = N4 - 2 - c;  // back vector holds elements {b, b+1}
         const f32x4 vf = cload(fft_buf, c);
         const f32x4 vb = cload(fft_buf, b);
         // Lane pairs {yr, yi}: swap_pairs(v)*p + v*q gives {re*t0 + im*t1, -(im*t0) + re*t1}.
         const f32x4 rf = swap_pairs(vf) * f32x4::load_aligned(tw.p + 2 * c) + vf * f32x4::load_aligned(tw.q + 2 * c);
         const f32x4 rb = swap_pairs(vb) * f32x4::load_aligned(tw.p + 2 * b) + vb * f32x4::load_aligned(tw.q + 2 * b);
         // Front c pairs with back b+1, front c+1 with back b.
         cstore(fft_buf, c, shuffle<0, 7, 2, 5>(rf, rb));
         cstore(fft_buf, b, shuffle<4, 3, 6, 1>(rf, rb));
      }
      // Middle element pair when the pair count is odd.
      for (; c < pairs; c++) {
         float* yp0 = fft_buf + 2 * c;
         float* yp1 = fft_buf + 2 * (N4 - 1 - c);
         float re = yp0[1];
         float im = yp0[0];
         float t0 = trig[c];
         float t1 = trig[N4 + c];
         const float yr0 = re * t0 + im * t1;
         const float yi0 = re * t1 - im * t0;
         re = yp1[1];
         im = yp1[0];
         yp0[0] = yr0;
         yp1[1] = yi0;
         t0 = trig[N4 - c - 1];
         t1 = trig[N2 - c - 1];
         yp1[0] = re * t0 + im * t1;
         yp0[1] = re * t1 - im * t0;
      }
   }

   // Mirror on both sides for TDAC: out[i] and out[overlap-1-i] are combined
   // with window[i] and window[overlap-1-i].
   {
      const float* window = tables::window120;
      for (int i = 0; i < overlap / 2; i += 4) {
         const int j = overlap - 4 - i;
         const f32x4 x2 = f32x4::load_aligned(out + i);
         const f32x4 x1 = reverse(f32x4::load_aligned(out + j));
         const f32x4 w1 = f32x4::load_aligned(window + i);
         const f32x4 w2 = reverse(f32x4::load_aligned(window + j));
         (x2 * w2 - x1 * w1).store_aligned(out + i);
         reverse(x2 * w1 + x1 * w2).store_aligned(out + j);
      }
   }
}

} // namespace opuspp::detail
