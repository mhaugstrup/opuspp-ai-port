// Checks the freestanding math replacements against the host libm: the
// float-rounded results of celt_exp2/celt_cos_norm/celt_sqrt must match what
// the reference float build computes through libm.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "opuspp/detail/math.hpp"

namespace {

struct Rng {
   std::uint64_t s;
   std::uint32_t next() {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      return std::uint32_t(s >> 32);
   }
   float uniform(float lo, float hi) { return lo + (hi - lo) * float(next() >> 8) * (1.f / 16777216.f); }
};

}  // namespace

int main(int argc, char** argv) {
   const long n = argc > 1 ? std::atol(argv[1]) : 20000000;
   Rng rng{99};
   long bad_exp = 0, bad_cos = 0, bad_sqrt = 0;
   for (long i = 0; i < n; i++) {
      // celt_exp2 arguments: band energies (log2 domain) and anti-collapse terms.
      const float xe = rng.uniform(-64.f, 32.f);
      const float ref_e = float(std::exp(0.6931471805599453094 * xe));
      if (std::bit_cast<std::uint32_t>(ref_e) != std::bit_cast<std::uint32_t>(opuspp::detail::celt_exp2(xe))) {
         if (bad_exp++ < 5) std::printf("exp2 mismatch at %.9g\n", double(xe));
      }
      // celt_cos_norm arguments: theta and 1 - theta in [0, 1].
      const float xc = rng.uniform(0.f, 1.f);
      const float ref_c = float(std::cos((.5f * 3.1415926535897931) * xc));
      if (std::bit_cast<std::uint32_t>(ref_c) != std::bit_cast<std::uint32_t>(opuspp::detail::celt_cos_norm(xc))) {
         if (bad_cos++ < 5) std::printf("cos mismatch at %.9g\n", double(xc));
      }
      // sqrt over the whole positive float range.
      const float xs = std::bit_cast<float>(rng.next() & 0x7F7FFFFFu);
      if (std::bit_cast<std::uint32_t>(float(std::sqrt(double(xs)))) !=
          std::bit_cast<std::uint32_t>(opuspp::detail::celt_sqrt(xs))) {
         if (bad_sqrt++ < 5) std::printf("sqrt mismatch at %.9g\n", double(xs));
      }
   }
   std::printf("%ld samples each: exp2 %ld, cos %ld, sqrt %ld mismatches\n", n, bad_exp, bad_cos, bad_sqrt);
   return (bad_exp || bad_cos || bad_sqrt) ? 1 : 0;
}
