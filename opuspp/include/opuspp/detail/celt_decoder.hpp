// CELT decoder (celt/celt_decoder.c) specialised for the static 48 kHz mode,
// stereo output, 10 ms fullband frames (LM = 2, all 21 bands coded), no
// downsampling and no accumulation.
//
// Copyright (c) 2007-2008 CSIRO, 2007-2010 Xiph.Org Foundation, 2008 Gregory Maxwell.
// Written by Jean-Marc Valin.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "bands.hpp"
#include "celt_tables.hpp"
#include "config.hpp"
#include "entdec.hpp"
#include "math.hpp"
#include "mdct.hpp"
#include "pitch.hpp"
#include "rate.hpp"

namespace opuspp::detail {

class CeltDecoder {
public:
   static constexpr int ok = 0;
   static constexpr int internal_error = -3;

   CeltDecoder() noexcept { reset(); }

   // Clears all state (OPUS_RESET_STATE). The stream channel count is
   // configuration and survives a reset.
   void reset() noexcept {
      s_.clear();
      for (auto& ch : decode_mem_) fill_aligned<64>(ch, 0.f, decode_mem_stride);
      fill_aligned<64>(old_band_e_, 0.f, 2 * band_stride);
      fill_aligned<64>(old_log_e_, -28.f, 2 * band_stride);
      fill_aligned<64>(old_log_e2_, -28.f, 2 * band_stride);
      fill_aligned<64>(background_log_e_, 0.f, 2 * band_stride);
      for (auto& l : lpc_) fill_aligned(l, 0.f, lpc_order);
   }

   void set_stream_channels(int c) noexcept { stream_channels_ = c; }
   std::uint32_t final_range() const noexcept { return s_.rng; }

   // Decodes one 10 ms frame from `dec` (initialised over `len` bytes) into
   // `pcm` (480 interleaved stereo samples, 64-byte aligned). Returns 480 or
   // a negative error. `pcm` also serves as scratch for the decoded spectrum
   // (2 * N floats, exactly its size) until de-emphasis writes the output.
   int decode(RangeDecoder& dec, int len, float* OPUSPP_RESTRICT pcm) noexcept;

   // True if the CELT frame `data` (`len` bytes) enables the pitch
   // post-filter, which an encoder with prediction disabled
   // (OPUS_SET_PREDICTION_DISABLED) never does. Reads the same leading fields
   // as decode() (silence flag, post-filter flag) with a range decoder of its
   // own, so the caller can reject the packet before any state changes. (A
   // copy of the caller's decoder would be a struct copy, which unoptimised
   // Clang does with a memcpy call.)
   static bool uses_postfilter(const std::uint8_t* data, int len) noexcept {
      RangeDecoder dec(data, static_cast<std::uint32_t>(len));
      const std::int32_t total_bits = len * 8;
      const std::int32_t tell = dec.tell();
      if (tell >= total_bits) return false;             // silence
      if (tell == 1 && dec.bit_logp(15)) return false;  // silence
      return start_ == 0 && tell + 16 <= total_bits && dec.bit_logp(1);
   }

   // Conceals one lost 10 ms frame into `pcm`, which also serves as scratch
   // for the noise-based concealment. Returns 480.
   int decode_lost(float* OPUSPP_RESTRICT pcm) noexcept {
      conceal(pcm);
      deemphasis(pcm);
      return frame_size;
   }

private:
   static constexpr int frame_none = 0;
   static constexpr int frame_normal = 1;
   static constexpr int frame_plc_noise = 2;
   static constexpr int frame_plc_periodic = 3;
   static constexpr int plc_pitch_lag_max = 720;
   static constexpr int plc_pitch_lag_min = 100;
   static constexpr int M = 1 << lm;
   static constexpr int N = frame_size;
   static constexpr int start_ = 0;       // CELT-only: always starts at band 0
   static constexpr int end_ = nb_ebands;  // fullband: always ends at band 21
   static constexpr int disable_inv_ = 0; // stereo decoder

