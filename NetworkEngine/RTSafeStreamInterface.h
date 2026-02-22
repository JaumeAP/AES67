//
// RTSafeStreamInterface.h
// AES67 macOS Driver
// Compile-time RT-safe boundary for audio IO thread access
//
// ============================================================================
// RT-SAFE BOUNDARY
// ============================================================================
//
// This struct is the ONLY interface the Core Audio IO thread (AES67IOHandler)
// should use to access shared audio data. It holds exclusively lock-free,
// wait-free data structures:
//
//   - Pointers to per-channel SPSC ring buffers (lock-free by design)
//   - Atomic counters for underrun/overrun statistics
//   - Atomic status flags
//
// RULES:
//   1. This struct must NEVER hold a reference or pointer to StreamManager.
//   2. All methods must be noexcept and lock-free.
//   3. No method may allocate, lock, or perform blocking syscalls.
//   4. AES67IOHandler must access audio data ONLY through this interface.
//
// StreamManager, RTPReceiver, and RTPTransmitter access the same underlying
// ring buffers through their own (non-RT) paths. The buffers themselves are
// SPSC lock-free and safe for concurrent access from one producer and one
// consumer thread.
//
// ============================================================================

#pragma once

#include "../Shared/RingBuffer.hpp"
#include <array>
#include <atomic>
#include <cstdint>

namespace AES67 {

// ============================================================================
// RT-Safe Stream Interface
// ============================================================================
//
// Lightweight, non-owning view into the lock-free audio data shared between
// the Core Audio IO thread and the network engine. Created by AES67Device
// during initialization and passed to AES67IOHandler.
//
// LIFETIME: The referenced buffers and atomics must outlive this struct.
// AES67Device owns the underlying storage and guarantees this.
//
struct RTSafeStreamInterface {
    static constexpr size_t kNumChannels = 128;

    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, kNumChannels>;

    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    // Construct from device-owned buffers and atomic counters.
    // All parameters are non-owning references -- caller must ensure they
    // outlive this interface.
    RTSafeStreamInterface(
        DeviceChannelBuffers& inputBuffers,
        DeviceChannelBuffers& outputBuffers,
        std::atomic<uint64_t>& inputUnderruns,
        std::atomic<uint64_t>& outputUnderruns,
        std::atomic<bool>& ioRunning
    ) noexcept
        : inputBuffers_(inputBuffers)
        , outputBuffers_(outputBuffers)
        , inputUnderruns_(inputUnderruns)
        , outputUnderruns_(outputUnderruns)
        , ioRunning_(ioRunning)
    {}

    // Non-copyable, non-movable (references cannot be reseated)
    RTSafeStreamInterface(const RTSafeStreamInterface&) = delete;
    RTSafeStreamInterface& operator=(const RTSafeStreamInterface&) = delete;
    RTSafeStreamInterface(RTSafeStreamInterface&&) = delete;
    RTSafeStreamInterface& operator=(RTSafeStreamInterface&&) = delete;

    // -----------------------------------------------------------------------
    // Ring Buffer Access (RT-SAFE)
    // -----------------------------------------------------------------------

    // Input buffers: Network writes, Core Audio reads.
    // Each element is one channel's SPSC ring buffer.
    DeviceChannelBuffers& inputBuffers() noexcept { return inputBuffers_; }

    // Output buffers: Core Audio writes, Network reads.
    DeviceChannelBuffers& outputBuffers() noexcept { return outputBuffers_; }

    // -----------------------------------------------------------------------
    // Statistics (RT-SAFE, lock-free atomics)
    // -----------------------------------------------------------------------

    // Increment input underrun counter (call when ring buffer read returns
    // fewer samples than requested).
    void recordInputUnderrun() noexcept {
        inputUnderruns_.fetch_add(1, std::memory_order_relaxed);
    }

    // Increment output overrun counter (call when ring buffer write returns
    // fewer samples than provided).
    void recordOutputOverrun() noexcept {
        outputUnderruns_.fetch_add(1, std::memory_order_relaxed);
    }

    // Read current underrun/overrun counts (for diagnostics from any thread).
    uint64_t getInputUnderrunCount() const noexcept {
        return inputUnderruns_.load(std::memory_order_relaxed);
    }

    uint64_t getOutputOverrunCount() const noexcept {
        return outputUnderruns_.load(std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // IO State (RT-SAFE, lock-free atomic)
    // -----------------------------------------------------------------------

    // Check whether Core Audio IO is currently active.
    bool isIORunning() const noexcept {
        return ioRunning_.load(std::memory_order_relaxed);
    }

private:
    // Non-owning references to device-owned data.
    // All are lock-free / wait-free and safe for RT access.
    DeviceChannelBuffers& inputBuffers_;
    DeviceChannelBuffers& outputBuffers_;
    std::atomic<uint64_t>& inputUnderruns_;
    std::atomic<uint64_t>& outputUnderruns_;
    std::atomic<bool>& ioRunning_;
};

} // namespace AES67
