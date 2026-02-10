#ifndef LOCK_FREE_PACKET_POOL_H
#define LOCK_FREE_PACKET_POOL_H

#include <atomic>
#include <memory>
#include <cstddef>
#include <cstdint>

namespace AES67 {

// Lock-free pool for RTP packets to avoid heap allocations during audio processing.
//
// Uses a tagged pointer to prevent the ABA problem in the lock-free stack:
// A version counter is incremented on every push/pop, so even if a node is
// recycled to the same address, the CAS will fail if any intervening
// operations occurred.
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

    // Get pool statistics (O(1) using atomic counter)
    size_t getAvailableCount() const { return availableCount_.load(std::memory_order_relaxed); }
    size_t getTotalCount() const { return totalCount_; }

private:
    PooledRTPPacket* poolMemory_;  // Pre-allocated contiguous memory
    size_t totalCount_;

    // Node for the lock-free stack
    struct Node {
        size_t index;
        Node* next;
    };

    // Tagged pointer to prevent ABA problem.
    // Combines a Node* with a monotonically increasing version counter.
    // On 64-bit systems, we use the upper 16 bits for the tag (pointers only
    // use 48 bits on current x86-64/ARM64 architectures).
    struct TaggedPtr {
        Node* ptr;
        uint64_t tag;

        TaggedPtr() : ptr(nullptr), tag(0) {}
        TaggedPtr(Node* p, uint64_t t) : ptr(p), tag(t) {}

        bool operator==(const TaggedPtr& other) const {
            return ptr == other.ptr && tag == other.tag;
        }
    };

    alignas(64) std::atomic<TaggedPtr> freeListHead_{TaggedPtr()};

    // O(1) available count (avoids traversing the free list)
    alignas(64) std::atomic<size_t> availableCount_{0};

    // Flags to track which packets are in use
    std::unique_ptr<std::atomic<bool>[]> inUseFlags_;

    // Pre-allocated nodes for the free list
    std::unique_ptr<Node[]> nodePool_;
};

} // namespace AES67

#endif // LOCK_FREE_PACKET_POOL_H
