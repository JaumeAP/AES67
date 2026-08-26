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

// `resource` is parenthesised in the capture and `cleanup_expr` is not, on
// purpose: a lambda capture cannot take a parenthesised name, while the
// expression can and must, or an argument containing a comma or a low-priority
// operator would bind wrongly at the expansion site.
// `resource` names a variable to capture by reference, and a lambda capture list
// cannot take a parenthesised name -- so this one exclusion is not a style
// preference, it is what the language allows. The expression argument is
// parenthesised, which is the half that can be.
//
// The suppression is a begin/end pair rather than a next-line one because the
// warning lands on the macro body, not on the #define. And the directive names
// are kept out of this prose: clang-tidy scans comments for them literally, so
// writing one in a sentence opens a block that never closes.
//
// NOLINTBEGIN(bugprone-macro-parentheses)
#define WITH_CLEANUP(resource, cleanup_expr) \
    AES67::ResourceGuard guard([&resource]() { (cleanup_expr); })
// NOLINTEND(bugprone-macro-parentheses)

} // namespace AES67

#endif // RESOURCE_CLEANUP_MANAGER_H