#include "CircularJitterBuffer.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace AES67 {

CircularJitterBuffer::CircularJitterBuffer() : packetPool_(BUFFER_SIZE) {
    // Initialize the buffer slots
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        buffer_[i].valid = false;
        slotLocks_[i].clear();  // Initially unlocked
    }
    
    // Initialize expected sequence number
    expectedSequenceNumber_.store(0);
}

CircularJitterBuffer::~CircularJitterBuffer() = default;

bool CircularJitterBuffer::addPacket(const uint8_t* packetData, size_t packetLength, 
                                   uint32_t sequenceNumber, uint64_t presentationTime) {
    // Get the buffer index based on sequence number
    size_t index = getIndex(sequenceNumber);
    
    // Try to acquire the lock for this slot
    while (slotLocks_[index].test_and_set(std::memory_order_acquire)) {
        // Busy wait, but only for a very short time since we expect
        // the lock to be released quickly
        std::this_thread::yield();
    }
    
    // Copy packet data to the buffer slot
    CircularBufferPacket& slot = buffer_[index];
    size_t copySize = std::min(packetLength, sizeof(slot.data));
    
    std::memcpy(slot.data, packetData, copySize);
    slot.length = copySize;
    slot.sequenceNumber = sequenceNumber;
    slot.presentationTime = presentationTime;
    
    // Get current time for arrival time tracking
    auto now = std::chrono::high_resolution_clock::now();
    slot.arrivalTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    
    // Mark as valid
    slot.valid = true;
    
    // Release the lock
    slotLocks_[index].clear(std::memory_order_release);
    
    // Update statistics
    totalPackets_.fetch_add(1);
    validPackets_.fetch_add(1);
    
    return true;
}

bool CircularJitterBuffer::getNextPacket(uint8_t* outputBuffer, size_t bufferSize, 
                                       size_t& outputLength, uint64_t& presentationTime, 
                                       uint32_t expectedSequenceNumber) {
    // Get the buffer index based on expected sequence number
    size_t index = getIndex(expectedSequenceNumber);
    
    // Try to acquire the lock for this slot
    while (slotLocks_[index].test_and_set(std::memory_order_acquire)) {
        // Busy wait, but only for a very short time
        std::this_thread::yield();
    }
    
    // Check if the slot has a valid packet with the expected sequence number
    CircularBufferPacket& slot = buffer_[index];
    if (slot.valid && slot.sequenceNumber == expectedSequenceNumber) {
        // Copy data to output buffer
        if (slot.length <= bufferSize) {
            std::memcpy(outputBuffer, slot.data, slot.length);
            outputLength = slot.length;
            presentationTime = slot.presentationTime;
            
            // Mark slot as invalid to free it up
            slot.valid = false;
            
            // Update expected sequence number
            expectedSequenceNumber_.store(expectedSequenceNumber + 1);
            
            // Update statistics
            validPackets_.fetch_sub(1);
            
            // Release the lock
            slotLocks_[index].clear(std::memory_order_release);
            
            return true;
        } else {
            // Packet too large for output buffer
            slot.valid = false; // Mark as invalid anyway
        }
    }
    
    // Release the lock
    slotLocks_[index].clear(std::memory_order_release);
    
    return false;
}

bool CircularJitterBuffer::getPacketBySequence(uint8_t* outputBuffer, size_t bufferSize, 
                                             size_t& outputLength, uint64_t& presentationTime, 
                                             uint32_t sequenceNumber) {
    // Get the buffer index based on sequence number
    size_t index = getIndex(sequenceNumber);
    
    // Try to acquire the lock for this slot
    while (slotLocks_[index].test_and_set(std::memory_order_acquire)) {
        // Busy wait, but only for a very short time
        std::this_thread::yield();
    }
    
    // Check if the slot has a valid packet with the requested sequence number
    CircularBufferPacket& slot = buffer_[index];
    if (slot.valid && slot.sequenceNumber == sequenceNumber) {
        // Copy data to output buffer
        if (slot.length <= bufferSize) {
            std::memcpy(outputBuffer, slot.data, slot.length);
            outputLength = slot.length;
            presentationTime = slot.presentationTime;
            
            // Mark slot as invalid to free it up
            slot.valid = false;
            
            // Update statistics
            validPackets_.fetch_sub(1);
            
            // Release the lock
            slotLocks_[index].clear(std::memory_order_release);
            
            return true;
        } else {
            // Packet too large for output buffer
            slot.valid = false; // Mark as invalid anyway
        }
    }
    
    // Release the lock
    slotLocks_[index].clear(std::memory_order_release);
    
    return false;
}

size_t CircularJitterBuffer::getBufferedPacketCount() const {
    return validPackets_.load();
}

void CircularJitterBuffer::reset() {
    // Lock all slots to ensure safe reset
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        while (slotLocks_[i].test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    
    // Reset all slots
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        buffer_[i].valid = false;
    }
    
    // Reset statistics
    totalPackets_.store(0);
    droppedPackets_.store(0);
    validPackets_.store(0);
    
    // Reset expected sequence number
    expectedSequenceNumber_.store(0);
    
    // Unlock all slots
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        slotLocks_[i].clear(std::memory_order_release);
    }
}

} // namespace AES67