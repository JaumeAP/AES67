#ifndef LOCK_FREE_PRIORITY_QUEUE_H
#define LOCK_FREE_PRIORITY_QUEUE_H

#include <atomic>
#include <memory>
#include <vector>
#include <functional>
#include <algorithm>
#include <thread>

namespace AES67 {

// Lock-free priority queue using a binary heap implementation
// Designed specifically for audio applications where only the audio thread
// performs pops (of the highest priority item) and the network thread does pushes
template<typename T, size_t Capacity, typename Compare = std::less<T>>
class LockFreePriorityQueue {
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    
    explicit LockFreePriorityQueue(const Compare& comp = Compare{});
    ~LockFreePriorityQueue() = default;
    
    // Non-copyable
    LockFreePriorityQueue(const LockFreePriorityQueue&) = delete;
    LockFreePriorityQueue& operator=(const LockFreePriorityQueue&) = delete;
    
    // Push an item into the priority queue (called by network thread)
    bool push(const T& item);
    bool push(T&& item);
    
    // Pop the highest priority item (called by audio thread)
    bool pop(T& item);
    
    // Peek at the highest priority item without removing it
    bool peek(T& item) const;
    
    // Check if queue is empty
    bool empty() const;
    
    // Get current size
    size_t size() const;
    
    // Get capacity
    size_t capacity() const { return Capacity; }

private:
    // Separate atomic indices for thread safety
    alignas(64) std::atomic<size_t> writeIdx_{0};  // Index for next write position
    alignas(64) std::atomic<size_t> readIdx_{0};   // Index for next read position
    alignas(64) std::atomic<size_t> heapSize_{0};  // Current number of items in heap
    
    // The actual heap storage
    T heap_[Capacity];
    
    // Comparator function
    Compare comp_;
    
    // Synchronize access to the heap structure
    alignas(64) std::atomic_flag heapMutex_{ATOMIC_FLAG_INIT};
    
