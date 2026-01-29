#ifndef LOCK_FREE_RING_BUFFER_H
#define LOCK_FREE_RING_BUFFER_H

#include <atomic>
#include <memory>
#include <thread>
#include <cstdint>

namespace AES67 {

// Lock-free SPSC (Single Producer, Single Consumer) Ring Buffer
template<typename T, size_t Capacity>
class LockFreeRingBuffer {
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    
    LockFreeRingBuffer();
    ~LockFreeRingBuffer() = default;
    
    // Non-copyable
    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;
    
    // Try to push an item to the buffer (called by producer/network thread)
    bool tryPush(const T& item);
    bool tryPush(T&& item);
    
    // Try to pop an item from the buffer (called by consumer/audio thread)
    bool tryPop(T& item);
    
    // Check if buffer is empty
    bool isEmpty() const;
    
    // Check if buffer is full
    bool isFull() const;
    
    // Get current number of items in buffer
    size_t size() const;
    
    // Get buffer capacity
    size_t capacity() const { return Capacity; }

private:
    alignas(64) std::atomic<size_t> head_{0};  // Producer index
    alignas(64) std::atomic<size_t> tail_{0};  // Consumer index
    
    T buffer_[Capacity];
    
    // Masks for circular buffer arithmetic (works because capacity is power of 2)
    static constexpr size_t MASK = Capacity - 1;
};

template<typename T, size_t Capacity>
LockFreeRingBuffer<T, Capacity>::LockFreeRingBuffer() {
    // Initialize all elements to default values
    for (size_t i = 0; i < Capacity; ++i) {
        new (&buffer_[i]) T{};
    }
}

template<typename T, size_t Capacity>
bool LockFreeRingBuffer<T, Capacity>::tryPush(const T& item) {
    const size_t currentHead = head_.load(std::memory_order_relaxed);
    const size_t nextHead = (currentHead + 1) & MASK;
    
    if (nextHead == tail_.load(std::memory_order_acquire)) {
        // Buffer is full
        return false;
    }
    
    buffer_[currentHead] = item;
    head_.store(nextHead, std::memory_order_release);
    
    return true;
}

template<typename T, size_t Capacity>
bool LockFreeRingBuffer<T, Capacity>::tryPush(T&& item) {
    const size_t currentHead = head_.load(std::memory_order_relaxed);
    const size_t nextHead = (currentHead + 1) & MASK;
    
    if (nextHead == tail_.load(std::memory_order_acquire)) {
        // Buffer is full
        return false;
    }
    
    buffer_[currentHead] = std::move(item);
    head_.store(nextHead, std::memory_order_release);
    
    return true;
}

template<typename T, size_t Capacity>
bool LockFreeRingBuffer<T, Capacity>::tryPop(T& item) {
    const size_t currentTail = tail_.load(std::memory_order_relaxed);
    
    if (currentTail == head_.load(std::memory_order_acquire)) {
        // Buffer is empty
        return false;
    }
    
    item = std::move(buffer_[currentTail]);
    tail_.store((currentTail + 1) & MASK, std::memory_order_release);
    
    return true;
}

template<typename T, size_t Capacity>
bool LockFreeRingBuffer<T, Capacity>::isEmpty() const {
    return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
}

template<typename T, size_t Capacity>
bool LockFreeRingBuffer<T, Capacity>::isFull() const {
    const size_t nextHead = (head_.load(std::memory_order_acquire) + 1) & MASK;
    return nextHead == tail_.load(std::memory_order_acquire);
}

template<typename T, size_t Capacity>
size_t LockFreeRingBuffer<T, Capacity>::size() const {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    
    if (head >= tail) {
        return head - tail;
    } else {
        return Capacity - tail + head;
    }
}

} // namespace AES67

#endif // LOCK_FREE_RING_BUFFER_H