---
name: cxxport-freestanding
description: Keep a header-only C++ port truly freestanding - no libc, libm, heap, exceptions or RTTI - and catch the hidden library calls compilers insert (memset/memcpy from aggregate initialisers, sqrtf fallbacks), with constexpr table generation and in-library math that matches libm bitwise. Use when writing or reviewing code in a freestanding header library, or when nm -u shows an undefined symbol.
---

# Freestanding C++ for audio code

## The contract

* Headers: only `<cstddef>`, `<cstdint>` and `<bit>`. No `<cmath>`,
  `<cstring>`, `<algorithm>`, `<array>`.
* No heap: all state in one object the caller places (static, stack, own
  allocator); scratch on the stack, `alignas(64)`, worst-case sized.
* No exceptions, RTTI, asm.
* **The check:** compile a translation unit that calls the public API with
  `-ffreestanding -fno-exceptions -fno-rtti -Wall -Wextra -Wshadow
  -Wconversion -Wno-sign-conversion -Werror`, at -O0, -O1, -O2, -O3, -Os,
  `-Os -fno-math-errno` and the fast-math mode, with GCC and Clang. `nm -u` on
  the object must print nothing.

## Hidden library calls (all found the hard way)

| Source | Fix |
|---|---|
| Aggregate zero-initialisers (`T x[N] = {}`, `S s{}`, `s = S{}`) make Clang -O0 call `memset`/`memcpy` | Explicit loops or field assignments; or zero with `= S{}` only under `#ifdef __OPTIMIZE__` and use loops otherwise |
| Struct copies at -O0 | Same: explicit copies through aligned helper loops |
| GCC turns `__builtin_sqrtf` into a `sqrtf` call (for errno) unless `__NO_MATH_ERRNO__` is set; at -O0/-Os it can't inline it at all, and `__attribute__((optimize("no-math-errno")))` and the pragma don't help | Clang: `__builtin_elementwise_sqrt`. GCC: `__builtin_sqrtf` under `__NO_MATH_ERRNO__` (or optimised, non-size builds with a guard); on x86 at -O0/-Os use `__builtin_ia32_sqrtss`; elsewhere a correctly rounded software sqrt (slow) |
| `__builtin_memcpy` for type punning | Fine: it is always inlined for small constant sizes; confirm with `nm -u` |

Find the culprit of a `memset`/`memcpy` symbol:

```sh
objdump -dlr --no-show-raw-insn obj.o | grep -B8 "PLT32\s*mem"
```

## Math without libm

* Implement `exp`, `cos` and friends in double precision accurate to a few
  ulp, so the **float-rounded** results equal what the reference gets from
  libm. Prove it: compare bitwise against host libm over tens of millions of
  arguments drawn from the ranges the code actually uses, and require zero
  mismatches.
* Use the hardware square root (see above); a software `sqrtf` is slow
  enough to show up clearly in benchmarks.

## Tables at compile time without exceptions

* Generated tables are `alignas(64) inline constexpr`.
* Derived tables are built by `constexpr` lambdas. Inside constant
  evaluation, report failure by calling a non-`constexpr` function (for
  example `table_generation_failed()`): the call makes compilation fail,
  where `throw` is unavailable under `-fno-exceptions`.

## Warnings worth keeping on

* `-Wshadow`: helper names (`fill`, `window`, `c1`) were silently shadowed by
  parameters.
* GCC `-Wmaybe-uninitialized` in a vector broadcast: write it as
  `native_type{} + x`.
* GCC `-Warray-bounds` in padding loops: pass the destination size in
  explicitly.
