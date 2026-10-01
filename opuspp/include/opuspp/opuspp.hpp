// opuspp - freestanding, header-only C++23 Opus decoder.
//
// A port of the libopus 1.6.1 float decoder restricted to the streams produced
// by an encoder configured with OPUS_APPLICATION_RESTRICTED_CELT,
// OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND), 10 ms frames, complexity 0-4
// and OPUS_SET_PREDICTION_DISABLED(1) (so no pitch post-filter):
//
//   * CELT-only fullband 10 ms packets (TOC config 30), mono- or stereo-coded,
//     one frame per packet (code 0, or code 3 with a frame count of one, e.g.
//     CBR padding);
//   * packets without audio data (0 or 1 payload bytes, emitted by the encoder
//     for DTX or when it has fewer than 3 bytes available) of any 10 ms
//     configuration are concealed, like libopus does;
//   * always decoded to 48 kHz, 2 channels, 480 samples per channel into a
//     cache-line aligned AudioBuffer, interleaved float in [-1, 1] (no soft
//     clipping, like opus_decode_float);
//   * packet loss concealment through the pitch/noise based CELT PLC.
//
// Anything else (SILK, hybrid, narrower bandwidths, other frame durations,
// multi-frame packets) is rejected with Error::unsupported_packet. There is no
// FEC, DRED, OSCE, multistream or gain control. The decoder never allocates
// memory and needs nothing from the C/C++ runtime beyond what a freestanding
// implementation provides.
//
// Copyright (c) 2010-2011 Xiph.Org Foundation, Skype Limited.
// Written by Jean-Marc Valin and Koen Vos (original C code).
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstddef>
#include <cstdint>

#include "audio_buffer.hpp"
#include "detail/celt_decoder.hpp"
#include "detail/config.hpp"
#include "detail/entdec.hpp"

namespace opuspp {

enum class Error : int {
   ok = 0,
   internal_error = -3,    // the CELT decoder read past the end of the frame
   invalid_packet = -4,    // malformed packet
   unsupported_packet = -5 // valid Opus, but not a 10 ms fullband CELT-only single-frame packet
};

class Decoder {
public:
   static constexpr int sample_rate = AudioBuffer::sample_rate;
   static constexpr int channels = AudioBuffer::channels;
   static constexpr int frame_size = AudioBuffer::frames;  // samples per channel per packet

   Decoder() noexcept { reset(); }

   // Returns the decoder to its initial state (OPUS_RESET_STATE).
   void reset() noexcept {
      celt_.reset();
      stream_channels_ = channels;
      got_packet_ = false;
      range_final_ = 0;
      celt_.set_stream_channels(stream_channels_);
   }

   // Decodes one packet into `out`. A null `data` or a zero `size` conceals
   // one lost 10 ms frame instead. On success `out` holds the next 10 ms of
   // audio. On invalid_packet/unsupported_packet the decoder state and `out`
   // are untouched; on internal_error (libopus behaviour) the frame was
   // decoded but overran its bits.
   [[nodiscard]] Error decode(const std::uint8_t* data, std::size_t size, AudioBuffer& out) noexcept;

   // Conceals one lost 10 ms frame. Before the first decoded packet (or after
   // reset()) this outputs silence, as libopus does.
   void decode_lost(AudioBuffer& out) noexcept { conceal(out.samples); }

   // Final state of the range coder after the last decoded packet
   // (OPUS_GET_FINAL_RANGE), for conformance checks against an encoder.
   std::uint32_t final_range() const noexcept { return range_final_; }

private:
   // Fullband CELT-only 10 ms: TOC bits 7..3 = 30.
   static constexpr int supported_config = 30;

   struct Frame {
      const std::uint8_t* data;
      int size;
   };

   static Error parse_packet(const std::uint8_t* data, std::size_t size, Frame& frame) noexcept;
   void conceal(float* pcm) noexcept;

