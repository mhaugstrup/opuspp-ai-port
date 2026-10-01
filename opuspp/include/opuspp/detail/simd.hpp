// Minimal portable fixed-width SIMD value type.
//
// The libopus SIMD kernels (SSE/NEON intrinsics) are expressed on top of this
// type. On GCC and Clang the storage is a generic vector-extension type that the
// compiler lowers to whatever the target ISA provides (SSE, NEON, AltiVec, RVV,
// WASM SIMD, or scalar code). Other compilers use a plain array with loops,
// which the auto-vectoriser usually picks up. Either way the arithmetic per lane
// is identical, so results do not depend on which backend is used.
//
// Memory access comes in two flavours: load()/store() for addresses that are
// only element aligned (sliding windows), and load_aligned()/store_aligned()
// for addresses aligned to the vector size, which compile to alignment-expecting
// instructions (movaps/movdqa, or aligned memory operands).
//
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <bit>
#include <cstdint>

#include "config.hpp"

namespace opuspp::detail {

// Define OPUSPP_NO_VECTOR_EXT to force the plain-array backend.
#if (defined(__GNUC__) || defined(__clang__)) && !defined(OPUSPP_NO_VECTOR_EXT)
#define OPUSPP_VECTOR_EXT 1
#endif

template <class T, int N>
struct alignas(sizeof(T) * N) simd {
   static_assert(N > 0 && (N & (N - 1)) == 0, "lane count must be a power of two");

#ifdef OPUSPP_VECTOR_EXT
   typedef T native_type __attribute__((vector_size(sizeof(T) * N)));
   native_type v;
#else
   T v[N];
#endif

   using value_type = T;
   static constexpr int size = N;
   static constexpr std::size_t alignment = sizeof(T) * N;

   // Unaligned load (element alignment only).
   static OPUSPP_INLINE simd load(const T* p) noexcept {
      simd r;
#ifdef OPUSPP_VECTOR_EXT
      __builtin_memcpy(&r.v, p, sizeof(r.v));
#else
      for (int i = 0; i < N; i++) r.v[i] = p[i];
#endif
      return r;
   }

   // Load from memory aligned to sizeof(simd).
   static OPUSPP_INLINE simd load_aligned(const T* p) noexcept {
#ifdef OPUSPP_VECTOR_EXT
      simd r;
      r.v = *reinterpret_cast<const native_type*>(assume_aligned<alignment>(p));
      return r;
#else
      return load(assume_aligned<alignment>(p));
#endif
   }

   static OPUSPP_INLINE simd broadcast(T x) noexcept {
      simd r;
#ifdef OPUSPP_VECTOR_EXT
      r.v = native_type{} + x;
#else
      for (int i = 0; i < N; i++) r.v[i] = x;
#endif
      return r;
   }

   static OPUSPP_INLINE simd zero() noexcept { return broadcast(T(0)); }

   // Lanes {x0, x1, ...} (N arguments).
   template <class... U>
   static OPUSPP_INLINE simd set(U... x) noexcept {
      static_assert(sizeof...(U) == N);
      simd r;
#ifdef OPUSPP_VECTOR_EXT
      r.v = native_type{static_cast<T>(x)...};
#else
      const T t[N] = {static_cast<T>(x)...};
      for (int i = 0; i < N; i++) r.v[i] = t[i];
#endif
      return r;
   }

   // Unaligned store.
   OPUSPP_INLINE void store(T* p) const noexcept {
#ifdef OPUSPP_VECTOR_EXT
      __builtin_memcpy(p, &v, sizeof(v));
#else
      for (int i = 0; i < N; i++) p[i] = v[i];
#endif
   }

   // Store to memory aligned to sizeof(simd).
   OPUSPP_INLINE void store_aligned(T* p) const noexcept {
#ifdef OPUSPP_VECTOR_EXT
      *reinterpret_cast<native_type*>(assume_aligned<alignment>(p)) = v;
#else
      store(assume_aligned<alignment>(p));
#endif
   }

