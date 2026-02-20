//
// AES67IOHandler.h
// AES67 macOS Driver - Build #1
// Real-time safe audio I/O handler for Core Audio
// NO ALLOCATION, NO LOCKS, NO BLOCKING
//

#pragma once

#include "../Shared/Types.h"
#include "../Shared/RingBuffer.hpp"
#include <aspl/IORequestHandler.hpp>
#include <aspl/Client.hpp>
#include <aspl/Stream.hpp>
#include <memory>
#include <array>
#include <atomic>

class IOHandlerBenchmark;

namespace AES67 {

//
// AES67 IO Handler
//
// Handles real-time audio I/O between Core Audio and ring buffers
// Called from Core Audio's real-time thread - MUST be RT-safe!
//
// RT-SAFE REQUIREMENTS:
// - NO memory allocation (malloc/new/delete)
// - NO locks (mutexes, semaphores)
// - NO blocking operations
// - NO system calls that can block
// - NO Objective-C message sends
// - Bounded execution time
//
class AES67IOHandler : public aspl::IORequestHandler {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;

    //
    // Constructor
    //
    AES67IOHandler(
        DeviceChannelBuffers& inputBuffers,
        DeviceChannelBuffers& outputBuffers,
        std::atomic<uint64_t>& inputUnderruns,
        std::atomic<uint64_t>& outputUnderruns
    );

    ~AES67IOHandler() override;

    //
    // aspl::IORequestHandler overrides (RT-SAFE!)
    //

    // Called when Core Audio needs input data from device
    // Reads from inputBuffers_ and provides raw bytes to Core Audio
    void OnReadClientInput(
        const std::shared_ptr<aspl::Client>& client,
        const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        void* bytes,
        UInt32 bytesCount
    ) override;

    // Called when Core Audio has output data for device
    // Writes Float32 frames to outputBuffers_
    void OnWriteClientOutput(
        const std::shared_ptr<aspl::Client>& client,
        const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        const Float32* frames,
        UInt32 frameCount,
        UInt32 channelCount
    ) override;

private:
    // Process input stream (Network → Core Audio)
    // RT-SAFE: Reads from ring buffers, fills silence on underrun
    // Uses batch processing for optimal performance
    void processInput(float* outputData, UInt32 frameCount, UInt32 channelCount) noexcept;

    // Process output stream (Core Audio → Network)
    // RT-SAFE: Writes to ring buffers, discards on overrun
    // Uses batch processing for optimal performance
    void processOutput(const float* inputData, UInt32 frameCount, UInt32 channelCount) noexcept;

    // Ring buffer references
    // Input: Network writes, Core Audio reads
    // Output: Core Audio writes, Network reads
    DeviceChannelBuffers& inputBuffers_;
    DeviceChannelBuffers& outputBuffers_;

    // Statistics references
    std::atomic<uint64_t>& inputUnderruns_;
    std::atomic<uint64_t>& outputUnderruns_;

    // Constants
    static constexpr size_t kNumChannels = 128;

    // Allow benchmark direct access to processInput/processOutput
    friend class ::IOHandlerBenchmark;
};

} // namespace AES67