    // Helper functions for heap operations
    void heapifyUp(size_t idx);
    void heapifyDown(size_t idx);
    void swap(size_t i, size_t j);
};

template<typename T, size_t Capacity, typename Compare>
LockFreePriorityQueue<T, Capacity, Compare>::LockFreePriorityQueue(const Compare& comp)
    : comp_(comp) {
    // Initialize all elements to default values
    for (size_t i = 0; i < Capacity; ++i) {
        new (&heap_[i]) T{};
    }
}

template<typename T, size_t Capacity, typename Compare>
bool LockFreePriorityQueue<T, Capacity, Compare>::push(const T& item) {
    if (heapSize_.load(std::memory_order_acquire) >= Capacity) {
        return false; // Queue is full
    }
    
    // Try to acquire the heap mutex
    while (heapMutex_.test_and_set(std::memory_order_acquire)) {
        // Busy wait, but only for a very short time
        std::this_thread::yield();
    }
    
    // Add item to the end of the heap
    size_t idx = heapSize_.load(std::memory_order_relaxed);
    heap_[idx] = item;
    
    // Restore the heap property
    heapifyUp(idx);
    
    // Increment the size
    heapSize_.fetch_add(1, std::memory_order_release);
    
    // Release the mutex
    heapMutex_.clear(std::memory_order_release);
    
    return true;
}

template<typename T, size_t Capacity, typename Compare>
bool LockFreePriorityQueue<T, Capacity, Compare>::push(T&& item) {
    if (heapSize_.load(std::memory_order_acquire) >= Capacity) {
        return false; // Queue is full
    }
    
    // Try to acquire the heap mutex
    while (heapMutex_.test_and_set(std::memory_order_acquire)) {
        // Busy wait, but only for a very short time
        std::this_thread::yield();
    }
    
    // Add item to the end of the heap
    size_t idx = heapSize_.load(std::memory_order_relaxed);
    heap_[idx] = std::move(item);
    
    // Restore the heap property
    heapifyUp(idx);
    
    // Increment the size
    heapSize_.fetch_add(1, std::memory_order_release);
    
    // Release the mutex
    heapMutex_.clear(std::memory_order_release);
    
    return true;
}

template<typename T, size_t Capacity, typename Compare>
bool LockFreePriorityQueue<T, Capacity, Compare>::pop(T& item) {
    if (heapSize_.load(std::memory_order_acquire) == 0) {
        return false; // Queue is empty
    }
    
    // Try to acquire the heap mutex
    while (heapMutex_.test_and_set(std::memory_order_acquire)) {
        // Busy wait, but only for a very short time
        std::this_thread::yield();
    }
    
    if (heapSize_.load(std::memory_order_relaxed) == 0) {
        // Another thread might have consumed the last item
        heapMutex_.clear(std::memory_order_release);
        return false;
    }
    
    // The highest priority item is at the root (index 0)
    item = std::move(heap_[0]);
    
    // Move the last item to the root and restore heap property
    size_t currentSize = heapSize_.load(std::memory_order_relaxed);
    heap_[0] = std::move(heap_[currentSize - 1]);
    
    // Decrement the size
    heapSize_.fetch_sub(1, std::memory_order_release);
    
    if (heapSize_.load(std::memory_order_relaxed) > 0) {
        // Restore the heap property
        heapifyDown(0);
    }
    
    // Release the mutex
    heapMutex_.clear(std::memory_order_release);
    
    return true;
}

template<typename T, size_t Capacity, typename Compare>
bool LockFreePriorityQueue<T, Capacity, Compare>::peek(T& item) const {
    if (heapSize_.load(std::memory_order_acquire) == 0) {
        return false; // Queue is empty
    }
    
    // Try to acquire the heap mutex
    while (heapMutex_.test_and_set(std::memory_order_acquire)) {
        // Busy wait, but only for a very short time
        std::this_thread::yield();
    }
    
    if (heapSize_.load(std::memory_order_relaxed) > 0) {
        item = heap_[0];  // Root of the heap has highest priority
    }
    
    // Release the mutex
    heapMutex_.clear(std::memory_order_release);
    
    return heapSize_.load(std::memory_order_relaxed) > 0;
}

template<typename T, size_t Capacity, typename Compare>
bool LockFreePriorityQueue<T, Capacity, Compare>::empty() const {
    return heapSize_.load(std::memory_order_acquire) == 0;
}

template<typename T, size_t Capacity, typename Compare>
size_t LockFreePriorityQueue<T, Capacity, Compare>::size() const {
    return heapSize_.load(std::memory_order_acquire);
}

template<typename T, size_t Capacity, typename Compare>
void LockFreePriorityQueue<T, Capacity, Compare>::heapifyUp(size_t idx) {
    while (idx > 0) {
        size_t parentIdx = (idx - 1) / 2;
        if (!comp_(heap_[parentIdx], heap_[idx])) {
            break;  // Heap property satisfied
        }
        
        swap(idx, parentIdx);
        idx = parentIdx;
    }
}

template<typename T, size_t Capacity, typename Compare>
void LockFreePriorityQueue<T, Capacity, Compare>::heapifyDown(size_t idx) {
    size_t currentSize = heapSize_.load(std::memory_order_relaxed);
    
    while (true) {
        size_t leftChild = 2 * idx + 1;
        size_t rightChild = 2 * idx + 2;
        size_t largest = idx;
        
        if (leftChild < currentSize && comp_(heap_[largest], heap_[leftChild])) {
            largest = leftChild;
        }
        
        if (rightChild < currentSize && comp_(heap_[largest], heap_[rightChild])) {
            largest = rightChild;
        }
        
        if (largest == idx) {
            break;  // Heap property satisfied
        }
        
        swap(idx, largest);
        idx = largest;
    }
}

template<typename T, size_t Capacity, typename Compare>
void LockFreePriorityQueue<T, Capacity, Compare>::swap(size_t i, size_t j) {
    if (i != j) {
        T temp = std::move(heap_[i]);
        heap_[i] = std::move(heap_[j]);
        heap_[j] = std::move(temp);
    }
}

} // namespace AES67

#endif // LOCK_FREE_PRIORITY_QUEUE_H