   OPUSPP_INLINE T operator[](int i) const noexcept { return v[i]; }

#ifdef OPUSPP_VECTOR_EXT
#define OPUSPP_SIMD_BINOP(op)                                               \
   friend OPUSPP_INLINE simd operator op(simd a, simd b) noexcept {         \
      a.v = a.v op b.v;                                                     \
      return a;                                                             \
   }
#else
#define OPUSPP_SIMD_BINOP(op)                                               \
   friend OPUSPP_INLINE simd operator op(simd a, simd b) noexcept {         \
      for (int i = 0; i < N; i++) a.v[i] = a.v[i] op b.v[i];                \
      return a;                                                             \
   }
#endif
   OPUSPP_SIMD_BINOP(+)
   OPUSPP_SIMD_BINOP(-)
   OPUSPP_SIMD_BINOP(*)
   OPUSPP_SIMD_BINOP(&)
   OPUSPP_SIMD_BINOP(|)
   OPUSPP_SIMD_BINOP(^)
#undef OPUSPP_SIMD_BINOP

   friend OPUSPP_INLINE simd operator>>(simd a, int s) noexcept {
#ifdef OPUSPP_VECTOR_EXT
      a.v = a.v >> s;
#else
      for (int i = 0; i < N; i++) a.v[i] = a.v[i] >> s;
#endif
      return a;
   }

   friend OPUSPP_INLINE simd operator<<(simd a, int s) noexcept {
#ifdef OPUSPP_VECTOR_EXT
      a.v = a.v << s;
#else
      for (int i = 0; i < N; i++) a.v[i] = a.v[i] << s;
#endif
      return a;
   }

   // Exact negation, including the sign of zero (unlike 0 - a).
   friend OPUSPP_INLINE simd operator-(simd a) noexcept { return a * broadcast(T(-1)); }

   OPUSPP_INLINE simd& operator+=(simd b) noexcept { return *this = *this + b; }

   // Horizontal sum using the pairwise order of the SSE/NEON kernels:
   // (x0 + x2) + (x1 + x3) for four lanes, halves folded first in general.
   OPUSPP_INLINE T hsum() const noexcept {
      alignas(alignment) T t[N];
      store_aligned(t);
      for (int w = N / 2; w >= 1; w /= 2)
         for (int i = 0; i < w; i++) t[i] = t[i] + t[i + w];
      return t[0];
   }
};

// Lane permutation of the concatenation {a, b}: lane k of the result is lane
// I_k of a (I_k < N) or lane I_k - N of b.
template <int... I, class T, int N>
OPUSPP_INLINE simd<T, N> shuffle(simd<T, N> a, simd<T, N> b) noexcept {
   static_assert(sizeof...(I) == N);
   simd<T, N> r;
#ifdef OPUSPP_VECTOR_EXT
   r.v = __builtin_shufflevector(a.v, b.v, I...);
#else
   const int idx[N] = {I...};
   for (int k = 0; k < N; k++) r.v[k] = idx[k] < N ? a.v[idx[k]] : b.v[idx[k] - N];
#endif
   return r;
}

// Reinterprets the bits of each lane (same lane count and width).
template <class To, class From, int N>
OPUSPP_INLINE simd<To, N> bit_cast(simd<From, N> a) noexcept {
   static_assert(sizeof(To) == sizeof(From));
   simd<To, N> r;
#ifdef OPUSPP_VECTOR_EXT
   r.v = std::bit_cast<typename simd<To, N>::native_type>(a.v);
#else
   for (int i = 0; i < N; i++) r.v[i] = std::bit_cast<To>(a.v[i]);
#endif
   return r;
}

// Value conversion of each lane (e.g. int32 -> float).
template <class To, class From, int N>
OPUSPP_INLINE simd<To, N> convert(simd<From, N> a) noexcept {
   simd<To, N> r;
#ifdef OPUSPP_VECTOR_EXT
   r.v = __builtin_convertvector(a.v, typename simd<To, N>::native_type);
#else
   for (int i = 0; i < N; i++) r.v[i] = static_cast<To>(a.v[i]);
#endif
   return r;
}

using f32x4 = simd<float, 4>;
using i32x4 = simd<std::int32_t, 4>;
using u32x4 = simd<std::uint32_t, 4>;

// Lane-wise a > b ? a : b and a < b ? a : b, exactly like the scalar
// MAX32/MIN32 macros (the second operand wins for unordered/NaN input).
OPUSPP_INLINE f32x4 vmax(f32x4 a, f32x4 b) noexcept {
#ifdef OPUSPP_VECTOR_EXT
   const auto m = a.v > b.v;
   u32x4 mask;
   mask.v = std::bit_cast<u32x4::native_type>(m);
   return bit_cast<float>((bit_cast<std::uint32_t>(a) & mask) | (bit_cast<std::uint32_t>(b) & (mask ^ u32x4::broadcast(~0u))));
#else
   f32x4 r;
   for (int i = 0; i < 4; i++) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i];
   return r;
#endif
}

