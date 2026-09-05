//
// TestDoPDecoder.cpp
// DSD-over-PCM framing, which had no tests.
//
// Pure static functions over byte buffers: nothing to mock, nothing timing
// dependent, and it sits in the core, so every consumer inherits it. The
// coverage run had it at 0%.
//
// What matters about DoP is the marker: a receiver tells DoP from ordinary PCM
// by 0x05 and 0xFA alternating in the top byte of consecutive 24-bit samples.
// Get that wrong and a DSD stream is played as noise at full scale, which is
// why the marker checks here are exact rather than approximate.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/DoPDecoder.h"

#include <cstdint>
#include <vector>

using AES67::DoPDecoder;

namespace {

/// DoP carries two DSD bytes per 24-bit sample, so a frame count of N needs
/// 3N bytes of DoP and 2N of DSD.
std::vector<uint8_t> encodeFrames(const std::vector<uint8_t>& dsd) {
    const size_t frames = dsd.size() / 2;
    std::vector<uint8_t> dop(frames * 3, 0);
    DoPDecoder::encode(dsd.data(), frames, dop.data());
    return dop;
}

}  // namespace

TEST_CASE("Markers alternate 0x05, 0xFA across frames") {
    const std::vector<uint8_t> dsd = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const auto dop = encodeFrames(dsd);
    REQUIRE(dop.size() == 12);

    CHECK(dop[0] == 0x05);
    CHECK(dop[3] == 0xFA);
    CHECK(dop[6] == 0x05);
    CHECK(dop[9] == 0xFA);
}

TEST_CASE("The two DSD bytes ride in the low two bytes, in order") {
    const std::vector<uint8_t> dsd = {0xAB, 0xCD};
    const auto dop = encodeFrames(dsd);
    REQUIRE(dop.size() == 3);

    CHECK(dop[1] == 0xAB);
    CHECK(dop[2] == 0xCD);
}

TEST_CASE("Encoding and decoding round-trips the DSD payload") {
    std::vector<uint8_t> dsd;
    for (int i = 0; i < 512; ++i) dsd.push_back(static_cast<uint8_t>(i * 7 + 3));

    const auto dop = encodeFrames(dsd);
    std::vector<uint8_t> back(dsd.size(), 0);
    DoPDecoder::decode(dop.data(), dsd.size() / 2, back.data());

    CHECK(back == dsd);
}

TEST_CASE("A DoP stream is recognised and ordinary PCM is not") {
    const std::vector<uint8_t> dsd = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const auto dop = encodeFrames(dsd);
    CHECK(DoPDecoder::isDoPStream(dop.data(), dop.size()));

    // Same length, no markers: audio that happens to be there.
    std::vector<uint8_t> pcm(dop.size(), 0x40);
    CHECK(DoPDecoder::isDoPStream(pcm.data(), pcm.size()) == false);
}

TEST_CASE("Validation rejects a broken marker sequence") {
    const std::vector<uint8_t> dsd = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    auto dop = encodeFrames(dsd);
    REQUIRE(DoPDecoder::validateDoPMarkers(dop.data(), dsd.size() / 2));

    // One marker wrong is enough: a receiver that tolerated this would be
    // tolerating a stream that has lost frame alignment.
    dop[6] = 0xFA;
    CHECK(DoPDecoder::validateDoPMarkers(dop.data(), dsd.size() / 2) == false);
}

TEST_CASE("DSD rates map to their DoP container rates, both ways") {
    // A DoP frame carries 16 DSD bits, so the container runs at one sixteenth
    // of the DSD rate: 2.8224 MHz becomes 176.4 kHz.
    CHECK(DoPDecoder::getDoPSampleRate(2822400) == 176400);
    CHECK(DoPDecoder::getDoPSampleRate(5644800) == 352800);
    CHECK(DoPDecoder::getDoPSampleRate(11289600) == 705600);

    CHECK(DoPDecoder::getDSDRate(176400) == 2822400);
    CHECK(DoPDecoder::getDSDRate(352800) == 5644800);
    CHECK(DoPDecoder::getDSDRate(705600) == 11289600);
}

TEST_CASE("Null buffers and zero frames are refused, not dereferenced") {
    std::vector<uint8_t> buf(12, 0);
    DoPDecoder::encode(nullptr, 4, buf.data());
    DoPDecoder::encode(buf.data(), 0, buf.data());
    DoPDecoder::decode(nullptr, 4, buf.data());
    DoPDecoder::decode(buf.data(), 4, nullptr);
    CHECK(DoPDecoder::isDoPStream(nullptr, 12) == false);
    CHECK(DoPDecoder::isDoPStream(buf.data(), 0) == false);
}