   float* out_syn(int c) noexcept { return decode_mem_[c] + decode_buffer_size - N; }

   void conceal(float* OPUSPP_RESTRICT scratch) noexcept;
   void prefilter_and_fold() noexcept;
   void synthesis(float* X, int start, int eff_end, int C, int is_transient, int silence) noexcept;
   void deemphasis(float* OPUSPP_RESTRICT pcm) noexcept;
   int plc_pitch_search() noexcept;
   void conceal_periodic(int pitch_index, float fade) noexcept;

   // Configuration (not affected by reset()).
   int stream_channels_ = 2;

   // Scalar state: one aligned cache line, all zero after a reset. No default
   // member initialisers, so State{} is plain zero-initialisation, which the
   // optimiser turns into aligned vector stores (field-wise zeroing gets
   // merged into overlapping unaligned stores instead). Unoptimised Clang
   // would call memset/memcpy for State{}, which a freestanding build must
   // not reference, so -O0 zeroes the fields one by one.
   struct alignas(64) State {  // one cache line
      // decode() writes these four together at the end; compilers merge the
      // writes into one 16-byte store, which is aligned only at offset 0.
      int loss_duration;
      int plc_duration;
      int last_frame_type;  // frame_none == 0
      int prefilter_and_fold;
      std::uint32_t rng;
      int last_pitch_index;
      float preemph_mem[2];
      // libopus skip_plc inverted, so that every field resets to zero.
      int plc_enabled;

