// Freestanding replacements for the libm functions used by the float CELT
// decoder (sqrt, exp, cos), plus the integer helpers from ecintrin.h/entcode.h.
//
// The reference float build evaluates celt_sqrt(), celt_exp2() and
// celt_cos_norm() through libm in double precision and rounds to float. The
// functions below reproduce that: sqrt is correctly rounded and exp/cos are
// accurate to about 1 ulp in double, so the float results agree with any
// conforming libm except in astronomically rare rounding ties.
//
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <bit>
#include <cstdint>

#include "config.hpp"

namespace opuspp::detail {

// Number of bits needed to represent v (EC_ILOG); 0 for v == 0.
constexpr OPUSPP_INLINE int ec_ilog(std::uint32_t v) noexcept { return std::bit_width(v); }

constexpr OPUSPP_INLINE std::uint32_t celt_udiv(std::uint32_t n, std::uint32_t d) noexcept {
   OPUSPP_ASSERT(d > 0);
   return n / d;
}

constexpr OPUSPP_INLINE std::int32_t celt_sudiv(std::int32_t n, std::int32_t d) noexcept {
   OPUSPP_ASSERT(d > 0);
   return n / d;
}

// Integer square root (celt/mathops.c: isqrt32).
constexpr inline unsigned isqrt32(std::uint32_t val) noexcept {
   unsigned g = 0;
   int bshift = (ec_ilog(val) - 1) >> 1;
   unsigned b = 1U << bshift;
   do {
      std::uint32_t t = ((std::uint32_t(g) << 1) + b) << bshift;
      if (t <= val) {
         g += b;
         val -= t;
      }
      b >>= 1;
      bshift--;
   } while (bshift >= 0);
   return g;
}

// Correctly rounded single precision square root, i.e. (float)sqrt((double)x).
// IEEE special values from their bit patterns (no diagnostics under
// -ffinite-math-only).
inline constexpr float float_nan = std::bit_cast<float>(0x7fc00000u);
inline constexpr float float_inf = std::bit_cast<float>(0x7f800000u);
inline constexpr double double_inf = std::bit_cast<double>(0x7ff0000000000000ull);

// NaN test on the bit pattern, reliable even with -ffast-math (celt_isnan with
// FLOAT_APPROX).
constexpr OPUSPP_INLINE bool is_nan(float x) noexcept {
   const std::uint32_t i = std::bit_cast<std::uint32_t>(x);
   return ((i >> 23) & 0xFF) == 0xFF && (i & 0x007FFFFF) != 0;
}

inline float sqrtf(float x) noexcept {
   if (!(x > 0.f)) return x == 0.f ? x : float_nan;
   if (x == float_inf) return x;
   const double xd = x;
   // Initial estimate from the exponent, refined with Newton iterations in double.
   std::uint64_t bits = std::bit_cast<std::uint64_t>(xd);
   double y = std::bit_cast<double>((bits >> 1) + (std::uint64_t(1023) << 51));
   for (int i = 0; i < 5; i++) y = 0.5 * (y + xd / y);
   float f = static_cast<float>(y);
   // Fix up the last bit: products of two floats (or midpoints of adjacent
   // floats) squared are exact in double, so these comparisons are exact.
   const std::uint32_t fb = std::bit_cast<std::uint32_t>(f);
   const float up = std::bit_cast<float>(fb + 1);
   const double mid_up = (double(f) + double(up)) * 0.5;
   if (mid_up * mid_up < xd) return up;
   const float dn = std::bit_cast<float>(fb - 1);
   const double mid_dn = (double(dn) + double(f)) * 0.5;
   if (mid_dn * mid_dn > xd) return dn;
   return f;
}

// Rounds to the nearest integer (ties away from zero) for |x| < 2^52.
constexpr OPUSPP_INLINE double round_nearest(double x) noexcept {
   return x >= 0 ? static_cast<double>(static_cast<std::int64_t>(x + 0.5))
                 : -static_cast<double>(static_cast<std::int64_t>(-x + 0.5));
}

// Table of 2^(j/32), j = 0..31, computed at compile time in extended precision.
struct Exp2Table {
   double v[32];
};

inline constexpr Exp2Table exp2_table = [] {
   Exp2Table t{};
   for (int j = 0; j < 32; j++) {
      // Taylor series of exp(j*ln2/32); converges to full precision in 30 terms.
      const long double r = (long double)j * 0.693147180559945309417232121458176568L / 32;
      long double term = 1, sum = 1;
      for (int i = 1; i < 30; i++) {
         term *= r / i;
         sum += term;
      }
      t.v[j] = static_cast<double>(sum);
   }
   return t;
}();

// exp() in double precision (within a few ulp), as used by celt_exp2().
inline double exp(double x) noexcept {
   if (!(x > -745.2)) return x != x ? x : 0.0;
   if (x > 709.78) return double_inf;
   // x = (32*m + j)*ln2/32 + r with |r| <= ln2/64; the high part of ln2/32
   // has 32 significant bits so kd*ln2_32_hi is exact.
   constexpr double inv_ln2_32 = 4.61662413084468283841e+01;
   constexpr double ln2_32_hi = 6.93147180369123816490e-01 / 32;
   constexpr double ln2_32_lo = 1.90821492927058770002e-10 / 32;
   const double kd = round_nearest(x * inv_ln2_32);
   const int k = static_cast<int>(kd);
   const double r = (x - kd * ln2_32_hi) - kd * ln2_32_lo;
   // exp(r) - 1 to degree 6 (truncation error < 2^-58 for |r| <= ln2/64).
   const double r2 = r * r;
   const double p = r + r2 * (0.5 + r * (1.0 / 6)) + (r2 * r2) * ((1.0 / 24) + r * (1.0 / 120) + r2 * (1.0 / 720));
   const double t = exp2_table.v[k & 31];
   const double e = t + t * p;
   // Scale by 2^m, in two steps so that subnormal results come out right.
   const int m = k >> 5;
   const int m1 = m / 2;
   const int m2 = m - m1;
   const double s1 = std::bit_cast<double>(std::uint64_t(1023 + m1) << 52);
   const double s2 = std::bit_cast<double>(std::uint64_t(1023 + m2) << 52);
   return e * s1 * s2;
}

// cos() in double precision, for the small arguments used here (|x| < 2^20).
inline double cos(double x) noexcept {
   constexpr double pio2_hi = 1.57079632673412561417e+00;
   constexpr double pio2_lo = 6.07710050650619224932e-11;
   constexpr double two_over_pi = 6.36619772367581382433e-01;
   const double nd = round_nearest(x * two_over_pi);
   const std::int64_t n = static_cast<std::int64_t>(nd);
   const double r = (x - nd * pio2_hi) - nd * pio2_lo;  // |r| <= pi/4
   const double r2 = r * r;
   // Taylor series of cos or sin on [-pi/4, pi/4] (error < 2^-58); only the
   // one needed for this quadrant is evaluated.
   if (n & 1) {
      double s = 1.0 / 1307674368000.0;
      s = -s * r2 + 1.0 / 6227020800.0;
      s = -s * r2 + 1.0 / 39916800.0;
      s = -s * r2 + 1.0 / 362880.0;
      s = -s * r2 + 1.0 / 5040.0;
      s = -s * r2 + 1.0 / 120.0;
      s = -s * r2 + 1.0 / 6.0;
      s = r - s * r2 * r;
      return (n & 2) ? s : -s;
   }
   double c = 1.0 / 20922789888000.0;
   c = -c * r2 + 1.0 / 87178291200.0;
   c = -c * r2 + 1.0 / 479001600.0;
   c = -c * r2 + 1.0 / 3628800.0;
   c = -c * r2 + 1.0 / 40320.0;
   c = -c * r2 + 1.0 / 720.0;
   c = -c * r2 + 1.0 / 24.0;
   c = -c * r2 + 0.5;
   c = 1.0 - c * r2;
   return (n & 2) ? -c : c;
}

// The float-build CELT math operators (celt/mathops.h without FLOAT_APPROX).

// Correctly rounded sqrt. Uses the hardware instruction where the compiler can
// emit it without a libm fallback (for errno):
// * Clang's elementwise builtin never sets errno.
// * GCC drops the fallback once the argument is known to be non-negative,
//   but only when optimising for speed: at -O0/-Os/-Oz it keeps a call to
//   sqrtf, and the optimize attribute/pragma doesn't change that. On x86 the
//   SSE scalar builtin is used there instead. It is the same correctly
//   rounded instruction and never touches errno.
// Otherwise, or for negative/NaN input, the software sqrtf().
inline float celt_sqrt(float x) noexcept {
#if defined(__clang__) && defined(__has_builtin)
#if __has_builtin(__builtin_elementwise_sqrt)
   return __builtin_elementwise_sqrt(x);
#else
   return sqrtf(x);
#endif
#elif defined(__GNUC__) && defined(__NO_MATH_ERRNO__)
   return __builtin_sqrtf(x);
#elif defined(__GNUC__) && defined(__OPTIMIZE__) && !defined(__OPTIMIZE_SIZE__)
   return x >= 0.f ? __builtin_sqrtf(x) : sqrtf(x);
#elif defined(__GNUC__) && defined(__SSE_MATH__) && (defined(__x86_64__) || defined(__i386__))
   typedef float v4sf __attribute__((vector_size(16)));
   const v4sf v = {x, 0.f, 0.f, 0.f};
   return __builtin_ia32_sqrtss(v)[0];
#else
   return sqrtf(x);
#endif
}
inline float celt_rsqrt(float x) noexcept { return 1.f / celt_sqrt(x); }
#ifndef OPUSPP_FLOAT_APPROX
inline float celt_exp2(float x) noexcept {
   return static_cast<float>(exp(0.6931471805599453094 * x));
}
#else
// 2^x as in libopus FLOAT_APPROX: 2^floor(x) through the exponent bits times
// a degree-5 Remez polynomial of the fraction.
inline float celt_exp2(float x) noexcept {
   // floor(x) < -50 exactly when x < -50.
   if (x < -50.f) return 0;
   int integer = static_cast<int>(x);
   if (static_cast<float>(integer) > x) integer--;  // floor
   const float frac = x - static_cast<float>(integer);
   float r = 9.999999403953552246093750000000e-01f +
             frac * (6.931530833244323730468750000000e-01f +
             frac * (2.401536107063293457031250000000e-01f +
             frac * (5.582631751894950866699218750000e-02f +
             frac * (8.989339694380760192871093750000e-03f +
             frac * (1.877576694823801517486572265625e-03f)))));
   const std::uint32_t bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(r)) +
                                                        static_cast<std::int32_t>(static_cast<std::uint32_t>(integer) << 23)) &
                              0x7fffffffu;
   return std::bit_cast<float>(bits);
}
#endif
inline float celt_cos_norm(float x) noexcept {
   constexpr double pi = 3.1415926535897931;
   return static_cast<float>(cos((.5f * pi) * x));
}

} // namespace opuspp::detail
