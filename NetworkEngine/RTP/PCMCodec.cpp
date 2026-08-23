//
// PCMCodec.cpp
// AES67 macOS Driver
// See PCMCodec.h. Two implementations of the same functions, selected at
// compile time: an Accelerate/vDSP path on Apple platforms, and a scalar
// fallback. Both must produce identical results — TestPCMCodec checks it.
//
#include "PCMCodec.h"

#include <algorithm>
#include <cstring>

#if defined(__APPLE__)
// Use the modern (non-deprecated as of macOS 13.3) CBLAS interface.
#define ACCELERATE_NEW_LAPACK 1
#include <Accelerate/Accelerate.h>
#include <libkern/OSByteOrder.h>
#define AES67_HAVE_ACCELERATE 1
#endif

namespace AES67 {

namespace {
constexpr float kL16Scale = 32767.0f;    // 2^15 - 1, encode
constexpr float kL16Full  = 32768.0f;    // 2^15,     decode
constexpr float kL24Scale = 8388607.0f;  // 2^23 - 1, encode
constexpr float kL24Full  = 8388608.0f;  // 2^23,     decode
} // namespace

// ============================================================================
// L16
// ============================================================================

void encodeL16BE(const float* audio, size_t totalSamples, uint8_t* payload) {
    if (totalSamples == 0) return;

#if AES67_HAVE_ACCELERATE
    // Clip to [-1,1], scale, convert to int16 (truncating, matching the
    // scalar static_cast), all vectorised; then pack big-endian.
    std::vector<float> scaled(totalSamples);
    float lo = -1.0f, hi = 1.0f;
    vDSP_vclip(audio, 1, &lo, &hi, scaled.data(), 1, totalSamples);
    float scale = kL16Scale;
    vDSP_vsmul(scaled.data(), 1, &scale, scaled.data(), 1, totalSamples);

    std::vector<int16_t> host(totalSamples);
    vDSP_vfix16(scaled.data(), 1, host.data(), 1, totalSamples); // truncates toward zero

    for (size_t i = 0; i < totalSamples; ++i) {
        uint16_t be = OSSwapHostToBigInt16(static_cast<uint16_t>(host[i]));
        std::memcpy(payload + i * 2, &be, 2);
    }
#else
    for (size_t i = 0; i < totalSamples; ++i) {
        float v = std::max(-1.0f, std::min(1.0f, audio[i]));
        int16_t s = static_cast<int16_t>(v * kL16Scale);
        payload[i * 2 + 0] = (s >> 8) & 0xFF;
        payload[i * 2 + 1] = s & 0xFF;
    }
#endif
}

void decodeL16BE(const uint8_t* payload, size_t totalSamples, float* audio) {
    if (totalSamples == 0) return;

#if AES67_HAVE_ACCELERATE
    std::vector<int16_t> host(totalSamples);
    for (size_t i = 0; i < totalSamples; ++i) {
        uint16_t be;
        std::memcpy(&be, payload + i * 2, 2);
        host[i] = static_cast<int16_t>(OSSwapBigToHostInt16(be));
    }
    vDSP_vflt16(host.data(), 1, audio, 1, totalSamples); // int16 -> float
    float inv = 1.0f / kL16Full;
    vDSP_vsmul(audio, 1, &inv, audio, 1, totalSamples);
#else
    for (size_t i = 0; i < totalSamples; ++i) {
        uint16_t raw = (static_cast<uint16_t>(payload[i * 2]) << 8) | payload[i * 2 + 1];
        int16_t s = static_cast<int16_t>(raw);
        audio[i] = s / kL16Full;
    }
#endif
}

// ============================================================================
// L24 — no native 24-bit type, so the byte<->int32 packing is scalar either
// way; only the float<->int32 arithmetic vectorises.
// ============================================================================

void encodeL24BE(const float* audio, size_t totalSamples, uint8_t* payload) {
    if (totalSamples == 0) return;

#if AES67_HAVE_ACCELERATE
    std::vector<float> scaled(totalSamples);
    float lo = -1.0f, hi = 1.0f;
    vDSP_vclip(audio, 1, &lo, &hi, scaled.data(), 1, totalSamples);
    float scale = kL24Scale;
    vDSP_vsmul(scaled.data(), 1, &scale, scaled.data(), 1, totalSamples);

    std::vector<int32_t> host(totalSamples);
    vDSP_vfix32(scaled.data(), 1, host.data(), 1, totalSamples); // truncates toward zero

    for (size_t i = 0; i < totalSamples; ++i) {
        int32_t s = host[i];
        payload[i * 3 + 0] = (s >> 16) & 0xFF;
        payload[i * 3 + 1] = (s >> 8) & 0xFF;
        payload[i * 3 + 2] = s & 0xFF;
    }
#else
    for (size_t i = 0; i < totalSamples; ++i) {
        float v = std::max(-1.0f, std::min(1.0f, audio[i]));
        int32_t s = static_cast<int32_t>(v * kL24Scale);
        payload[i * 3 + 0] = (s >> 16) & 0xFF;
        payload[i * 3 + 1] = (s >> 8) & 0xFF;
        payload[i * 3 + 2] = s & 0xFF;
    }
#endif
}

void decodeL24BE(const uint8_t* payload, size_t totalSamples, float* audio) {
    if (totalSamples == 0) return;

#if AES67_HAVE_ACCELERATE
    std::vector<int32_t> host(totalSamples);
    for (size_t i = 0; i < totalSamples; ++i) {
        uint32_t raw = (static_cast<uint32_t>(payload[i * 3]) << 24) |
                       (static_cast<uint32_t>(payload[i * 3 + 1]) << 16) |
                       (static_cast<uint32_t>(payload[i * 3 + 2]) << 8);
        host[i] = static_cast<int32_t>(raw) >> 8; // arithmetic shift, sign-extends
    }
    vDSP_vflt32(host.data(), 1, audio, 1, totalSamples);
    float inv = 1.0f / kL24Full;
    vDSP_vsmul(audio, 1, &inv, audio, 1, totalSamples);
#else
    for (size_t i = 0; i < totalSamples; ++i) {
        uint32_t raw = (static_cast<uint32_t>(payload[i * 3]) << 24) |
                       (static_cast<uint32_t>(payload[i * 3 + 1]) << 16) |
                       (static_cast<uint32_t>(payload[i * 3 + 2]) << 8);
        int32_t s = static_cast<int32_t>(raw) >> 8;
        audio[i] = s / kL24Full;
    }
#endif
}

// ============================================================================
// Interleave — a strided copy, which cblas does natively.
// ============================================================================

// A strided copy is memory-bound, not arithmetic — the DSP win is in the
// float<->int conversions above, not here. cblas_scopy would do it, but its
// modern (non-deprecated) form needs macOS 13.3, above this driver's
// deployment target, so a plain strided loop is both simpler and portable.
void interleaveChannel(const float* src, float* dst, size_t frames,
                       size_t channels, size_t channel) {
    for (size_t f = 0; f < frames; ++f) {
        dst[f * channels + channel] = src[f];
    }
}

void deinterleaveChannel(const float* src, float* dst, size_t frames,
                         size_t channels, size_t channel) {
    for (size_t f = 0; f < frames; ++f) {
        dst[f] = src[f * channels + channel];
    }
}

} // namespace AES67
