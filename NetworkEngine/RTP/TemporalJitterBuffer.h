#ifndef TEMPORAL_JITTER_BUFFER_H
#define TEMPORAL_JITTER_BUFFER_H

#include "LockFreePacketPool.h"
#include "LockFreeRingBuffer.h"
#include <atomic>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace AES67 {

// Structure to hold packet with its presentation time for ordering
struct TimedPacket {
    uint64_t presentationTime;
    uint8_t data[1500]; // Standard max Ethernet frame size
    size_t length;
    uint32_t sequenceNumber;
    uint64_t arrivalTime;
    
    TimedPacket() : presentationTime(0), length(0), sequenceNumber(0), arrivalTime(0) {}
    
    // Comparison operator for ordering by presentation time
    bool operator<(const TimedPacket& other) const {
        return presentationTime > other.presentationTime; // Reverse for min-heap behavior
    }
};

class TemporalJitterBuffer {
public:
    static constexpr size_t BUFFER_CAPACITY = 200;  // Power of 2 for ring buffer
    
    TemporalJitterBuffer();
    ~TemporalJitterBuffer();
    
    // Add a packet to the buffer (should be called from network thread)
    bool addPacket(const uint8_t* packetData, size_t packetLength, 
                   uint32_t sequenceNumber, uint64_t presentationTime);
    
    // Get the next packet that should be played based on presentation time
    // Should be called from audio thread
    bool getNextPacket(uint8_t* outputBuffer, size_t bufferSize, 
                       size_t& outputLength, uint64_t& presentationTime, 
                       uint64_t currentTime, uint64_t toleranceNs = 1000000); // 1ms tolerance by default
    
    // Get buffer statistics
    size_t getBufferedPacketCount() const;
    size_t getMaxBufferSize() const { return BUFFER_CAPACITY; }
    
    // Reset the buffer
    void reset();
    
    // Get reference to packet pool
    LockFreePacketPool& getPacketPool() { return packetPool_; }

private:
    // Lock-free ring buffer to transfer packets between threads
    std::unique_ptr<LockFreeRingBuffer<TimedPacket, BUFFER_CAPACITY>> ringBuffer_;
    
    // Local buffer for the audio thread to maintain temporal ordering
    std::vector<TimedPacket> temporalBuffer_;
    mutable std::mutex temporalMutex_;  // Only used in audio thread during non-critical moments
    
    size_t maxBufferSize_;
    RTPPacketPool packetPool_;
    
    // Track the last sequence number for packet loss detection
    std::atomic<uint32_t> lastSequenceNumber_{0};
    std::atomic<bool> firstPacket_{true};
    
    // Statistics
    std::atomic<size_t> droppedPackets_{0};
    std::atomic<size_t> totalPackets_{0};
    
    // Helper method to maintain temporal order locally
    void addToTemporalBuffer(const TimedPacket& packet);
};

} // namespace AES67

#endif // TEMPORAL_JITTER_BUFFER_H