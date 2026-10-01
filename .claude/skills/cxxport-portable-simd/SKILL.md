---
name: cxxport-portable-simd
description: Vectorise a bit-exact C++ audio port with a portable simd<T,N> value type (GCC/Clang vector extensions plus a plain-array backend) instead of intrinsics or asm - mirroring the reference SIMD kernels' lane layout so results stay bit-identical, keeping every vector memory access aligned (alignas(64), padding, sliding windows from aligned blocks), and avoiding the compiler's store merging. Use when designing the SIMD layer or vectorising a kernel in such a port.
---

# Portable, bit-exact, aligned SIMD

## The value type

A `simd<T, N>` struct (`alignas(sizeof(T) * N)`) with:

* **Two backends with identical per-lane arithmetic:** GCC/Clang vector
  extensions (`__attribute__((vector_size))`), which lower to SSE, AVX, NEON,
  AltiVec, RVV or WASM SIMD; and a plain array with loops for other compilers
  or when forced (a `..._NO_VECTOR_EXT` define). The differential test runs
  with both (see `cxxport-bit-exact-oracle`).
* `load`/`store` (element-aligned) and `load_aligned`/`store_aligned`
  (vector-aligned, compile to `movaps`-class instructions). Prefer the
  aligned forms everywhere; see below.
* Lane-wise arithmetic, compares, `bit_cast`/`convert`, compile-time
  shuffles of the concatenation `{a, b}` (`__builtin_shufflevector` /
  `__builtin_shuffle`), reverse, and 4×4 transposes.
* **Exact negation:** `-a` is `a * -1`, never `0 - a`, which turns `-0` into
  `+0`.
* **Min/max with the reference macro semantics:** `a > b ? a : b` lane-wise,
  mask-based, so the second operand wins on NaN exactly like the scalar
  `MAX32`/`MIN32`.
* Broadcast written as `native_type{} + x` (avoids a GCC
  `-Wmaybe-uninitialized` false positive).

Width 4 floats covers SSE and NEON and maps cleanly onto the reference SSE
kernels; bulk memory helpers use `simd<float, 16>` (one cache line).

## Bit-exactness rules

* **Each lane computes exactly the scalar reference expression**, in the same
  order. Vectorise across independent elements, never across a dependency.
* **Reductions copy the reference SIMD kernel's lane layout** (its SSE or
  NEON implementation of the same function): which partial sums go in which
  lane, and the order of the final horizontal sum. Don't invent a new summation tree; an
  auto-vectorised reduction changes results, so mark such scalar loops
  no-vectorise (Clang `#pragma clang loop vectorize(disable)
  interleave(disable) unroll(disable)`, GCC `#pragma GCC novector` plus
  `#pragma GCC unroll 1`) with a comment saying why.
* **Re-run the differential test after every kernel.** Vectorise in order of
  payoff, typically: the kernels the reference already has SIMD versions
  of; FFT butterflies and transforms; noise fill and similar generators;
  then the element-wise loops.
* What stays scalar: genuinely serial code (entropy decoding, bit
  allocation, combinatorial decoding, LPC/IIR recursions, rotations).
* Random-number loops can still be vectorised: run N lanes of an LCG with
  jump-ahead constants so lane k produces the k-th value of the serial
  sequence.

## Vector width

Reference libraries are often written for 4-wide SSE/NEON, and copying their
kernels keeps the port at that width even on AVX2/AVX-512 targets, where the
compiler won't widen explicit 4-lane code by itself. Decide width per kernel:

* **Reductions are bounded by the reference.** A kernel that sums across
  lanes must keep the reference's lane layout and final summation order to
  stay bit-exact, so it stays at the reference's width.
* **Element-wise kernels can scale.** Where each lane computes the same
  scalar expression independently (windowing, rotations, scaling, mixing,
  state updates, bulk moves), 8 or 16 lanes give identical results. Write
  them against a width chosen at compile time from the target (for example
  4 by default, 8 with AVX2, 16 with AVX-512) rather than hardcoding 4, and
  pad buffers to the widest width.
* **Kernels whose lanes are independent accumulators** (a cross-correlation
  computing several lags at once, each summed in order) can also widen
  without changing any lane's summation order.
* **Measure before widening:** the width census in `cxxport-codegen-audit`
  shows how much of the run time is in 128-bit vector code at all; if most
  of it is serial integer work, wider vectors won't pay.

## Alignment

* **Every buffer and table `alignas(64)`;** pad per-channel rows and band
  arrays to whole vectors (for example 21 bands stored with a stride of 24).
  Tell the compiler with `__builtin_assume_aligned(p, 64)` (`<memory>` is
  off limits) and `[[assume(n % 4 == 0)]]`.
* **Sliding windows** (`x[i + k]` for a runtime `k`, `x[i - T]` for a pitch
  lag, band partitions at arbitrary offsets) are **never read with unaligned
  loads**. Resolve the misalignment once per call into one of four template
  instantiations (`with_misalignment(p, [&]<int R>() {...})`, R = 0..3), and
  assemble each window from the aligned blocks around it with a compile-time
  shuffle (`window<S>(a, b)`, like NEON `vext` / SSSE3 `palignr`). Load only
  blocks that contain requested elements, so nothing outside the array is
  touched (ASan proves it).
* **Element-wise loops over unaligned ranges:** peel a scalar head to the
  first aligned address, run aligned vectors, then a scalar tail. Exact,
  because the elements are independent.
* **Compilers merge stores.** Clang with AVX/AVX2 merges two adjacent aligned
  16-byte operations into one unaligned 32-byte one unless the base is 64-byte
  aligned; Clang merges field-by-field struct zeroing into overlapping
  unaligned 16-byte stores. Hence: 64-byte aligned bases, bulk helpers
  (`copy_aligned<64>`, `fill_aligned<64>`, `move_down_aligned<64>`) that move
  whole cache lines as `simd<float, 16>`, and state structs padded to a
  multiple of 64 bytes.
* **Accepted exception:** Clang with AVX-512 recombines "aligned blocks +
  shuffle" back into unaligned loads; preventing that needs inline asm.
  Document it rather than fight it.

Check the result with `cxxport-codegen-audit`.

## Why it matters

Specialisation alone doesn't make a port fast. With only the auto-vectoriser
(the plain-array backend), a port can end up slower than the reference with
one compiler and faster with another; the explicit, aligned SIMD layer is
what makes it fast independently of the compiler's auto-vectoriser. Measure
this with an ablation (`cxxport-perf-and-measure`).
