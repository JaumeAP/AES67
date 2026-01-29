#include "ResourceCleanupManager.h"
#include "NonBlockingLogger.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>

namespace AES67 {

// Global cleanup manager instance
std::unique_ptr<ResourceCleanupManager> g_resourceCleanupManager;

ResourceCleanupManager::ResourceCleanupManager() = default;

ResourceCleanupManager::~ResourceCleanupManager() {
    performCleanup();
}

void ResourceCleanupManager::registerCleanup(CleanupFunction cleanupFunc) {
    if (cleanupFunc) {
        std::lock_guard<std::mutex> lock(cleanupMutex_);
        cleanupFunctions_.push_back(std::move(cleanupFunc));
    }
}

void ResourceCleanupManager::performCleanup() {
    if (cleanupInProgress_.exchange(true)) {
        // Another thread is already performing cleanup
        return;
    }
    
    std::vector<CleanupFunction> localCleanupFunctions;
    
    {
        std::lock_guard<std::mutex> lock(cleanupMutex_);
        localCleanupFunctions.swap(cleanupFunctions_);
    }
    
    // Execute cleanup functions
    for (auto& cleanupFunc : localCleanupFunctions) {
        try {
            cleanupFunc();
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("Exception during cleanup: ") + e.what());
        } catch (...) {
            LOG_ERROR("Unknown exception during cleanup");
        }
    }
    
    cleanupInProgress_.store(false);
}

void ResourceCleanupManager::performCleanupAndReset() {
    performCleanup();
    
    std::lock_guard<std::mutex> lock(cleanupMutex_);
    cleanupFunctions_.clear();
}

bool ResourceCleanupManager::hasPendingCleanup() const {
    std::lock_guard<std::mutex> lock(cleanupMutex_);
    return !cleanupFunctions_.empty();
}

size_t ResourceCleanupManager::getCleanupCount() const {
    std::lock_guard<std::mutex> lock(cleanupMutex_);
    return cleanupFunctions_.size();
}

// SocketGuard implementation
SocketGuard::SocketGuard(int socket_fd) : socket_fd_(socket_fd), active_(true) {}

SocketGuard::~SocketGuard() {
    if (active_) {
        close();
    }
}

void SocketGuard::release() {
    active_ = false;
}

void SocketGuard::close() {
    if (active_ && socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
        active_ = false;
    }
}

// MemoryGuard implementation
MemoryGuard::MemoryGuard(void* ptr, std::function<void(void*)> deleter) 
    : ptr_(ptr), deleter_(std::move(deleter)), active_(true) {}

MemoryGuard::~MemoryGuard() {
    if (active_) {
        free();
    }
}

void MemoryGuard::release() {
    active_ = false;
}

void MemoryGuard::free() {
    if (active_ && ptr_) {
        deleter_(ptr_);
        ptr_ = nullptr;
        active_ = false;
    }
}

} // namespace AES67