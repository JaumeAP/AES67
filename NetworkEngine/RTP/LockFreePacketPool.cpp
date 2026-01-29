#include "LockFreePacketPool.h"
#include <cstring>

namespace AES67 {

LockFreePacketPool::LockFreePacketPool(size_t poolSize) : totalCount_(poolSize) {
    // Pre-allocate all packet memory as a contiguous block
    poolMemory_ = new PooledRTPPacket[totalCount_];
    
    // Allocate the node pool and in-use flags
    nodePool_ = std::make_unique<Node[]>(totalCount_);
    inUseFlags_ = std::make_unique<std::atomic<bool>[]>(totalCount_);
    
    // Initialize all packets as free
    for (size_t i = 0; i < totalCount_; ++i) {
        inUseFlags_[i].store(false, std::memory_order_relaxed);
        
        // Initialize the node with the index
        nodePool_[i].index = i;
        
        // Add to the free list using lock-free stack push
        Node* oldHead = freeListHead_.load(std::memory_order_relaxed);
        nodePool_[i].next = oldHead;
        
        while (!freeListHead_.compare_exchange_weak(oldHead, &nodePool_[i], 
                                                   std::memory_order_release, 
                                                   std::memory_order_relaxed)) {
            nodePool_[i].next = oldHead;
        }
    }
}

LockFreePacketPool::~LockFreePacketPool() {
    delete[] poolMemory_;
}

LockFreePacketPool::PooledRTPPacket* LockFreePacketPool::acquire() {
    // Pop from the lock-free stack
    Node* node = freeListHead_.load(std::memory_order_acquire);
    Node* newHead;
    
    do {
        if (node == nullptr) {
            return nullptr; // Pool is empty
        }
        newHead = node->next;
    } while (!freeListHead_.compare_exchange_weak(node, newHead, 
                                                 std::memory_order_release, 
                                                 std::memory_order_acquire));
    
    // Mark the corresponding packet as in use
    inUseFlags_[node->index].store(true, std::memory_order_release);
    
    return &poolMemory_[node->index];
}

void LockFreePacketPool::release(LockFreePacketPool::PooledRTPPacket* packet) {
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
    Node* oldHead = freeListHead_.load(std::memory_order_relaxed);
    nodePool_[index].next = oldHead;
    
    while (!freeListHead_.compare_exchange_weak(oldHead, &nodePool_[index], 
                                               std::memory_order_release, 
                                               std::memory_order_relaxed)) {
        nodePool_[index].next = oldHead;
    }
}

size_t LockFreePacketPool::getAvailableCount() const {
    // Counting elements in a lock-free stack is complex
    // In a real implementation, we might maintain a separate atomic counter
    // For now, this is a simplified implementation
    size_t count = 0;
    Node* current = freeListHead_.load(std::memory_order_acquire);
    
    while (current != nullptr) {
        count++;
        current = current->next;
    }
    
    return count;
}

} // namespace AES67