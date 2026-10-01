// opuspp - freestanding, header-only C++23 port of the libopus 1.6.1 decoder,
// restricted to 48 kHz stereo output, CELT-only 10 ms packets.
//
// Copyright (c) 2007-2008 CSIRO, 2007-2011 Xiph.Org Foundation, 2008 Gregory Maxwell.
// Written by Jean-Marc Valin and Timothy B. Terriberry (original C code).
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstddef>
#include <cstdint>

// Define OPUSPP_FLOAT_APPROX to use the same float approximations as a
// libopus build with FLOAT_APPROX (celt_exp2), matching that build instead of
// the default one. Like libopus, -ffast-math is only allowed together with
// it: the NaN checks in the concealment then use bit tests.
#if defined(__FAST_MATH__) && !defined(OPUSPP_FLOAT_APPROX)
#error "opuspp can only be compiled with -ffast-math when OPUSPP_FLOAT_APPROX is defined"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define OPUSPP_INLINE inline __attribute__((always_inline))
#define OPUSPP_NOINLINE __attribute__((noinline))
#define OPUSPP_RESTRICT __restrict__
#define OPUSPP_LIKELY(x) __builtin_expect(!!(x), 1)
#elif defined(_MSC_VER)
#define OPUSPP_INLINE __forceinline
#define OPUSPP_NOINLINE __declspec(noinline)
#define OPUSPP_RESTRICT __restrict
#define OPUSPP_LIKELY(x) (x)
#else
#define OPUSPP_INLINE inline
#define OPUSPP_NOINLINE
#define OPUSPP_RESTRICT
#define OPUSPP_LIKELY(x) (x)
#endif

// Keeps the following loop scalar. Used for short in-order float reductions
// whose operands have arbitrary alignment, where a vectorised version would
// only vectorise the loads/multiplies and need unaligned accesses.
// Unrolling is disabled too, so the basic-block (SLP) vectoriser cannot pack
// the unrolled iterations into unaligned vector accesses either.
#if defined(__clang__)
#define OPUSPP_NO_VECTORIZE _Pragma("clang loop vectorize(disable) interleave(disable) unroll(disable)")
#elif defined(__GNUC__) && __GNUC__ >= 14
#define OPUSPP_NO_VECTORIZE _Pragma("GCC novector") _Pragma("GCC unroll 1")
#else
#define OPUSPP_NO_VECTORIZE
#endif

// Assertions are compiled out unless OPUSPP_ENABLE_ASSERTIONS is defined. The
// handler must be provided by the user in that case (there is no abort() in a
// freestanding environment).
#ifdef OPUSPP_ENABLE_ASSERTIONS
namespace opuspp {
[[noreturn]] void assertion_failed(const char* expr, const char* file, int line) noexcept;
}
#define OPUSPP_ASSERT(cond) \
   ((cond) ? (void)0 : ::opuspp::assertion_failed(#cond, __FILE__, __LINE__))
#else
#define OPUSPP_ASSERT(cond) ((void)0)
#endif

namespace opuspp::detail {

// Alignment used for every buffer that is processed with simd<float, 4>.
inline constexpr std::size_t simd_alignment = 16;
// Alignment of every buffer and table: one cache line. Buffer offsets used
// with vector loads are multiples of 16 bytes; aligning the bases to 64 bytes
// also lets compilers that merge neighbouring 16-byte accesses into 32/64-byte
// ones (AVX, AVX-512) keep using aligned instructions.
inline constexpr std::size_t buffer_alignment = 64;

// Fixed stream parameters. Everything below is hardcoded for this configuration.
inline constexpr int channels = 2;          // output channels, also CELT decoder channels
inline constexpr int frame_size = 480;      // 10 ms at 48 kHz
inline constexpr int lm = 2;                // log2(frame_size / short_mdct_size)
inline constexpr int nb_short_mdcts = 1 << lm;
inline constexpr int short_mdct_size = 120;
inline constexpr int overlap = 120;
inline constexpr int nb_ebands = 21;
// Per-channel stride of band-energy arrays: 21 bands padded to whole vectors,
// so each channel starts 16-byte aligned.
inline constexpr int band_stride = 24;
inline constexpr int max_lm = 3;
inline constexpr int max_period = 1024;
inline constexpr int decode_buffer_size = 2048;
inline constexpr int lpc_order = 24;
inline constexpr int bitres = 3;
inline constexpr int max_fine_bits = 8;
inline constexpr int fine_offset = 21;
inline constexpr float preemph = 0.85000610f;
inline constexpr float very_small = 1e-30f;
inline constexpr float epsilon = 1e-15f;
inline constexpr float sig_scale = 32768.f;

inline constexpr int spread_none = 0;
inline constexpr int spread_light = 1;
inline constexpr int spread_normal = 2;
inline constexpr int spread_aggressive = 3;

// Alignment of the decoder output (one cache line).
inline constexpr std::size_t output_alignment = 64;

template <std::size_t Align, class T>
OPUSPP_INLINE T* assume_aligned(T* p) noexcept {
   OPUSPP_ASSERT(reinterpret_cast<std::uintptr_t>(p) % Align == 0);
#if defined(__GNUC__) || defined(__clang__)
   return static_cast<T*>(__builtin_assume_aligned(p, Align));
#else
   return p;
#endif
}

template <class T>
constexpr OPUSPP_INLINE T min(T a, T b) noexcept { return a < b ? a : b; }
template <class T>
constexpr OPUSPP_INLINE T max(T a, T b) noexcept { return a > b ? a : b; }
constexpr OPUSPP_INLINE int iabs(int a) noexcept { return a < 0 ? -a : a; }
constexpr OPUSPP_INLINE float fabs(float a) noexcept { return a < 0 ? -a : a; }

template <class T>
constexpr OPUSPP_INLINE void copy(T* OPUSPP_RESTRICT dst, const T* OPUSPP_RESTRICT src, int n) noexcept {
   OPUSPP_NO_VECTORIZE
   for (int i = 0; i < n; i++) dst[i] = src[i];
}

// Overlapping copy (memmove semantics).
template <class T>
constexpr OPUSPP_INLINE void move(T* dst, const T* src, int n) noexcept {
   if (dst < src) {
      OPUSPP_NO_VECTORIZE
      for (int i = 0; i < n; i++) dst[i] = src[i];
   } else if (dst > src) {
      OPUSPP_NO_VECTORIZE
      for (int i = n - 1; i >= 0; i--) dst[i] = src[i];
   }
}

template <class T>
constexpr OPUSPP_INLINE void fill(T* dst, T value, int n) noexcept {
   OPUSPP_NO_VECTORIZE
   for (int i = 0; i < n; i++) dst[i] = value;
}

} // namespace opuspp::detail
