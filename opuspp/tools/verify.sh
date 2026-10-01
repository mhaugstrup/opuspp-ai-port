#!/usr/bin/env bash
# Full acceptance check for opuspp (see README.md, "Changing the code").
#
#   tools/verify.sh [build-dir] [--quick]
#
# 1. CMake test build (Release) + ctest: compare, compare_approx, fuzz,
#    math_check and the freestanding object.
# 2. compare + fuzz under ASan/UBSan.
# 3. Freestanding object: no undefined symbols, -Werror, both compilers, all -O levels.
# 4. Bit-exactness matrix: compare with GCC/Clang x -O0..-O3/-Os x both SIMD
#    backends, plus x86-64-v2/v3/v4 with -ffp-contract=off.
# 5. Alignment: 0 unaligned vector memory accesses (tools/align_report.py).
#
# --quick runs a reduced matrix (-O0/-O2 only, shorter streams).
# Exits non-zero on the first failing stage; prints PASS at the end.
#
# Not automated, check by hand: tools/vec_report.py (every loop that isn't
# vectorised has a reason) and tools/profile_report.py (no benchmark case is
# slower than libopus).
set -euo pipefail
shopt -s lastpipe

root=$(cd "$(dirname "$0")/.." && pwd)
libopus=${OPUSPP_LIBOPUS_DIR:-$root/../opus-1.6.1}   # reference sources, outside the project
build=$root/build-verify
quick=0
for a in "$@"; do
   if [[ $a == --quick ]]; then quick=1; else build=$(realpath -m "$a"); fi
done
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
step() { printf '\n=== %s\n' "$*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

inc=(-I"$root/include" -I"$libopus/include")
cxx_list=(g++ clang++)
if ((quick)); then opt_list=(-O0 -O2); secs=5; fuzz_iter=50000
else opt_list=(-O0 -O1 -O2 -O3 -Os); secs=21; fuzz_iter=300000; fi

step "1. CMake Release build + ctest ($build)"
cmake -S "$root" -B "$build" -DOPUSPP_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release \
      -DOPUSPP_LIBOPUS_DIR="$libopus" >/dev/null
cmake --build "$build" -j >"$tmp/build.log" 2>&1 || { tail -40 "$tmp/build.log"; fail build; }
ctest --test-dir "$build" --output-on-failure || fail ctest
ref=$build/opus-1.6.1/libopus.a   # SSE baseline (AVX2 dispatch off)
[[ -f $ref ]] || fail "reference library $ref not found"

step "2. ASan/UBSan: compare + fuzz"
san=(-std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined)
clang++ "${san[@]}" "${inc[@]}" "$root/tests/compare.cpp" "$ref" -lm -o "$tmp/compare_san"
clang++ "${san[@]}" "${inc[@]}" "$root/tests/fuzz.cpp" "$ref" -lm -o "$tmp/fuzz_san"
"$tmp/compare_san" "$secs" | tail -1 | grep -q PASS || fail "compare under sanitizers"
"$tmp/fuzz_san" "$fuzz_iter" | tail -1 || fail "fuzz under sanitizers"

step "3. Freestanding object: no external symbols"
fs_flags=(-std=c++23 -ffreestanding -fno-exceptions -fno-rtti -Wall -Wextra -Wshadow -Wconversion
          -Wno-sign-conversion -Werror -I"$root/include" -c "$root/tests/freestanding.cpp")
for c in "${cxx_list[@]}"; do
   for o in -O0 -O1 -O2 -O3 -Os "-Os -fno-math-errno" "-O2 -ffast-math -DOPUSPP_FLOAT_APPROX"; do
      # shellcheck disable=SC2086
      $c "${fs_flags[@]}" $o -o "$tmp/fs.o" || fail "$c $o does not compile cleanly"
      undef=$(nm -u "$tmp/fs.o" | awk '{print $2}' | tr '\n' ' ')
      [[ -z $undef ]] || fail "$c $o references: $undef"
      echo "  $c $o: ok"
   done
done

step "4. Bit-exactness matrix (compare, $secs s per stream)"
run_compare() {  # compiler, flags...
   local c=$1; shift
   $c -std=c++23 "$@" "${inc[@]}" "$root/tests/compare.cpp" "$ref" -lm -o "$tmp/cmp" \
      || fail "$c $* does not build"
   local r; r=$("$tmp/cmp" "$secs" | tail -1)
   echo "  $c $*: $r"
   [[ $r == PASS* ]] || fail "$c $*"
}
for c in "${cxx_list[@]}"; do
   for o in "${opt_list[@]}"; do
      run_compare "$c" "$o"
      run_compare "$c" "$o" -DOPUSPP_NO_VECTOR_EXT
   done
   for m in x86-64-v2 x86-64-v3 x86-64-v4; do
      if [[ $m == x86-64-v4 ]] && ! grep -q avx512f /proc/cpuinfo; then
         echo "  $c -march=$m: skipped (no AVX-512 on this CPU)"; continue
      fi
      run_compare "$c" -O2 -march=$m -ffp-contract=off
   done
done

step "5. Alignment: unaligned vector memory accesses"
check_align() {  # compiler, flags... ; must report 0 unaligned
   local r; r=$(python3 "$root/tools/align_report.py" "$@" | head -1)
   echo "  $r"
   [[ $r == *": 0 unaligned"* ]] || fail "unaligned accesses: $*"
}
for o in -O1 -O2 -O3 -Os; do
   for m in "" -march=x86-64-v2 -march=x86-64-v3 -march=x86-64-v4; do
      # shellcheck disable=SC2086
      check_align g++ $o $m
   done
   for m in "" -march=x86-64-v2 -march=x86-64-v3; do
      # shellcheck disable=SC2086
      check_align clang++ $o $m
   done
done
echo "  (clang++ -march=x86-64-v4 is a documented exception: see tools/align_report.py)"

printf '\nPASS\n'
