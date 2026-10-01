// Cache-line aligned interleaved stereo buffer holding one decoded 10 ms frame.
//
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstddef>

#include "detail/config.hpp"

namespace opuspp {

inline constexpr std::size_t cache_line_size = detail::output_alignment;

// One 10 ms frame at 48 kHz: 480 frames of interleaved L/R float samples in
// the range [-1, 1]. The first sample starts on a cache line, and the object
// occupies exactly 60 cache lines, so arrays of buffers stay aligned too.
struct alignas(cache_line_size) AudioBuffer {
   static constexpr int channels = detail::channels;
   static constexpr int frames = detail::frame_size;
   static constexpr int size = frames * channels;
   static constexpr int sample_rate = 48000;

   float samples[size];

   constexpr float* data() noexcept { return samples; }
   constexpr const float* data() const noexcept { return samples; }

   // Sample of `channel` (0 = left, 1 = right) at time index `frame`.
   constexpr float& operator()(int frame, int channel) noexcept { return samples[frame * channels + channel]; }
   constexpr float operator()(int frame, int channel) const noexcept { return samples[frame * channels + channel]; }

   constexpr float* begin() noexcept { return samples; }
   constexpr float* end() noexcept { return samples + size; }
   constexpr const float* begin() const noexcept { return samples; }
   constexpr const float* end() const noexcept { return samples + size; }
};

static_assert(alignof(AudioBuffer) == cache_line_size);
static_assert(sizeof(AudioBuffer) == AudioBuffer::size * sizeof(float));
static_assert(sizeof(AudioBuffer) % cache_line_size == 0);

} // namespace opuspp