   detail::CeltDecoder celt_;
   int stream_channels_;
   bool got_packet_;
   std::uint32_t range_final_;
};

// Validates the packet framing (opus_packet_parse_impl) and extracts its
// single 10 ms frame. The mode and bandwidth are checked by the caller.
inline Error Decoder::parse_packet(const std::uint8_t* data, std::size_t size, Frame& frame) noexcept {
   if (size > 0x7FFFFFFF) return Error::invalid_packet;
   int len = static_cast<int>(size);
   const std::uint8_t toc = *data++;
   len--;
   // The frame must last 10 ms, whatever the mode.
   const bool is_10ms = (toc & 0x80)            ? ((toc >> 3) & 0x3) == 2   // CELT-only
                        : (toc & 0x60) == 0x60 ? (toc & 0x08) == 0        // hybrid
                                                : ((toc >> 3) & 0x3) == 0; // SILK-only
   if (!is_10ms) return Error::unsupported_packet;
   int last_size = len;
   switch (toc & 0x3) {
   case 0:  // One frame.
      break;
   case 1:  // Two CBR frames.
   case 2:  // Two VBR frames.
      return Error::unsupported_packet;
   default: {  // Frame count byte, optional padding.
      if (len < 1) return Error::invalid_packet;
      const std::uint8_t ch = *data++;
      len--;
      const int count = ch & 0x3F;
      if (count <= 0) return Error::invalid_packet;
      if (count != 1) return Error::unsupported_packet;
      // Padding flag is bit 6.
      if (ch & 0x40) {
         int p;
         do {
            if (len <= 0) return Error::invalid_packet;
            p = *data++;
            len--;
            const int tmp = p == 255 ? 254 : p;
            len -= tmp;
         } while (p == 255);
      }
      if (len < 0) return Error::invalid_packet;
      // With a single frame, CBR and VBR framing are identical.
      last_size = len;
      break;
   }
   }
   // The size of the last frame is implicit, so it might exceed the maximum.
   if (last_size > 1275) return Error::invalid_packet;
   frame.data = data;
   frame.size = last_size;
   return Error::ok;
}

// PLC/DTX path of opus_decode_frame.
inline void Decoder::conceal(float* pcm) noexcept {
   // If we haven't got any packet yet, all we can do is return zeros.
   if (!got_packet_) {
      detail::fill_aligned<64>(pcm, 0.f, AudioBuffer::size);
      return;
   }
   celt_.set_stream_channels(stream_channels_);
   celt_.decode_lost(pcm);
   range_final_ = 0;
}

inline Error Decoder::decode(const std::uint8_t* data, std::size_t size, AudioBuffer& out) noexcept {
   if (data == nullptr || size == 0) {
      conceal(out.samples);
      return Error::ok;
   }

   Frame frame;
   if (const Error err = parse_packet(data, size, frame); err != Error::ok) return err;
   const int stream_channels = (data[0] & 0x4) ? 2 : 1;

   // Payloads of 1 (2 including ToC) or 0 bytes trigger the PLC/DTX, whatever
   // mode the TOC signals (the encoder's initial mode is hybrid).
   if (frame.size <= 1) {
      stream_channels_ = stream_channels;
      conceal(out.samples);
      return Error::ok;
   }
   if ((data[0] >> 3) != supported_config) return Error::unsupported_packet;

   // Supported streams come from encoders with prediction disabled, which
   // never enable the pitch post-filter.
   if (detail::CeltDecoder::uses_postfilter(frame.data, int(frame.size))) return Error::unsupported_packet;

   // Update the state only once the packet is known to be valid.
   stream_channels_ = stream_channels;
   detail::RangeDecoder dec(frame.data, static_cast<std::uint32_t>(frame.size));
   celt_.set_stream_channels(stream_channels_);
   const int ret = celt_.decode(dec, frame.size, out.samples);
   range_final_ = celt_.final_range();
   got_packet_ = true;
   return ret < 0 ? Error::internal_error : Error::ok;
}

} // namespace opuspp
