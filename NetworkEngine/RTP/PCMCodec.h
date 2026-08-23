//
// PCMCodec.h
// AES67 macOS Driver
// float <-> AES67 linear PCM (L16 / L24, big-endian) conversion, and
// interleave/deinterleave, as free functions.
//
// Pulled out of RTPTransmitter/RTPReceiver so the per-sample DSP can be (a)
// unit-tested on its own — it's the real-time hot path and had no direct
// test before — and (b) implemented with the platform's own vectorised
// routines (Accelerate/vDSP, and cblas strided copy for interleave) rather
// than hand-written per-sample loops. The vDSP path is compiled in on Apple
// platforms; a portable scalar fallback keeps the same results everywhere
// and is what the tests pin.
//
// Semantics are exactly those of the loops these replaced, including the
// deliberate encode/decode scale asymmetry (encode multiplies by the max
// positive code 2^n-1, decode divides by the full-scale 2^n) — pinned by
// TestPCMCodec so the vDSP and scalar paths must agree bit-for-bit.
//
#pragma once

#include <cstddef>
#include <cstdint>

namespace AES67 {

/// Interleaved float [-1,1] -> L16 big-endian. `payload` must hold
/// totalSamples*2 bytes. totalSamples = frames * channels.
void encodeL16BE(const float* audio, size_t totalSamples, uint8_t* payload);

/// Interleaved float [-1,1] -> L24 big-endian (3 bytes/sample).
/// `payload` must hold totalSamples*3 bytes.
void encodeL24BE(const float* audio, size_t totalSamples, uint8_t* payload);

/// L16 big-endian -> interleaved float. `audio` must hold totalSamples.
void decodeL16BE(const uint8_t* payload, size_t totalSamples, float* audio);

/// L24 big-endian -> interleaved float. `audio` must hold totalSamples.
void decodeL24BE(const uint8_t* payload, size_t totalSamples, float* audio);

/// Copy `frames` samples of one channel into an interleaved buffer of
/// `channels` channels, at channel index `channel`. Strided store —
/// dst[frame*channels + channel] = src[frame].
void interleaveChannel(const float* src, float* dst, size_t frames,
                       size_t channels, size_t channel);

/// The inverse: dst[frame] = src[frame*channels + channel].
void deinterleaveChannel(const float* src, float* dst, size_t frames,
                         size_t channels, size_t channel);

} // namespace AES67