OPUSPP_INLINE f32x4 vmin(f32x4 a, f32x4 b) noexcept {
#ifdef OPUSPP_VECTOR_EXT
   const auto m = a.v < b.v;
   u32x4 mask;
   mask.v = std::bit_cast<u32x4::native_type>(m);
   return bit_cast<float>((bit_cast<std::uint32_t>(a) & mask) | (bit_cast<std::uint32_t>(b) & (mask ^ u32x4::broadcast(~0u))));
#else
   f32x4 r;
   for (int i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i];
   return r;
#endif
}

// Reverses the four lanes.
OPUSPP_INLINE f32x4 reverse(f32x4 a) noexcept { return shuffle<3, 2, 1, 0>(a, a); }

// ---------------------------------------------------------------------------
// Aligned access to misaligned data.
//
// A pointer p into a 16-byte aligned array has a misalignment R = 0..3 floats.
// Instead of unaligned loads, windows p[k..k+3] are assembled from the aligned
// blocks around them with compile-time shuffles (like NEON vext / SSSE3
// palignr). Only the aligned blocks that contain requested elements are
// loaded, so nothing outside the array is touched.
// ---------------------------------------------------------------------------

// Lanes S..S+3 of the concatenation {a, b}, 0 <= S <= 4.
template <int S>
OPUSPP_INLINE f32x4 window(f32x4 a, f32x4 b) noexcept {
   static_assert(S >= 0 && S <= 4);
   if constexpr (S == 0)
      return a;
   else if constexpr (S == 4)
      return b;
   else
      return shuffle<S, S + 1, S + 2, S + 3>(a, b);
}

// Lanes S..S+3 of the concatenation {a, b, c}, 0 <= S <= 8.
template <int S>
OPUSPP_INLINE f32x4 window(f32x4 a, f32x4 b, f32x4 c) noexcept {
   if constexpr (S <= 4)
      return window<S>(a, b);
   else
      return window<S - 4>(b, c);
}

// Misalignment of p in floats (0..3), relative to 16 bytes.
OPUSPP_INLINE int misalignment(const float* p) noexcept {
   return static_cast<int>((reinterpret_cast<std::uintptr_t>(p) / sizeof(float)) & 3);
}

// The aligned address at or below p.
OPUSPP_INLINE const float* align_down(const float* p) noexcept {
   return reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(p) & ~std::uintptr_t(15));
}

// Calls f.template operator()<R>() with R = misalignment of p.
template <class F>
OPUSPP_INLINE decltype(auto) with_misalignment(const float* p, F&& f) noexcept {
   switch (misalignment(p)) {
   case 0: return f.template operator()<0>();
   case 1: return f.template operator()<1>();
   case 2: return f.template operator()<2>();
   default: return f.template operator()<3>();
   }
}

// Applies an element-wise operation to n elements. `vec(i)` handles four
// elements starting at an index where dst + i is 16-byte aligned; `scalar(i)`
// handles one element. Peeling a scalar head/tail is exact because the
// elements are independent.
template <class V, class S>
OPUSPP_INLINE void for_each_aligned(const float* dst, int n, V&& vec, S&& scalar) noexcept {
   int i = 0;
   const int head = min(n, (4 - misalignment(dst)) & 3);
   OPUSPP_NO_VECTORIZE
   for (; i < head; i++) scalar(i);
   for (; i + 4 <= n; i += 4) vec(i);
   OPUSPP_NO_VECTORIZE
   for (; i < n; i++) scalar(i);
}

// Bulk helpers for n floats (a multiple of 4) at Align-byte aligned
// addresses. With Align == 64 whole cache lines are moved as simd<float, 16>,
// whose 64-byte alignment is part of the type, so every target width (4x SSE,
// 2x AVX, 1x AVX-512) uses aligned instructions; the rest goes by 16 bytes.
using f32x16 = simd<float, 16>;

