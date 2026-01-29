#ifndef LOCK_FREE_PACKET_POOL_H
#define LOCK_FREE_PACKET_POOL_H

#include <atomic>
#include <memory>
#include <cstddef>

namespace AES67 {

// Lock-free pool for RTP packets to avoid heap allocations during audio processing
class LockFreePacketPool {
public:
    static constexpr size_t MAX_PACKET_SIZE = 2048; // Typical max RTP packet size
    static constexpr size_t DEFAULT_POOL_SIZE = 100;
    
    struct PooledRTPPacket {
        uint8_t data[MAX_PACKET_SIZE];
        size_t length;
        uint32_t sequenceNumber;
        uint64_t presentationTime;
        uint64_t arrivalTime;
        
        PooledRTPPacket() : length(0), sequenceNumber(0), presentationTime(0), arrivalTime(0) {}
    };
    
    explicit LockFreePacketPool(size_t poolSize = DEFAULT_POOL_SIZE);
    ~LockFreePacketPool();
    
    // Acquire a packet from the pool (lock-free, called by network thread)
    PooledRTPPacket* acquire();
    
    // Release a packet back to the pool (lock-free, called by audio thread)
    void release(PooledRTPPacket* packet);
    
    // Get pool statistics
    size_t getAvailableCount() const;
    size_t getTotalCount() const { return totalCount_; }

private:
    PooledRTPPacket* poolMemory_;  // Pre-allocated contiguous memory
    size_t totalCount_;
    
    // Lock-free stack using atomic operations
    struct Node {
        size_t index;
        Node* next;
    };
    
    alignas(64) std::atomic<Node*> freeListHead_{nullptr};
    
    // Flags to track which packets are in use
    std::unique_ptr<std::atomic<bool>[]> inUseFlags_;
    
    // Pre-allocated nodes for the free list
    std::unique_ptr<Node[]> nodePool_;
};

} // namespace AES67

#endif // LOCK_FREE_PACKET_POOL_H