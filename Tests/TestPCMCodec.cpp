//
// TestPCMCodec.cpp
// AES67 macOS Driver
// Pins the float<->PCM conversion behaviour so the vDSP/Accelerate path and
// the scalar fallback must agree with an independent, hand-computed spec —
// the real-time encode/decode had no direct test before this. Runs in the
// standard suite (pure arithmetic, no sockets).
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/RTP/PCMCodec.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace AES67;



namespace {

// Expected big-endian bytes for a float encoded to L16, computed the same
// way the spec says: clamp, * (2^15 - 1), truncate toward zero.
void expectL16(float v, uint8_t& hi, uint8_t& lo) {
    float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    int16_t s = static_cast<int16_t>(c * 32767.0f);
    hi = (s >> 8) & 0xFF;
    lo = s & 0xFF;
}

} // namespace

TEST_CASE("L16 Known Vectors") {
    std::cout << "Test: L16 encode matches the hand-computed big-endian bytes... ";
    const std::vector<float> in = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f /*clamps*/, -2.0f /*clamps*/};
    std::vector<uint8_t> out(in.size() * 2);
    encodeL16BE(in.data(), in.size(), out.data());
    for (size_t i = 0; i < in.size(); ++i) {
        uint8_t hi, lo;
        expectL16(in[i], hi, lo);
        CHECK((out[i * 2] == hi && out[i * 2 + 1] == lo));
    }
    // Full-scale positive clamps to 0x7FFF; full-scale negative to -32767 = 0x8001.
    CHECK((out[2] == 0x7F && out[3] == 0xFF));
    CHECK((out[4] == 0x80 && out[5] == 0x01));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("L16 Round Trip") {
    std::cout << "Test: L16 encode->decode is near-identity within one LSB... ";
    std::vector<float> in;
    for (int i = -100; i <= 100; ++i) in.push_back(i / 100.0f);
    std::vector<uint8_t> pcm(in.size() * 2);
    std::vector<float> out(in.size());
    encodeL16BE(in.data(), in.size(), pcm.data());
    decodeL16BE(pcm.data(), in.size(), out.data());
    for (size_t i = 0; i < in.size(); ++i) {
        // One 16-bit LSB is ~3e-5; encode/decode scale asymmetry adds a hair.
        CHECK(std::fabs(out[i] - in[i]) < 1.0e-4f);
    }
    std::cout << "PASS" << std::endl;
}

TEST_CASE("L24 Known Vectors") {
    std::cout << "Test: L24 encode matches the hand-computed big-endian bytes... ";
    const std::vector<float> in = {0.0f, 1.0f, -1.0f, 0.25f};
    std::vector<uint8_t> out(in.size() * 3);
    encodeL24BE(in.data(), in.size(), out.data());
    // 0.0 -> 0x000000
    CHECK((out[0] == 0 && out[1] == 0 && out[2] == 0));
    // +1.0 -> 8388607 = 0x7FFFFF
    CHECK((out[3] == 0x7F && out[4] == 0xFF && out[5] == 0xFF));
    // -1.0 -> -8388607 = 0x800001
    CHECK((out[6] == 0x80 && out[7] == 0x00 && out[8] == 0x01));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("L24 Round Trip") {
    std::cout << "Test: L24 encode->decode is near-identity... ";
    std::vector<float> in;
    for (int i = -1000; i <= 1000; ++i) in.push_back(i / 1000.0f);
    std::vector<uint8_t> pcm(in.size() * 3);
    std::vector<float> out(in.size());
    encodeL24BE(in.data(), in.size(), pcm.data());
    decodeL24BE(pcm.data(), in.size(), out.data());
    for (size_t i = 0; i < in.size(); ++i) {
        CHECK(std::fabs(out[i] - in[i]) < 1.0e-6f);
    }
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Decode Known Bytes") {
    std::cout << "Test: decode of known big-endian bytes gives the right float... ";
    // 0x4000 = 16384 -> 16384/32768 = 0.5
    uint8_t l16[2] = {0x40, 0x00};
    float f16 = 0;
    decodeL16BE(l16, 1, &f16);
    CHECK(std::fabs(f16 - 0.5f) < 1e-6f);
    // 0x400000 = 4194304 -> /8388608 = 0.5
    uint8_t l24[3] = {0x40, 0x00, 0x00};
    float f24 = 0;
    decodeL24BE(l24, 1, &f24);
    CHECK(std::fabs(f24 - 0.5f) < 1e-6f);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Interleave Round Trip") {
    std::cout << "Test: interleave then deinterleave restores each channel... ";
    const size_t frames = 5, channels = 3;
    std::vector<float> interleaved(frames * channels, 0.0f);
    // Fill channel c with c*10 + frame.
    for (size_t c = 0; c < channels; ++c) {
        std::vector<float> ch(frames);
        for (size_t f = 0; f < frames; ++f) ch[f] = static_cast<float>(c * 10 + f);
        interleaveChannel(ch.data(), interleaved.data(), frames, channels, c);
    }
    // Verify layout: interleaved[f*channels + c] == c*10 + f.
    for (size_t f = 0; f < frames; ++f)
        for (size_t c = 0; c < channels; ++c)
            CHECK(interleaved[f * channels + c] == static_cast<float>(c * 10 + f));
    // Deinterleave back and compare.
    for (size_t c = 0; c < channels; ++c) {
        std::vector<float> ch(frames);
        deinterleaveChannel(interleaved.data(), ch.data(), frames, channels, c);
        for (size_t f = 0; f < frames; ++f)
            CHECK(ch[f] == static_cast<float>(c * 10 + f));
    }
    std::cout << "PASS" << std::endl;
}

