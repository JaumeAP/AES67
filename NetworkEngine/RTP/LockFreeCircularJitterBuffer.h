#ifndef LOCK_FREE_CIRCULAR_JITTER_BUFFER_H
#define LOCK_FREE_CIRCULAR_JITTER_BUFFER_H

#include "LockFreePacketPool.h"
#include <atomic>
#include <memory>
#include <cstdint>
#include <array>

namespace AES67 {

// Atomic state machine for lock-free slot management
enum class SlotState : uint8_t {
    EMPTY = 0,    // Slot is available for writing
    WRITING = 1,  // Writer is currently writing data
    READY = 2,    // Data is ready for reading
    READING = 3   // Reader is currently reading data
};

// Structure to hold packet with its sequence number and presentation time
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
    static constexpr size_t BUFFER_SIZE = 256;  // Power of 2 for efficient modulo operations
    static constexpr uint32_t SEQUENCE_MASK = BUFFER_SIZE - 1;  // Mask for modulo operations
    
    LockFreeCircularJitterBuffer();
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
    size_t getMaxBufferSize() const { return BUFFER_SIZE; }
    
    // Reset the buffer
    void reset();
    
    // Get reference to packet pool
    LockFreePacketPool& getPacketPool() { return packetPool_; }

private:
    // Circular buffer to hold packets indexed by sequence number
    std::array<LockFreeBufferPacket, BUFFER_SIZE> buffer_;
    
    // Expected sequence number for the audio thread
    std::atomic<uint32_t> expectedSequenceNumber_{0};
    
    // Statistics
    std::atomic<size_t> totalPackets_{0};
    std::atomic<size_t> droppedPackets_{0};
    std::atomic<size_t> validPackets_{0};
    
    // Packet pool for memory management
    LockFreePacketPool packetPool_;
    
    // Helper function to get buffer index from sequence number
    static constexpr size_t getIndex(uint32_t sequenceNumber) {
        return sequenceNumber & SEQUENCE_MASK;  // Equivalent to mod BUFFER_SIZE since BUFFER_SIZE is a power of 2
    }
};

} // namespace AES67

#endif // LOCK_FREE_CIRCULAR_JITTER_BUFFER_H