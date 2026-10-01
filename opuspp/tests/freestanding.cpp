// Compiled with -ffreestanding -fno-exceptions -fno-rtti -Werror by the test
// build to make sure the library only needs a freestanding implementation.
#include "opuspp/opuspp.hpp"

namespace {
opuspp::Decoder decoder;
}

extern "C" int opuspp_freestanding_decode(const unsigned char* packet, unsigned long size, opuspp::AudioBuffer* out) {
   return static_cast<int>(decoder.decode(packet, size, *out));
}
