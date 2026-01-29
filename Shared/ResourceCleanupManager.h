#ifndef RESOURCE_CLEANUP_MANAGER_H
#define RESOURCE_CLEANUP_MANAGER_H

#include <functional>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>

namespace AES67 {

// Function type for cleanup operations
using CleanupFunction = std::function<void()>;

class ResourceCleanupManager {
public:
    ResourceCleanupManager();
    ~ResourceCleanupManager();
    
    // Register a resource for cleanup
    void registerCleanup(CleanupFunction cleanupFunc);
    
    // Perform all registered cleanup operations
    void performCleanup();
    
    // Perform cleanup and reset the manager
    void performCleanupAndReset();
    
    // Check if there are pending cleanup operations
    bool hasPendingCleanup() const;
    
    // Get the number of registered cleanup operations
    size_t getCleanupCount() const;
    
    // Execute a function with automatic cleanup registration
    template<typename Func, typename CleanupFunc>
    auto executeWithCleanup(Func&& func, CleanupFunc&& cleanup) -> decltype(func()) {
        registerCleanup(std::forward<CleanupFunc>(cleanup));
        return func();
    }

private:
    std::vector<CleanupFunction> cleanupFunctions_;
    mutable std::mutex cleanupMutex_;
    std::atomic<bool> cleanupInProgress_{false};
};

// RAII wrapper for automatic resource cleanup
template<typename CleanupFunc>
class ResourceGuard {
public:
    ResourceGuard(CleanupFunc&& cleanup)
        : cleanup_(std::forward<CleanupFunc>(cleanup)), active_(true) {}
    
    ~ResourceGuard() {
        if (active_) {
            cleanup_();
        }
    }
    
    // Release the guard (disable automatic cleanup)
    void release() { active_ = false; }
    
    // Explicitly trigger cleanup
    void cleanup() {
        if (active_) {
            cleanup_();
            active_ = false;
        }
    }

private:
    CleanupFunc cleanup_;
    bool active_;
};

// Specialized guards for common resource types
class SocketGuard {
public:
    SocketGuard(int socket_fd);
    ~SocketGuard();
    void release();
    void close();
    
private:
    int socket_fd_;
    bool active_;
};

class MemoryGuard {
public:
    MemoryGuard(void* ptr, std::function<void(void*)> deleter = [](void* p){ ::operator delete(p); });
    ~MemoryGuard();
    void release();
    void free();
    
private:
    void* ptr_;
    std::function<void(void*)> deleter_;
    bool active_;
};

// Global cleanup manager instance
extern std::unique_ptr<ResourceCleanupManager> g_resourceCleanupManager;

// Macro for easy resource cleanup registration
#define REGISTER_CLEANUP(func) \
    if (AES67::g_resourceCleanupManager) { \
        AES67::g_resourceCleanupManager->registerCleanup(func); \
    }

#define WITH_CLEANUP(resource, cleanup_expr) \
    AES67::ResourceGuard guard([&resource]() { cleanup_expr; })

} // namespace AES67

#endif // RESOURCE_CLEANUP_MANAGER_H