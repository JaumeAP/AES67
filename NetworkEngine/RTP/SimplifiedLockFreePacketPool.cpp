#include "SimplifiedLockFreePacketPool.h"
#include <cstring>

namespace AES67 {

SimplifiedLockFreePacketPool::SimplifiedLockFreePacketPool(size_t poolSize) : totalCount_(poolSize) {
    // Pre-allocate all packet memory as a contiguous block
    poolMemory_ = new PooledRTPPacket[totalCount_];
    
    // Allocate the node pool and in-use flags
    nodePool_ = std::make_unique<Node[]>(totalCount_);
    nodeInUse_ = std::make_unique<std::atomic<bool>[]>(totalCount_);
    inUseFlags_ = std::make_unique<std::atomic<bool>[]>(totalCount_);
    
    // Initialize all packets as free
    for (size_t i = 0; i < totalCount_; ++i) {
        inUseFlags_[i].store(false, std::memory_order_relaxed);
        nodeInUse_[i].store(false, std::memory_order_relaxed);
        
        // Initialize the node with the index
        nodePool_[i].index = i;
        nodePool_[i].next = nullptr;
        
        // Add to the free list using lock-free stack push
        Node* oldHead = head_.load(std::memory_order_relaxed);
        nodePool_[i].next = oldHead;
        
        while (!head_.compare_exchange_weak(oldHead, &nodePool_[i], std::memory_order_release, std::memory_order_relaxed)) {
            nodePool_[i].next = oldHead;
        }
    }
}

SimplifiedLockFreePacketPool::~SimplifiedLockFreePacketPool() {
    delete[] poolMemory_;
}

SimplifiedLockFreePacketPool::PooledRTPPacket* SimplifiedLockFreePacketPool::acquire() {
    // Pop from the lock-free stack
    Node* node = head_.load(std::memory_order_acquire);
    Node* newHead;
    
    do {
        if (node == nullptr) {
            return nullptr; // Pool is empty
        }
        newHead = node->next;
    } while (!head_.compare_exchange_weak(node, newHead, std::memory_order_release, std::memory_order_relaxed));
    
    // Mark the corresponding packet as in use
    inUseFlags_[node->index].store(true, std::memory_order_release);
    
    return &poolMemory_[node->index];
}

void SimplifiedLockFreePacketPool::release(SimplifiedLockFreePacketPool::PooledRTPPacket* packet) {
    if (!packet) return;
    
    // Calculate the index of the packet in our pool
    size_t index = packet - poolMemory_;
    if (index >= totalCount_) {
        return; // Not a valid pool packet
    }
    
    // Verify it was actually in use
    if (!inUseFlags_[index].load(std::memory_order_acquire)) {
        return; // Already free
    }
    
    // Reset the packet
    packet->length = 0;
    packet->sequenceNumber = 0;
    packet->presentationTime = 0;
    packet->arrivalTime = 0;
    
    // Mark as not in use
    inUseFlags_[index].store(false, std::memory_order_release);
    
    // Push the node back to the free list
    Node* oldHead = head_.load(std::memory_order_relaxed);
    nodePool_[index].next = oldHead;
    
    while (!head_.compare_exchange_weak(oldHead, &nodePool_[index], std::memory_order_release, std::memory_order_relaxed)) {
        nodePool_[index].next = oldHead;
    }
}

size_t SimplifiedLockFreePacketPool::getAvailableCount() const {
    // Note: Counting elements in a lock-free stack is not straightforward
    // This is a simplified approach - in a real implementation you might want
    // to maintain a separate atomic counter
    return totalCount_ - getTotalCount(); // This is just a placeholder
}

} // namespace AES67