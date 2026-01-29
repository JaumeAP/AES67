#include "TemporalJitterBuffer.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace AES67 {

TemporalJitterBuffer::TemporalJitterBuffer()
    : ringBuffer_(std::make_unique<LockFreeRingBuffer<TimedPacket, BUFFER_CAPACITY>>()),
      temporalBuffer_(BUFFER_CAPACITY),
      maxBufferSize_(BUFFER_CAPACITY),
      packetPool_(BUFFER_CAPACITY) {
    // Initialize temporal buffer
    temporalBuffer_.clear();
}

TemporalJitterBuffer::~TemporalJitterBuffer() = default;

// Add a packet to the buffer (should be called from network thread)
bool TemporalJitterBuffer::addPacket(const uint8_t* packetData, size_t packetLength, 
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
    
    // Prepare a timed packet
    TimedPacket timedPacket;
    timedPacket.presentationTime = presentationTime;
    timedPacket.sequenceNumber = sequenceNumber;
    timedPacket.length = std::min(packetLength, sizeof(timedPacket.data));
    
    // Copy packet data
    std::memcpy(timedPacket.data, packetData, timedPacket.length);
    
    // Get current time for arrival time tracking
    auto now = std::chrono::high_resolution_clock::now();
    timedPacket.arrivalTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    
    // Try to push to the lock-free ring buffer
    bool success = ringBuffer_->tryPush(std::move(timedPacket));
    
    if (success) {
        totalPackets_.fetch_add(1);
    } else {
        droppedPackets_.fetch_add(1);
    }
    
    return success;
}

// Get the next packet that should be played based on presentation time
// Should be called from audio thread
bool TemporalJitterBuffer::getNextPacket(uint8_t* outputBuffer, size_t bufferSize, 
                                        size_t& outputLength, uint64_t& presentationTime, 
                                        uint64_t currentTime, uint64_t toleranceNs) {
    // First, pull any new packets from the ring buffer to our local temporal buffer
    TimedPacket tempPacket;
    while (ringBuffer_->tryPop(tempPacket)) {
        // Add to our local temporal buffer with maintained order
        addToTemporalBuffer(tempPacket);
    }
    
    // Look for the earliest packet that is ready to be played
    std::lock_guard<std::mutex> lock(temporalMutex_);
    
    // Find the first packet that's ready to play
    auto it = temporalBuffer_.begin();
    while (it != temporalBuffer_.end()) {
        if (it->presentationTime <= currentTime + toleranceNs) {
            // Found a packet ready to play
            if (it->length <= bufferSize) {
                // Copy data to output buffer
                std::memcpy(outputBuffer, it->data, it->length);
                outputLength = it->length;
                presentationTime = it->presentationTime;
                
                // Remove the packet from our buffer
                temporalBuffer_.erase(it);
                
                return true;
            } else {
                // Packet too large, remove it anyway
                temporalBuffer_.erase(it);
                return false;
            }
        }
        ++it;
    }
    
    // No packets ready to play yet
    return false;
}

// Helper method to maintain temporal order in the local buffer
void TemporalJitterBuffer::addToTemporalBuffer(const TimedPacket& packet) {
    std::lock_guard<std::mutex> lock(temporalMutex_);
    
    // Find the correct position to insert based on presentation time
    auto insertPos = std::lower_bound(temporalBuffer_.begin(), temporalBuffer_.end(), packet,
        [](const TimedPacket& a, const TimedPacket& b) {
            return a.presentationTime < b.presentationTime;
        });
    
    if (insertPos != temporalBuffer_.end() || temporalBuffer_.size() < BUFFER_CAPACITY) {
        if (temporalBuffer_.size() >= BUFFER_CAPACITY) {
            // Buffer is full, remove the oldest packet (earliest presentation time)
            temporalBuffer_.erase(temporalBuffer_.begin());
        }
        
        // Insert the new packet in the correct position
        temporalBuffer_.insert(insertPos, packet);
    } else {
        // Buffer is full and this packet is later than all others, drop it
        droppedPackets_.fetch_add(1);
    }
}

// Get the number of buffered packets
size_t TemporalJitterBuffer::getBufferedPacketCount() const {
    std::lock_guard<std::mutex> lock(temporalMutex_);
    return temporalBuffer_.size();
}

// Reset the buffer
void TemporalJitterBuffer::reset() {
    // Clear the ring buffer by consuming all items
    TimedPacket tempPacket;
    while (ringBuffer_->tryPop(tempPacket)) {
        // Discard all packets
    }
    
    // Clear the temporal buffer
    std::lock_guard<std::mutex> lock(temporalMutex_);
    temporalBuffer_.clear();
    
    // Reset sequence tracking
    lastSequenceNumber_.store(0);
    firstPacket_.store(true);
    
    // Reset statistics
    droppedPackets_.store(0);
    totalPackets_.store(0);
}

} // namespace AES67