template <std::size_t Align = 16>
inline void copy_aligned(float* OPUSPP_RESTRICT dst, const float* OPUSPP_RESTRICT src, int n) noexcept {
   OPUSPP_ASSERT(n % 4 == 0);
   int i = 0;
   if constexpr (Align >= 64)
      for (; i + 16 <= n; i += 16) f32x16::load_aligned(src + i).store_aligned(dst + i);
   for (; i < n; i += 4) f32x4::load_aligned(src + i).store_aligned(dst + i);
}

// Copies n floats between non-overlapping buffers of any float alignment
// (both inside 16-byte aligned arrays): aligned stores after a scalar head,
// loads assembled from aligned blocks.
inline void copy_any(float* OPUSPP_RESTRICT dst, const float* OPUSPP_RESTRICT src, int n) noexcept {
   int i = 0;
   const int head = min(n, (4 - misalignment(dst)) & 3);
   OPUSPP_NO_VECTORIZE
   for (; i < head; i++) dst[i] = src[i];
   if (i + 4 <= n) {
      with_misalignment(src + i, [&]<int R>() {
         const float* sa = src + i - R;
         f32x4 b0 = f32x4::load_aligned(sa);
         for (; i + 4 <= n; i += 4, sa += 4) {
            f32x4 v = b0;
            if constexpr (R > 0) {
               const f32x4 b1 = f32x4::load_aligned(sa + 4);
               v = window<R>(b0, b1);
               b0 = b1;
            } else if (i + 8 <= n) {
               b0 = f32x4::load_aligned(sa + 4);
            }
            v.store_aligned(dst + i);
         }
      });
   }
   OPUSPP_NO_VECTORIZE
   for (; i < n; i++) dst[i] = src[i];
}

// memmove() for 16-byte aligned buffers with dst below src (shifting left).
template <std::size_t Align = 16>
inline void move_down_aligned(float* dst, const float* src, int n) noexcept {
   OPUSPP_ASSERT(dst <= src && n % 4 == 0);
   int i = 0;
   // Each chunk is loaded before it is stored and dst < src, so this is a
   // correct forward memmove for any overlap.
   if constexpr (Align >= 64)
      for (; i + 16 <= n; i += 16) f32x16::load_aligned(src + i).store_aligned(dst + i);
   for (; i < n; i += 4) f32x4::load_aligned(src + i).store_aligned(dst + i);
}

// Fills n floats (a multiple of 4) of a 16-byte aligned buffer.
template <std::size_t Align = 16>
inline void fill_aligned(float* dst, float v, int n) noexcept {
   OPUSPP_ASSERT(n % 4 == 0);
   int i = 0;
   if constexpr (Align >= 64) {
      const f32x16 x16 = f32x16::broadcast(v);
      for (; i + 16 <= n; i += 16) x16.store_aligned(dst + i);
   }
   const f32x4 x = f32x4::broadcast(v);
   for (; i < n; i += 4) x.store_aligned(dst + i);
}

// Two complex numbers per vector, interleaved as {re0, im0, re1, im1}.
// {re, im} -> {im, re}
OPUSPP_INLINE f32x4 swap_pairs(f32x4 a) noexcept { return shuffle<1, 0, 3, 2>(a, a); }

// {re, im} -> {im, -re}, i.e. multiplication by -i. Exact.
OPUSPP_INLINE f32x4 mul_neg_i(f32x4 a) noexcept {
   return swap_pairs(a) * f32x4::set(1.f, -1.f, 1.f, -1.f);
}

// Complex multiply a*b for twiddles given as br = {br0, br0, br1, br1} and
// bi = {bi0, bi0, bi1, bi1}. Each lane evaluates exactly the scalar
// expressions re = ar*br - ai*bi and im = ar*bi + ai*br (C_MUL): negation and
// the order of the two terms of a sum do not change IEEE results.
OPUSPP_INLINE f32x4 cmul(f32x4 a, f32x4 br, f32x4 bi) noexcept {
   const f32x4 t1 = a * br;                // {ar*br, ai*br}
   const f32x4 t2 = swap_pairs(a) * bi;    // {ai*bi, ar*bi}
   return t1 + t2 * f32x4::set(-1.f, 1.f, -1.f, 1.f);
}

} // namespace opuspp::detail
