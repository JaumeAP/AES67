//
// TestIOHandler.cpp
// AES67 macOS Driver - Tests
//
// The two callbacks Core Audio makes on the real-time thread, driven
// directly: AES67IOHandler::OnReadClientInput and OnWriteClientOutput.
//
// This file was the least covered in the package -- 8.7% of lines, no branch
// taken at all -- and it is the one place in the driver that runs inside
// coreaudiod's deadline. What was tested of it was a benchmark that is not a
// CTest, measuring how long it takes rather than what it does.
//
// The guard these cases exist for: RTSafeStreamInterface::isIORunning() is
// how AES67Device says the ring buffers behind the interface are still its
// own. It is cleared first thing in ~AES67Device, and a cycle arriving after
// that would otherwise read freed memory on the real-time thread. Nothing
// read the flag until 2026-09-15, and nothing but this asserts that it is.
//
// No Core Audio object is needed: both callbacks take the client and the
// stream as shared pointers and touch neither -- the format they need was
// cached at construction, precisely so the real-time path makes no virtual
// call -- so a null pointer for each is what the test hands them.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Driver/AES67IOHandler.h"
#include "NetworkEngine/RTSafeStreamInterface.h"
#include "Shared/RingBuffer.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <utility>
#include <vector>

using namespace AES67;

namespace {

constexpr size_t kChannels = 128;
constexpr UInt32 kFrames = 64;
constexpr UInt32 kTestChannels = 2;

template <size_t... Is>
auto makeRingBuffers(size_t size, std::index_sequence<Is...>) {
    return std::array<SPSCRingBuffer<float>, sizeof...(Is)>{
        ((void)Is, SPSCRingBuffer<float>(size))...};
}

/// Everything AES67Device owns on behalf of the IO thread, minus the device.
struct HandlerFixture {
    std::array<SPSCRingBuffer<float>, kChannels> input =
        makeRingBuffers(512, std::make_index_sequence<kChannels>{});
    std::array<SPSCRingBuffer<float>, kChannels> output =
        makeRingBuffers(512, std::make_index_sequence<kChannels>{});
    std::atomic<uint64_t> inputUnderruns{0};
    std::atomic<uint64_t> outputUnderruns{0};
    std::atomic<bool> ioRunning{true};
    RTSafeStreamInterface interface{input, output, inputUnderruns, outputUnderruns, ioRunning};
    AES67IOHandler handler{interface};

    /// The read callback, with the client and stream the real one ignores.
    void read(void* bytes, UInt32 bytesCount) {
        handler.OnReadClientInput(nullptr, nullptr, 0.0, 0.0, bytes, bytesCount);
    }

    void write(const Float32* frames, UInt32 frameCount, UInt32 channelCount) {
        handler.OnWriteClientOutput(nullptr, nullptr, 0.0, 0.0, frames, frameCount, channelCount);
    }
};

}  // namespace

TEST_CASE("What the network put in the buffers is what Core Audio reads") {
    HandlerFixture fixture;

    // One ramp per channel, the way a receiver fills them.
    for (UInt32 channel = 0; channel < kTestChannels; ++channel) {
        std::vector<float> samples(kFrames);
        for (UInt32 frame = 0; frame < kFrames; ++frame) {
            samples[frame] = static_cast<float>(channel + 1) + static_cast<float>(frame) / 1000.0f;
        }
        REQUIRE(fixture.input[channel].write(samples.data(), kFrames) == kFrames);
    }

    // The read path interleaves at the device's own width -- 128 channels,
    // cached at construction so the real-time thread asks no object for it --
    // so the buffer is frames x 128, whatever the network filled.
    std::vector<float> destination(kFrames * kChannels, -1.0f);
    fixture.read(destination.data(), static_cast<UInt32>(destination.size() * sizeof(float)));

    // Frame 0, then frame 1, each carrying every channel in order.
    CHECK(destination[0] == doctest::Approx(1.0f));
    CHECK(destination[1] == doctest::Approx(2.0f));
    CHECK(destination[2] == doctest::Approx(0.0f));            // nothing wrote channel 2
    CHECK(destination[kChannels] == doctest::Approx(1.001f));
    CHECK(destination[kChannels + 1] == doctest::Approx(2.001f));
}

TEST_CASE("A device that is going away hands back silence, not freed memory") {
    HandlerFixture fixture;

    for (UInt32 channel = 0; channel < kTestChannels; ++channel) {
        std::vector<float> samples(kFrames, 0.5f);
        REQUIRE(fixture.input[channel].write(samples.data(), kFrames) == kFrames);
    }

    // What ~AES67Device does before it destroys anything.
    fixture.ioRunning.store(false);

    std::vector<float> destination(kFrames * kChannels, 7.0f);
    fixture.read(destination.data(), static_cast<UInt32>(destination.size() * sizeof(float)));

    for (float sample : destination) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("Core Audio's output reaches the buffers the transmitters read") {
    HandlerFixture fixture;

    std::vector<Float32> frames(kFrames * kTestChannels);
    for (UInt32 frame = 0; frame < kFrames; ++frame) {
        frames[frame * kTestChannels] = 0.25f;
        frames[frame * kTestChannels + 1] = -0.25f;
    }
    fixture.write(frames.data(), kFrames, kTestChannels);

    std::vector<float> readBack(kFrames, 0.0f);
    CHECK(fixture.output[0].read(readBack.data(), kFrames) == kFrames);
    CHECK(readBack[0] == doctest::Approx(0.25f));
    CHECK(fixture.output[1].read(readBack.data(), kFrames) == kFrames);
    CHECK(readBack[0] == doctest::Approx(-0.25f));
}

TEST_CASE("Output during teardown is dropped rather than written") {
    HandlerFixture fixture;
    fixture.ioRunning.store(false);

    std::vector<Float32> frames(kFrames * kTestChannels, 0.5f);
    fixture.write(frames.data(), kFrames, kTestChannels);

    std::vector<float> readBack(kFrames, 0.0f);
    CHECK(fixture.output[0].read(readBack.data(), kFrames) == 0);
}

TEST_CASE("A channel count this device does not have is refused") {
    HandlerFixture fixture;

    std::vector<Float32> frames(kFrames * 4, 0.5f);
    // Zero, and more than the device has: both are a caller that has lost
    // track of the format, and writing either would run off the array of
    // buffers.
    fixture.write(frames.data(), kFrames, 0);
    fixture.write(frames.data(), kFrames, kChannels + 1);

    std::vector<float> readBack(kFrames, 0.0f);
    CHECK(fixture.output[0].read(readBack.data(), kFrames) == 0);
}

TEST_CASE("A read of no bytes asks nothing of the buffers") {
    HandlerFixture fixture;
    std::vector<float> destination(kFrames * kChannels, 3.0f);

    fixture.read(destination.data(), 0);
    // Untouched: a zero-length request is not a request for silence.
    CHECK(destination[0] == doctest::Approx(3.0f));

    fixture.read(nullptr, 128);   // and a null destination is not a crash
}
