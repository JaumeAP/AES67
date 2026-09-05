/// @file LockFreeCircularJitterBuffer.h
/// @brief Lock-free jitter buffer for RTP packet reordering between network and audio threads.

#ifndef LOCK_FREE_CIRCULAR_JITTER_BUFFER_H
#define LOCK_FREE_CIRCULAR_JITTER_BUFFER_H

#include "LockFreePacketPool.h"
#include <atomic>
#include <memory>
#include <cstdint>
#include <vector>

namespace AES67 {

/// Atomic state machine for lock-free slot management.
enum class SlotState : uint8_t {
    EMPTY = 0,    ///< Slot is available for writing
    WRITING = 1,  ///< Writer is currently filling the slot
    READY = 2,    ///< Data is ready for reading
    READING = 3   ///< Reader is currently consuming data
};

/// Slot holding one RTP packet with metadata for lock-free access.
struct LockFreeBufferPacket {
    uint8_t data[1500]; // Standard max Ethernet frame size
    std::atomic<size_t> length{0};
    std::atomic<uint32_t> sequenceNumber{0};
    std::atomic<uint64_t> presentationTime{0};
    std::atomic<uint64_t> arrivalTime{0};
    std::atomic<SlotState> state{SlotState::EMPTY};  // Single atomic state for lock-free coordination

    LockFreeBufferPacket() = default;
};

class LockFreeCircularJitterBuffer {
public:
    // Default buffer depth (power of 2 for efficient modulo via bitwise AND)
    static constexpr size_t DEFAULT_BUFFER_SIZE = 256;

    // Allowed range for configurable depth (before rounding to power of 2)
    static constexpr size_t MIN_BUFFER_SIZE = 32;
    static constexpr size_t MAX_BUFFER_SIZE = 4096;

    // Construct with configurable depth.
    // The depth is clamped to [MIN_BUFFER_SIZE, MAX_BUFFER_SIZE] and then
    // rounded up to the next power of 2 for efficient modulo operations.
    explicit LockFreeCircularJitterBuffer(size_t depth = DEFAULT_BUFFER_SIZE);
    ~LockFreeCircularJitterBuffer();

    // Add a packet to the buffer (should be called from network thread)
    bool addPacket(const uint8_t* packetData, size_t packetLength,
                   uint32_t sequenceNumber, uint64_t presentationTime);

    // Get the next packet that should be played based on sequence number
    // Should be called from audio thread
    bool getNextPacket(uint8_t* outputBuffer, size_t bufferSize,
                       size_t& outputLength, uint64_t& presentationTime,
                       uint32_t expectedSequenceNumber);

    // Get a packet by sequence number (for out-of-order packets)
    bool getPacketBySequence(uint8_t* outputBuffer, size_t bufferSize,
                             size_t& outputLength, uint64_t& presentationTime,
                             uint32_t sequenceNumber);

    // Get buffer statistics
    size_t getBufferedPacketCount() const;
    size_t getMaxBufferSize() const { return bufferSize_; }

    // Reset the buffer
    void reset();

    // Get reference to packet pool
    LockFreePacketPool& getPacketPool() { return packetPool_; }

private:
    // Round up to the next power of 2 (returns v unchanged if already a power of 2)
    static size_t nextPowerOf2(size_t v);

    // Actual buffer size after clamping and power-of-2 rounding (set once at construction)
    const size_t bufferSize_;

    // Bitmask for efficient modulo: bufferSize_ - 1  (set once at construction)
    const uint32_t sequenceMask_;

    // Circular buffer to hold packets indexed by sequence number
    // Dynamically allocated at construction, size is bufferSize_
    std::vector<LockFreeBufferPacket> buffer_;

    // Expected sequence number for the audio thread
    std::atomic<uint32_t> expectedSequenceNumber_{0};

    // Statistics
    std::atomic<size_t> totalPackets_{0};
    std::atomic<size_t> droppedPackets_{0};
    std::atomic<size_t> validPackets_{0};

    // Packet pool for memory management
    LockFreePacketPool packetPool_;

    // Helper function to get buffer index from sequence number
    size_t getIndex(uint32_t sequenceNumber) const {
        return sequenceNumber & sequenceMask_;  // Equivalent to mod bufferSize_ since bufferSize_ is a power of 2
    }
};

} // namespace AES67

#endif // LOCK_FREE_CIRCULAR_JITTER_BUFFER_H