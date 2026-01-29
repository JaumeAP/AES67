#include "JitterBuffer.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace AES67 {

// Constructor - initialize the priority queue
JitterBuffer::JitterBuffer(size_t maxBufferSize) 
    : maxBufferSize_(maxBufferSize), 
      packetPool_(maxBufferSize) {
}

JitterBuffer::~JitterBuffer() {
    // Release all packets back to the pool
    std::lock_guard<std::mutex> lock(bufferMutex_);
    while (!orderedPackets_.empty()) {
        auto timedPacket = orderedPackets_.top();
        packetPool_.release(timedPacket.packet);
        orderedPackets_.pop();
    }
}

// Add a packet to the buffer (should be called from network thread)
bool JitterBuffer::addPacket(const uint8_t* packetData, size_t packetLength, 
                            uint32_t sequenceNumber, uint64_t presentationTime) {
    // Check for packet loss by looking at sequence numbers
    uint32_t lastSeq = lastSequenceNumber_.load();
    bool isFirst = firstPacket_.load();
    
    if (!isFirst && sequenceNumber != (lastSeq + 1)) {
        // There's a gap in sequence numbers indicating lost packets
        uint32_t lostPackets = sequenceNumber - lastSeq - 1;
        // Handle packet loss if needed
    }
    
    // Update last sequence number
    lastSequenceNumber_.store(sequenceNumber);
    firstPacket_.store(false);
    
    // Acquire a packet from the pool (no allocation during audio processing)
    auto pooledPacket = packetPool_.acquire();
    if (!pooledPacket) {
        // Pool exhausted - drop this packet
        droppedPackets_.fetch_add(1);
        return false;
    }
    
    // Copy packet data to pre-allocated buffer
    if (packetLength > RTPPacketPool::MAX_PACKET_SIZE) {
        // Packet too large, release back to pool
        packetPool_.release(pooledPacket);
        return false;
    }
    
    std::memcpy(pooledPacket->data, packetData, packetLength);
    pooledPacket->length = packetLength;
    pooledPacket->sequenceNumber = sequenceNumber;
    pooledPacket->presentationTime = presentationTime;
    
    // Get current time for arrival time tracking
    auto now = std::chrono::high_resolution_clock::now();
    pooledPacket->arrivalTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    
    // Add to the ordered buffer with mutex protection (minimal duration)
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        
        // If we're at max capacity, remove the oldest packet (by presentation time)
        if (orderedPackets_.size() >= maxBufferSize_) {
            auto oldestPacket = orderedPackets_.top();
            packetPool_.release(oldestPacket.packet);
            orderedPackets_.pop();
        }
        
        // Add the new packet
        TimedPacket timedPacket;
        timedPacket.packet = pooledPacket;
        timedPacket.presentationTime = presentationTime;
        
        orderedPackets_.push(timedPacket);
    }
    
    totalPackets_.fetch_add(1);
    
    return true;
}

// Get the next packet that should be played based on presentation time
bool JitterBuffer::getNextPacket(uint8_t* outputBuffer, size_t bufferSize, 
                                size_t& outputLength, uint64_t& presentationTime, 
                                uint64_t currentTime, uint64_t toleranceNs) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    
    // Check if we have any packets
    if (orderedPackets_.empty()) {
        return false;
    }
    
    // Check if the earliest packet is ready to be played
    auto nextPacket = orderedPackets_.top();
    if (nextPacket.presentationTime <= currentTime + toleranceNs) {
        // This packet is ready to be played
        if (nextPacket.packet->length <= bufferSize) {
            // Copy data to output buffer
            std::memcpy(outputBuffer, nextPacket.packet->data, nextPacket.packet->length);
            outputLength = nextPacket.packet->length;
            presentationTime = nextPacket.presentationTime;
            
            // Remove the packet from the queue and release back to pool
            orderedPackets_.pop();
            packetPool_.release(nextPacket.packet);
            
            return true;
        } else {
            // Packet too large for output buffer, release it and remove from queue
            packetPool_.release(nextPacket.packet);
            orderedPackets_.pop();
        }
    }
    
    // No packet is ready yet
    return false;
}

// Get the number of buffered packets
size_t JitterBuffer::getBufferedPacketCount() const {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return orderedPackets_.size();
}

// Reset the buffer
void JitterBuffer::reset() {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    
    // Release all packets back to the pool
    while (!orderedPackets_.empty()) {
        auto timedPacket = orderedPackets_.top();
        packetPool_.release(timedPacket.packet);
        orderedPackets_.pop();
    }
    
    // Reset sequence tracking
    lastSequenceNumber_.store(0);
    firstPacket_.store(true);
    
    // Reset statistics
    droppedPackets_.store(0);
    totalPackets_.store(0);
}

} // namespace AES67