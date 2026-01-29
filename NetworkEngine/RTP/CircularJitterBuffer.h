#ifndef CIRCULAR_JITTER_BUFFER_H
#define CIRCULAR_JITTER_BUFFER_H

#include "LockFreePacketPool.h"
#include <atomic>
#include <memory>
#include <cstdint>
#include <array>

namespace AES67 {

// Structure to hold packet with its sequence number and presentation time
struct CircularBufferPacket {
    uint8_t data[1500]; // Standard max Ethernet frame size
    size_t length;
    uint32_t sequenceNumber;
    uint64_t presentationTime;
    uint64_t arrivalTime;
    bool valid;  // Flag to indicate if this slot contains a valid packet
    
    CircularBufferPacket() : length(0), sequenceNumber(0), presentationTime(0), arrivalTime(0), valid(false) {}
};

class CircularJitterBuffer {
public:
    static constexpr size_t BUFFER_SIZE = 256;  // Power of 2 for efficient modulo operations
    static constexpr uint32_t INVALID_SEQUENCE = 0xFFFFFFFF;  // Special value for empty slots
    
    CircularJitterBuffer();
    ~CircularJitterBuffer();
    
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
    std::array<CircularBufferPacket, BUFFER_SIZE> buffer_;
    
    // Atomic access to buffer elements to prevent race conditions
    // Each slot can be accessed independently
    std::array<std::atomic_flag, BUFFER_SIZE> slotLocks_;
    
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
        return sequenceNumber % BUFFER_SIZE;
    }
};

} // namespace AES67

#endif // CIRCULAR_JITTER_BUFFER_H