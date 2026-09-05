#include "LockFreePacketPool.h"
#include <cstring>

namespace AES67 {

LockFreePacketPool::LockFreePacketPool(size_t poolSize) : totalCount_(poolSize) {
    // Pre-allocate all packet memory as a contiguous block
    poolMemory_ = new PooledRTPPacket[totalCount_];

    // Allocate the node pool and in-use flags
    nodePool_ = std::make_unique<Node[]>(totalCount_);
    inUseFlags_ = std::make_unique<std::atomic<bool>[]>(totalCount_);

    // Initialize all packets as free and build the free list
    for (size_t i = 0; i < totalCount_; ++i) {
        inUseFlags_[i].store(false, std::memory_order_relaxed);
        nodePool_[i].index = i;

        // Push to free list using tagged pointer (ABA-safe)
        TaggedPtr oldHead = freeListHead_.load(std::memory_order_relaxed);
        TaggedPtr newHead;
        do {
            nodePool_[i].next = oldHead.ptr;
            newHead = TaggedPtr(&nodePool_[i], oldHead.tag + 1);
        } while (!freeListHead_.compare_exchange_weak(oldHead, newHead,
                                                      std::memory_order_release,
                                                      std::memory_order_relaxed));
    }

    availableCount_.store(totalCount_, std::memory_order_relaxed);
}

LockFreePacketPool::~LockFreePacketPool() {
    delete[] poolMemory_;
}

LockFreePacketPool::PooledRTPPacket* LockFreePacketPool::acquire() {
    // Pop from the lock-free stack (ABA-safe via tagged pointer)
    TaggedPtr oldHead = freeListHead_.load(std::memory_order_acquire);
    TaggedPtr newHead;

    do {
        if (oldHead.ptr == nullptr) {
            return nullptr; // Pool is empty
        }
        newHead = TaggedPtr(oldHead.ptr->next, oldHead.tag + 1);
    } while (!freeListHead_.compare_exchange_weak(oldHead, newHead,
                                                  std::memory_order_release,
                                                  std::memory_order_acquire));

    // Mark the corresponding packet as in use
    inUseFlags_[oldHead.ptr->index].store(true, std::memory_order_release);
    availableCount_.fetch_sub(1, std::memory_order_relaxed);

    return &poolMemory_[oldHead.ptr->index];
}

void LockFreePacketPool::release(LockFreePacketPool::PooledRTPPacket* packet) {
    if (!packet) return;

    // Calculate the index of the packet in our pool
    size_t index = packet - poolMemory_;
    if (index >= totalCount_) {
        return; // Not a valid pool packet
    }

    // Atomically check-and-clear the in-use flag.
    // compare_exchange prevents the TOCTOU race where two concurrent
    // release() calls could both see inUseFlags_ as true and double-push
    // the same node onto the free list.
    bool expected = true;
    if (!inUseFlags_[index].compare_exchange_strong(expected, false,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
        return; // Already free (double-free protection)
    }

    // Reset the packet
    packet->length = 0;
    packet->sequenceNumber = 0;
    packet->presentationTime = 0;
    packet->arrivalTime = 0;

    // Push the node back to the free list (ABA-safe via tagged pointer)
    TaggedPtr oldHead = freeListHead_.load(std::memory_order_relaxed);
    TaggedPtr newHead;
    do {
        nodePool_[index].next = oldHead.ptr;
        newHead = TaggedPtr(&nodePool_[index], oldHead.tag + 1);
    } while (!freeListHead_.compare_exchange_weak(oldHead, newHead,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed));

    availableCount_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace AES67
