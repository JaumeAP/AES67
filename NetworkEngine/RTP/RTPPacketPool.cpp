#include "RTPPacketPool.h"
#include <cstring>

namespace AES67 {

RTPPacketPool::RTPPacketPool(size_t poolSize) : totalCount_(poolSize) {
    // Pre-allocate all packet memory as a contiguous block
    poolMemory_ = new PooledRTPPacket[totalCount_];
    
    // Initialize the available pool
    for (size_t i = 0; i < totalCount_; ++i) {
        poolMemory_[i].inUse = false;
        availablePool_.push(&poolMemory_[i]);
    }
}

RTPPacketPool::~RTPPacketPool() {
    delete[] poolMemory_;
}

RTPPacketPool::PooledRTPPacket* RTPPacketPool::acquire() {
    std::lock_guard<std::mutex> lock(poolMutex_);
    
    if (availablePool_.empty()) {
        return nullptr; // Pool exhausted - caller should handle this appropriately
    }
    
    PooledRTPPacket* packet = availablePool_.top();
    availablePool_.pop();
    
    // Mark as in use and reset key fields
    packet->inUse = true;
    packet->length = 0;
    
    return packet;
}

void RTPPacketPool::release(PooledRTPPacket* packet) {
    if (!packet) return;
    
    std::lock_guard<std::mutex> lock(poolMutex_);
    
    // Mark as no longer in use
    packet->inUse = false;
    
    // Return to available pool
    availablePool_.push(packet);
}

size_t RTPPacketPool::getAvailableCount() const {
    std::lock_guard<std::mutex> lock(poolMutex_);
    return availablePool_.size();
}

} // namespace AES67