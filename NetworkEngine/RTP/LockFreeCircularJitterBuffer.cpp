#include "LockFreeCircularJitterBuffer.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace AES67 {

LockFreeCircularJitterBuffer::LockFreeCircularJitterBuffer() : packetPool_(BUFFER_SIZE) {
    // Initialize the buffer slots (they start as EMPTY by default)
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        buffer_[i].state.store(SlotState::EMPTY, std::memory_order_relaxed);
    }

    // Initialize expected sequence number
    expectedSequenceNumber_.store(0, std::memory_order_relaxed);
}

LockFreeCircularJitterBuffer::~LockFreeCircularJitterBuffer() = default;

bool LockFreeCircularJitterBuffer::addPacket(const uint8_t* packetData, size_t packetLength,
                                          uint32_t sequenceNumber, uint64_t presentationTime) {
    // Get the buffer index based on sequence number
    size_t index = getIndex(sequenceNumber);
    LockFreeBufferPacket& slot = buffer_[index];

    // Try to atomically transition from EMPTY to WRITING
    // This gives us exclusive write access to the slot
    SlotState expected = SlotState::EMPTY;
    if (!slot.state.compare_exchange_strong(expected, SlotState::WRITING,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
        // Slot is not empty (either WRITING, READY, or READING)
        // We cannot write to it, so drop this packet
        droppedPackets_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // We now have exclusive write access (state is WRITING)
    // No reader can access this slot until we transition to READY

    // Limit the amount of data copied to fit in the buffer
    size_t copySize = std::min(packetLength, sizeof(slot.data));

    // Copy the data (safe because state is WRITING)
    std::memcpy(slot.data, packetData, copySize);

    // Update metadata fields (no memory ordering needed here - state transition handles it)
    slot.length.store(copySize, std::memory_order_relaxed);
    slot.sequenceNumber.store(sequenceNumber, std::memory_order_relaxed);
    slot.presentationTime.store(presentationTime, std::memory_order_relaxed);

    // Get current time for arrival time tracking
    auto now = std::chrono::high_resolution_clock::now();
    uint64_t arrivalTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    slot.arrivalTime.store(arrivalTime, std::memory_order_relaxed);

    // Transition from WRITING to READY with release semantics
    // This makes all the data writes visible to readers
    slot.state.store(SlotState::READY, std::memory_order_release);

    // Update statistics
    totalPackets_.fetch_add(1, std::memory_order_relaxed);
    validPackets_.fetch_add(1, std::memory_order_relaxed);

    return true;
}

bool LockFreeCircularJitterBuffer::getNextPacket(uint8_t* outputBuffer, size_t bufferSize,
                                              size_t& outputLength, uint64_t& presentationTime,
                                              uint32_t expectedSequenceNumber) {
    // Get the buffer index based on expected sequence number
    size_t index = getIndex(expectedSequenceNumber);
    LockFreeBufferPacket& slot = buffer_[index];

    // Try to atomically transition from READY to READING
    // This gives us exclusive read access to the slot
    SlotState expected = SlotState::READY;
    if (!slot.state.compare_exchange_strong(expected, SlotState::READING,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
        // Slot is not READY (either EMPTY, WRITING, or already READING)
        return false;
    }

    // We now have exclusive read access (state is READING)
    // The writer cannot modify this slot until we transition to EMPTY

    // Load the sequence number with acquire to ensure we see all writes
    uint32_t storedSequence = slot.sequenceNumber.load(std::memory_order_acquire);

    // Check if it matches the expected sequence number
    if (storedSequence != expectedSequenceNumber) {
        // Wrong packet in this slot (stale from a previous wrap-around).
        // The slot held a valid packet that was counted in validPackets_,
        // so we must decrement before releasing the slot back to EMPTY.
        slot.state.store(SlotState::EMPTY, std::memory_order_release);
        validPackets_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    // Load the length
    size_t storedLength = slot.length.load(std::memory_order_acquire);

    // Check if output buffer is large enough
    if (storedLength > bufferSize) {
        // Can't use this packet - release the slot back to EMPTY
        slot.state.store(SlotState::EMPTY, std::memory_order_release);
        validPackets_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    // Copy data to output buffer (safe because state is READING)
    std::memcpy(outputBuffer, slot.data, storedLength);
    outputLength = storedLength;
    presentationTime = slot.presentationTime.load(std::memory_order_acquire);

    // Transition from READING to EMPTY with release semantics
    // This makes the slot available for writing again
    slot.state.store(SlotState::EMPTY, std::memory_order_release);

    // Update expected sequence number
    expectedSequenceNumber_.store(expectedSequenceNumber + 1, std::memory_order_relaxed);

    // Update statistics
    validPackets_.fetch_sub(1, std::memory_order_relaxed);

    return true;
}

bool LockFreeCircularJitterBuffer::getPacketBySequence(uint8_t* outputBuffer, size_t bufferSize,
                                                   size_t& outputLength, uint64_t& presentationTime,
                                                   uint32_t sequenceNumber) {
    // Get the buffer index based on sequence number
    size_t index = getIndex(sequenceNumber);
    LockFreeBufferPacket& slot = buffer_[index];

    // Try to atomically transition from READY to READING
    // This gives us exclusive read access to the slot
    SlotState expected = SlotState::READY;
    if (!slot.state.compare_exchange_strong(expected, SlotState::READING,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
        // Slot is not READY (either EMPTY, WRITING, or already READING)
        return false;
    }

    // We now have exclusive read access (state is READING)
    // The writer cannot modify this slot until we transition to EMPTY

    // Load the sequence number with acquire to ensure we see all writes
    uint32_t storedSequence = slot.sequenceNumber.load(std::memory_order_acquire);

    // Check if it matches the requested sequence number
    if (storedSequence != sequenceNumber) {
        // Wrong packet in this slot (stale from a previous wrap-around).
        // The slot held a valid packet that was counted in validPackets_,
        // so we must decrement before releasing the slot back to EMPTY.
        slot.state.store(SlotState::EMPTY, std::memory_order_release);
        validPackets_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    // Load the length
    size_t storedLength = slot.length.load(std::memory_order_acquire);

    // Check if output buffer is large enough
    if (storedLength > bufferSize) {
        // Can't use this packet - release the slot back to EMPTY
        slot.state.store(SlotState::EMPTY, std::memory_order_release);
        validPackets_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    // Copy data to output buffer (safe because state is READING)
    std::memcpy(outputBuffer, slot.data, storedLength);
    outputLength = storedLength;
    presentationTime = slot.presentationTime.load(std::memory_order_acquire);

    // Transition from READING to EMPTY with release semantics
    // This makes the slot available for writing again
    slot.state.store(SlotState::EMPTY, std::memory_order_release);

    // Update statistics
    validPackets_.fetch_sub(1, std::memory_order_relaxed);

    return true;
}

size_t LockFreeCircularJitterBuffer::getBufferedPacketCount() const {
    return validPackets_.load(std::memory_order_acquire);
}

void LockFreeCircularJitterBuffer::reset() {
    // Reset all slots to EMPTY state
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        buffer_[i].state.store(SlotState::EMPTY, std::memory_order_release);
    }

    // Reset statistics
    totalPackets_.store(0, std::memory_order_relaxed);
    droppedPackets_.store(0, std::memory_order_relaxed);
    validPackets_.store(0, std::memory_order_relaxed);

    // Reset expected sequence number
    expectedSequenceNumber_.store(0, std::memory_order_relaxed);
}

} // namespace AES67