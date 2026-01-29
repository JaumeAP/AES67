#ifndef RTP_PACKET_POOL_H
#define RTP_PACKET_POOL_H

#include <memory>
#include <mutex>
#include <stack>
#include <cstddef>

namespace AES67 {

// Pre-allocated pool for RTP packets to avoid heap allocations during audio processing
class RTPPacketPool {
public:
    static constexpr size_t MAX_PACKET_SIZE = 2048; // Typical max RTP packet size
    
    struct PooledRTPPacket {
        uint8_t data[MAX_PACKET_SIZE];
        size_t length;
        uint32_t sequenceNumber;
        uint64_t presentationTime;
        uint64_t arrivalTime;
        bool inUse;
        
        PooledRTPPacket() : length(0), sequenceNumber(0), presentationTime(0), arrivalTime(0), inUse(false) {}
    };
    
    explicit RTPPacketPool(size_t poolSize = 100);  // Default to 100 packets in pool
    ~RTPPacketPool();
    
    // Acquire a packet from the pool (never allocates during audio processing)
    PooledRTPPacket* acquire();
    
    // Release a packet back to the pool
    void release(PooledRTPPacket* packet);
    
    // Get pool statistics
    size_t getAvailableCount() const;
    size_t getTotalCount() const { return totalCount_; }

private:
    std::stack<PooledRTPPacket*> availablePool_;
    PooledRTPPacket* poolMemory_;  // Pre-allocated contiguous memory
    size_t totalCount_;
    mutable std::mutex poolMutex_;  // Only used during acquire/release, not during audio processing
};

} // namespace AES67

#endif // RTP_PACKET_POOL_H