      void clear() noexcept {
#ifdef __OPTIMIZE__
         *this = State{};
#else
         rng = 0;
         last_pitch_index = 0;
         loss_duration = 0;
         plc_duration = 0;
         last_frame_type = 0;
         prefilter_and_fold = 0;
         preemph_mem[0] = 0;
         preemph_mem[1] = 0;
         plc_enabled = 0;
#endif
      }
   };
   static_assert(sizeof(State) == 64);
   State s_;  // initialised by reset() in the constructor
   // Rows padded to whole cache lines (decode_buffer_size + overlap = 2168).
   static constexpr int decode_mem_stride = (decode_buffer_size + overlap + 15) & ~15;
   alignas(buffer_alignment) float decode_mem_[channels][decode_mem_stride];
   // Band energies, channel c at [c * band_stride]; the padding lanes are
   // never read by the codec and only exist for aligned vector updates.
   alignas(buffer_alignment) float old_band_e_[2 * band_stride];
   alignas(buffer_alignment) float old_log_e_[2 * band_stride];
   alignas(buffer_alignment) float old_log_e2_[2 * band_stride];
   alignas(buffer_alignment) float background_log_e_[2 * band_stride];
   alignas(buffer_alignment) float lpc_[channels][lpc_order];
};

// De-emphasis for the stereo, no downsampling, no accumulation case
// (deemphasis_stereo_simple), converting to the +/-1 float output scale.
inline void CeltDecoder::deemphasis(float* OPUSPP_RESTRICT out) noexcept {
   float* OPUSPP_RESTRICT pcm = assume_aligned<output_alignment>(out);
   const float* x0 = out_syn(0);
   const float* x1 = out_syn(1);
   float m0 = s_.preemph_mem[0];
   float m1 = s_.preemph_mem[1];
   // The recursion is serial per channel; four samples of both channels are
   // loaded and stored as aligned vectors.
   const f32x4 scale = f32x4::broadcast(1 / sig_scale);
   for (int j = 0; j < N; j += 4) {
      const f32x4 a = f32x4::load_aligned(x0 + j);
      const f32x4 b = f32x4::load_aligned(x1 + j);
      float t0[4], t1[4];
      for (int k = 0; k < 4; k++) {
         // Add VERY_SMALL to x[] first to reduce dependency chain.
         t0[k] = a[k] + very_small + m0;
         t1[k] = b[k] + very_small + m1;
         m0 = preemph * t0[k];
         m1 = preemph * t1[k];
      }
      (f32x4::set(t0[0], t1[0], t0[1], t1[1]) * scale).store_aligned(pcm + 2 * j);
      (f32x4::set(t0[2], t1[2], t0[3], t1[3]) * scale).store_aligned(pcm + 2 * j + 4);
   }
   s_.preemph_mem[0] = m0;
   s_.preemph_mem[1] = m1;
}

// Inverse transform of all channels (celt_synthesis with CC = 2).
inline void CeltDecoder::synthesis(float* X, int start, int eff_end, int C, int is_transient,
                                   int silence) noexcept {
   alignas(buffer_alignment) float freq[N];
   // Four interleaved 120-point IMDCTs for transients, one 480-point otherwise.
   auto imdct = [is_transient](const float* f, float* out) {
      if (is_transient) {
         for (int b = 0; b < M; b++) mdct_backward<max_lm>(f + b, out + short_mdct_size * b, M);
      } else {
         mdct_backward<max_lm - lm>(f, out, 1);
      }
   };
   if (C == 1) {
      // Copying a mono stream to two channels.
      denormalise_bands(X, freq, old_band_e_, start, eff_end, silence);
      // Store a temporary copy in the output buffer because the IMDCT destroys its input.
      float* freq2 = out_syn(1) + overlap / 2;
      copy_aligned(freq2, freq, N);
      imdct(freq2, out_syn(0));
      imdct(freq, out_syn(1));
   } else {
      for (int c = 0; c < 2; c++) {
         denormalise_bands(X + c * N, freq, old_band_e_ + c * band_stride, start, eff_end, silence);
         imdct(freq, out_syn(c));
      }
   }
}

inline void CeltDecoder::prefilter_and_fold() noexcept {
   const float* window = tables::window120;
   alignas(buffer_alignment) float etmp[overlap];
   for (int c = 0; c < channels; c++) {
      float* mem = decode_mem_[c];
      // libopus applies the inverse post-filter (comb filter) to the MDCT
      // overlap here. Supported streams never use the post-filter, so its
      // gains are always zero and the comb filter is a plain copy.
      copy_aligned(etmp, mem + decode_buffer_size - N, overlap);
      // Simulate TDAC on the concealed audio so that it blends with the MDCT of the next frame.
      // mem[i] = window[i]*etmp[overlap-1-i] + window[overlap-1-i]*etmp[i]
      for (int i = 0; i < overlap / 2; i += 4) {
         const int j = overlap - 4 - i;
         const f32x4 w = f32x4::load_aligned(window + i);
         const f32x4 wr = reverse(f32x4::load_aligned(window + j));
         const f32x4 er = reverse(f32x4::load_aligned(etmp + j));
         (w * er + wr * f32x4::load_aligned(etmp + i)).store_aligned(mem + decode_buffer_size - N + i);
      }
   }
}

// Out of line: its scratch (the downsampled history, 4 KB, plus the pitch
// search's own) is released before conceal_periodic() allocates its own.
OPUSPP_NOINLINE inline int CeltDecoder::plc_pitch_search() noexcept {
   alignas(buffer_alignment) float lp_pitch_buf[decode_buffer_size >> 1];
   const float* mem[2] = {decode_mem_[0], decode_mem_[1]};
   pitch_downsample(mem, lp_pitch_buf, decode_buffer_size >> 1);
   const int pitch_index = pitch_search<decode_buffer_size - plc_pitch_lag_max, plc_pitch_lag_max - plc_pitch_lag_min>(
      lp_pitch_buf + (plc_pitch_lag_max >> 1), lp_pitch_buf);
   return plc_pitch_lag_max - pitch_index;
}

// Pitch-based concealment after the pitch is known: LPC analysis of the
// history, excitation extrapolation with decay, and re-synthesis (the
// periodic branch of celt_decode_lost).
OPUSPP_NOINLINE inline void CeltDecoder::conceal_periodic(int pitch_index, float fade) noexcept {
   constexpr int C = channels;
   const float* window = tables::window120;
   // We want the excitation for 2 pitch periods in order to look for a
   // decaying signal, but we can't get more than MAX_PERIOD.
   const int exc_length = min(2 * pitch_index, max_period);
   alignas(buffer_alignment) float exc_buf[max_period + lpc_order];
   alignas(buffer_alignment) float fir_tmp[max_period];
   float* exc = exc_buf + lpc_order;

   for (int c = 0; c < C; c++) {
      float* buf = decode_mem_[c];
      float* lpc = lpc_[c];
      copy_aligned(exc_buf, buf + decode_buffer_size - max_period - lpc_order, max_period + lpc_order);

      if (s_.last_frame_type != frame_plc_periodic) {
         alignas(buffer_alignment) float ac[(lpc_order + 1 + 3) & ~3];
         // Compute LPC coefficients for the last MAX_PERIOD samples before
         // the first loss so we can work in the excitation-filter domain.
         // fir_tmp is free until the FIR filter below: use it as scratch.
         celt_autocorr<lpc_order>(exc, ac, window, overlap, max_period, fir_tmp);
         // Add a noise floor of -40 dB.
         ac[0] *= 1.0001f;
         // Use lag windowing to stabilize the Levinson-Durbin recursion.
         // ac[i] -= ac[i]*(0.008*0.008)*i*i for i = 1..24; lane 0 of the first
         // block keeps ac[0], the padding lanes 25..27 are zeroed first.
         ac[25] = ac[26] = ac[27] = 0;
         for (int i = 0; i < lpc_order + 1; i += 4) {
            const f32x4 a = f32x4::load_aligned(ac + i);
            const f32x4 fi = f32x4::set(float(i), float(i + 1), float(i + 2), float(i + 3));
            const f32x4 r = a - a * f32x4::broadcast(0.008f * 0.008f) * fi * fi;
            (i == 0 ? shuffle<0, 5, 6, 7>(a, r) : r).store_aligned(ac + i);
         }
         celt_lpc(lpc, ac, lpc_order);
      }
      // Compute the excitation for exc_length samples before the loss.
      celt_fir<lpc_order>(exc + max_period - exc_length, lpc, fir_tmp, exc_length);
      copy_any(exc + max_period - exc_length, fir_tmp, exc_length);

      // Check if the waveform is decaying, and if so how fast.
      float decay;
      {
         float E1 = 1, E2 = 1;
         const int decay_length = exc_length >> 1;
         // In-order sums over arbitrarily aligned ranges: scalar.
         OPUSPP_NO_VECTORIZE
         for (int i = 0; i < decay_length; i++) {
            float e = exc[max_period - decay_length + i];
            E1 += e * e;
            e = exc[max_period - 2 * decay_length + i];
            E2 += e * e;
         }
         E1 = min(E1, E2);
         decay = celt_sqrt(E1 / E2);
      }

      // Move the decoder memory one frame to the left to give us room to
      // add the data for the new frame.
      move_down_aligned<64>(buf, buf + N, decode_buffer_size - N);

      // Extrapolate from the end of the excitation with a period of
      // pitch_index, scaling down each period by an additional factor of decay.
      const int extrapolation_offset = max_period - pitch_index;
      // Cover a complete MDCT window (including overlap/2 samples on both sides).
      constexpr int extrapolation_len = N + overlap;
      float attenuation = fade * decay;
      float S1 = 0;
      OPUSPP_NO_VECTORIZE
      for (int i = 0, j = 0; i < extrapolation_len; i++, j++) {
         if (j >= pitch_index) {
            j -= pitch_index;
            attenuation = attenuation * decay;
         }
         buf[decode_buffer_size - N + i] = attenuation * exc[extrapolation_offset + j];
         // Energy of the previously decoded signal whose excitation we're copying.
         const float tmp = buf[decode_buffer_size - max_period - N + extrapolation_offset + j];
         S1 += tmp * tmp;
      }
      {
         alignas(buffer_alignment) float lpc_mem[lpc_order];
         // Copy the last decoded samples (prior to the overlap region) to
         // synthesis filter memory so we can have a continuous signal:
         // lpc_mem[i] = buf[decode_buffer_size - N - 1 - i].
         for (int i = 0; i < lpc_order; i += 4)
            reverse(f32x4::load_aligned(buf + decode_buffer_size - N - 4 - i)).store_aligned(lpc_mem + i);
         // Apply the synthesis filter to convert the excitation back into the signal domain.
         celt_iir<lpc_order>(buf + decode_buffer_size - N, lpc, buf + decode_buffer_size - N,
                             extrapolation_len, lpc_mem);
      }

      // Check if the synthesis energy is higher than expected, which can
      // happen with the signal changes during our window. If so, attenuate.
      {
         float* syn = assume_aligned<16>(buf + decode_buffer_size - N);
         float S2 = 0;
         OPUSPP_NO_VECTORIZE
         for (int i = 0; i < extrapolation_len; i++) S2 += syn[i] * syn[i];
         // This test also catches NaNs in the output of the IIR filter.
         if (!(S1 > 0.2f * S2) || is_nan(S1) || is_nan(S2)) {
            fill_aligned<64>(syn, 0.f, extrapolation_len);
         } else if (S1 < S2) {
            const float ratio = celt_sqrt((S1 + 1) / (S2 + 1));
            const f32x4 one = f32x4::broadcast(1.f);
            const f32x4 r = f32x4::broadcast(ratio);
            const f32x4 one_minus_r = f32x4::broadcast(1.f - ratio);
            for (int i = 0; i < overlap; i += 4) {
               const f32x4 tmp_g = one - f32x4::load_aligned(window + i) * one_minus_r;
               (tmp_g * f32x4::load_aligned(syn + i)).store_aligned(syn + i);
            }
            for (int i = overlap; i < extrapolation_len; i += 4)
               (r * f32x4::load_aligned(syn + i)).store_aligned(syn + i);
         }
      }
   }
}

// Packet loss concealment (celt_decode_lost without the neural PLC).
// Kept out of line, like decode(): a stack frame holds all local arrays of a
// function, so inlining both paths into one caller would add the
// concealment scratch to the normal decoding scratch.
OPUSPP_NOINLINE inline void CeltDecoder::conceal(float* OPUSPP_RESTRICT scratch) noexcept {
   constexpr int C = channels;
   const int loss_duration = s_.loss_duration;
   int curr_frame_type = frame_plc_periodic;
   if (s_.plc_duration >= 40 || start_ != 0 || !s_.plc_enabled) curr_frame_type = frame_plc_noise;

   if (curr_frame_type == frame_plc_noise) {
      // Noise-based PLC/CNG. The spectrum is built in the output buffer,
      // which de-emphasis overwrites afterwards: this keeps 3.75 KB off the
      // stack.
      float* X = assume_aligned<output_alignment>(scratch);
      constexpr int end = end_;
      constexpr int eff_end = end_;
      for (int c = 0; c < C; c++)
         move_down_aligned<64>(decode_mem_[c], decode_mem_[c] + N, decode_buffer_size - N + overlap);
      if (s_.prefilter_and_fold) prefilter_and_fold();

      // Energy decay (bands start..end of both channels; the padding lanes
      // are updated too, harmlessly).
      static_assert(start_ == 0 && end == nb_ebands);
      const f32x4 decay = f32x4::broadcast(loss_duration == 0 ? 1.5f : .5f);
      for (int i = 0; i < 2 * band_stride; i += 4) {
         const f32x4 e = f32x4::load_aligned(old_band_e_ + i) - decay;
         const f32x4 bg = f32x4::load_aligned(background_log_e_ + i);
         vmax(bg, e).store_aligned(old_band_e_ + i);
      }
      std::uint32_t seed = s_.rng;
      for (int c = 0; c < C; c++) {
         for (int i = start_; i < eff_end; i++) {
            const int boffs = N * c + (ebands[i] << lm);
            const int blen = (ebands[i + 1] - ebands[i]) << lm;
            seed = lcg_noise(X + boffs, blen, seed);
            renormalise_vector(X + boffs, blen, 1.f);
         }
      }
      s_.rng = seed;

      synthesis(X, start_, eff_end, C, 0, 0);

      // libopus runs the post-filter with the last parameters here; their
      // gains are always zero in supported streams, so it does nothing.

      s_.prefilter_and_fold = 0;
      // Skip regular PLC until we get two consecutive packets.
      s_.plc_enabled = 0;
   } else {
      // Pitch-based PLC. The pitch search and the extrapolation run as separate
      // out-of-line steps, so their scratch (about 9 KB each) never shares a
      // stack frame, like libopus's variable-length arrays.
      float fade = 1.f;
      int pitch_index;
      if (s_.last_frame_type != frame_plc_periodic) {
         s_.last_pitch_index = pitch_index = plc_pitch_search();
      } else {
         pitch_index = s_.last_pitch_index;
         fade = .8f;
      }

      conceal_periodic(pitch_index, fade);
      s_.prefilter_and_fold = 1;
   }

   // Saturate to something large to avoid wrap-around.
   s_.loss_duration = min(10000, loss_duration + (1 << lm));
   s_.plc_duration = min(10000, s_.plc_duration + (1 << lm));
   s_.last_frame_type = curr_frame_type;
}

// Kept out of line, like conceal() (see there).
OPUSPP_NOINLINE inline int CeltDecoder::decode(RangeDecoder& dec, int len, float* OPUSPP_RESTRICT pcm) noexcept {
   static constexpr unsigned char trim_icdf[11] = {126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};
   static constexpr unsigned char spread_icdf[4] = {25, 23, 2, 0};
   static constexpr signed char tf_select_table[8] = {0, -2, 0, -3, 2, 0, 1, -1};  // LM = 2

   const int C = stream_channels_;
   constexpr int start = start_;
   constexpr int end = end_;
   constexpr int eff_end = end_;

   // Check if there are at least two packets received consecutively before
   // turning on the pitch-based PLC.
   if (s_.loss_duration == 0) s_.plc_enabled = 1;

   if (C == 1)
      for (int i = 0; i < band_stride; i += 4)
         vmax(f32x4::load_aligned(old_band_e_ + i), f32x4::load_aligned(old_band_e_ + band_stride + i))
            .store_aligned(old_band_e_ + i);

   std::int32_t total_bits = len * 8;
   std::int32_t tell = dec.tell();
   int silence;
   if (tell >= total_bits)
      silence = 1;
   else if (tell == 1)
      silence = dec.bit_logp(15);
   else
      silence = 0;
   if (silence) {
      // Pretend we've read all the remaining bits.
      tell = len * 8;
      dec.skip_to_end(tell);
   }

   // Post-filter flag: always 0, as Decoder::decode rejects packets that use
   // the post-filter before any state changes (uses_postfilter()).
   if (start == 0 && tell + 16 <= total_bits) {
      [[maybe_unused]] const int postfilter = dec.bit_logp(1);
      OPUSPP_ASSERT(!postfilter);
      tell = dec.tell();
   }

   int is_transient = 0;
   if (tell + 3 <= total_bits) {
      is_transient = dec.bit_logp(3);
      tell = dec.tell();
   }
   const int short_blocks = is_transient ? M : 0;

   // Decode the global flags (first symbols in the stream).
   const int intra_ener = tell + 3 <= total_bits ? dec.bit_logp(3) : 0;
   // If recovering from packet loss, make sure we make the energy prediction
   // safe to reduce the risk of getting loud artifacts.
   if (!intra_ener && s_.loss_duration != 0) {
      for (int c = 0; c < 2; c++) {
         const int missing = min(10, s_.loss_duration >> lm);
         for (int i = start; i < end; i++) {
            float& e0 = old_band_e_[c * band_stride + i];
            const float e1 = old_log_e_[c * band_stride + i];
            const float e2 = old_log_e2_[c * band_stride + i];
            if (e0 < max(e1, e2)) {
               // If energy is going down already, continue the trend.
               float E0 = e0;
               float slope = max(e1 - E0, .5f * (e2 - E0));
               slope = min(slope, 2.f);
               E0 -= max(0.f, float(1 + missing) * slope);
               e0 = max(-20.f, E0);
            } else {
               // Otherwise take the min of the last frames.
               e0 = min(min(e0, e1), e2);
            }
            // (No extra safety margin for LM = 2.)
         }
      }
   }

   // Get band energies.
   unquant_coarse_energy(start, end, old_band_e_, intra_ener, dec, C);

   // tf_decode()
   alignas(buffer_alignment) int tf_res[band_stride];
   {
      std::uint32_t budget = dec.storage() * 8;
      std::uint32_t ttell = std::uint32_t(dec.tell());
      int logp = is_transient ? 2 : 4;
      const int tf_select_rsv = ttell + unsigned(logp) + 1 <= budget;
      budget -= unsigned(tf_select_rsv);
      int tf_changed = 0, curr = 0;
      for (int i = start; i < end; i++) {
         if (ttell + unsigned(logp) <= budget) {
            curr ^= dec.bit_logp(unsigned(logp));
            ttell = std::uint32_t(dec.tell());
            tf_changed |= curr;
         }
         tf_res[i] = curr;
         logp = is_transient ? 4 : 5;
      }
      int tf_select = 0;
      if (tf_select_rsv && tf_select_table[4 * is_transient + 0 + tf_changed] !=
                              tf_select_table[4 * is_transient + 2 + tf_changed])
         tf_select = dec.bit_logp(1);
      for (int i = start; i < end; i++) tf_res[i] = tf_select_table[4 * is_transient + 2 * tf_select + tf_res[i]];
   }

   tell = dec.tell();
   int spread_decision = spread_normal;
   if (tell + 4 <= total_bits) spread_decision = dec.icdf(spread_icdf, 5);

   alignas(buffer_alignment) int cap[band_stride];
   init_caps(cap, C);

   alignas(buffer_alignment) int offsets[band_stride];
   fill(offsets, 0, nb_ebands);
   int dynalloc_logp = 6;
   total_bits <<= bitres;
   tell = std::int32_t(dec.tell_frac());
   for (int i = start; i < end; i++) {
      const int width = C * (ebands[i + 1] - ebands[i]) << lm;
      // quanta is 6 bits, but no more than 1 bit/sample and no less than 1/8 bit/sample.
      const int quanta = min(width << bitres, max(6 << bitres, width));
      int dynalloc_loop_logp = dynalloc_logp;
      int boost = 0;
      while (tell + (dynalloc_loop_logp << bitres) < total_bits && boost < cap[i]) {
         const int flag = dec.bit_logp(unsigned(dynalloc_loop_logp));
         tell = std::int32_t(dec.tell_frac());
         if (!flag) break;
         boost += quanta;
         total_bits -= quanta;
         dynalloc_loop_logp = 1;
      }
      offsets[i] = boost;
      // Making dynalloc more likely.
      if (boost > 0) dynalloc_logp = max(2, dynalloc_logp - 1);
   }

   const int alloc_trim = tell + (6 << bitres) <= total_bits ? dec.icdf(trim_icdf, 7) : 5;

   std::int32_t bits = ((std::int32_t(len) * 8) << bitres) - std::int32_t(dec.tell_frac()) - 1;
   const int anti_collapse_rsv = is_transient && bits >= ((lm + 2) << bitres) ? (1 << bitres) : 0;
   bits -= anti_collapse_rsv;

   alignas(buffer_alignment) int pulses[band_stride];
   alignas(buffer_alignment) int fine_quant[band_stride];
   alignas(buffer_alignment) int fine_priority[band_stride];
   const Allocation alloc =
      compute_allocation(start, end, offsets, cap, alloc_trim, bits, pulses, fine_quant, fine_priority, C, dec);

   unquant_fine_energy(start, end, old_band_e_, fine_quant, dec, C);

   for (int c = 0; c < channels; c++)
      move_down_aligned<64>(decode_mem_[c], decode_mem_[c] + N, decode_buffer_size - N + overlap);

   // Decode fixed codebook. The spectrum is decoded into the output buffer,
   // which is dead until de-emphasis writes the output at the end: this keeps
   // 3.75 KB off the stack.
   float* X = assume_aligned<output_alignment>(pcm);
   alignas(buffer_alignment) unsigned char collapse_masks[2 * band_stride];
   quant_all_bands(start, end, X, C == 2 ? X + N : nullptr, collapse_masks, pulses, short_blocks, spread_decision,
                   alloc.dual_stereo, alloc.intensity, tf_res, len * (8 << bitres) - anti_collapse_rsv,
                   alloc.balance, dec, alloc.coded_bands, &s_.rng, disable_inv_);

   int anti_collapse_on = 0;
   if (anti_collapse_rsv > 0) anti_collapse_on = int(dec.bits(1));
   unquant_energy_finalise(start, end, old_band_e_, fine_quant, fine_priority, len * 8 - dec.tell(), dec, C);
   if (anti_collapse_on)
      anti_collapse(X, collapse_masks, C, N, start, end, old_band_e_, old_log_e_, old_log_e2_, pulses, s_.rng);

   if (silence)
      for (int c = 0; c < C; c++) fill_aligned(old_band_e_ + c * band_stride, -28.f, band_stride);
   if (s_.prefilter_and_fold) prefilter_and_fold();
   synthesis(X, start, eff_end, C, is_transient, silence);

   if (C == 1) copy_aligned(old_band_e_ + band_stride, old_band_e_, band_stride);

   if (!is_transient) {
      copy_aligned<64>(old_log_e2_, old_log_e_, 2 * band_stride);
      copy_aligned<64>(old_log_e_, old_band_e_, 2 * band_stride);
   } else {
      for (int i = 0; i < 2 * band_stride; i += 4)
         vmin(f32x4::load_aligned(old_log_e_ + i), f32x4::load_aligned(old_band_e_ + i)).store_aligned(old_log_e_ + i);
   }
   // In normal circumstances, we only allow the noise floor to increase by
   // up to 2.4 dB/second, but when we're in DTX we give the weight of all
   // missing packets to the update packet.
   const float max_background_increase = float(min(160, s_.loss_duration + M)) * 0.001f;
   const f32x4 inc = f32x4::broadcast(max_background_increase);
   for (int i = 0; i < 2 * band_stride; i += 4)
      vmin(f32x4::load_aligned(background_log_e_ + i) + inc, f32x4::load_aligned(old_band_e_ + i))
         .store_aligned(background_log_e_ + i);
   // libopus resets the energies outside [start, end) here; with a fixed
   // fullband CELT-only stream that range is empty.
   static_assert(start == 0 && end == nb_ebands);
   s_.rng = dec.range();

   deemphasis(pcm);
   s_.loss_duration = 0;
   s_.plc_duration = 0;
   s_.last_frame_type = frame_normal;
   s_.prefilter_and_fold = 0;
   if (dec.tell() > 8 * len) return internal_error;
   return frame_size;
}

} // namespace opuspp::detail
