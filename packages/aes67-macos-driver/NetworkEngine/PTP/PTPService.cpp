#include "PTPService.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace AES67 {
namespace {

uint64_t MonotonicMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool FillAddress(const std::string& path, sockaddr_un* address) {
    std::memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (path.size() >= sizeof(address->sun_path)) return false;
    (void)std::snprintf(address->sun_path, sizeof(address->sun_path), "%s",
                  path.c_str());
    return true;
}

}  // namespace

// ============================================================================
// Server
// ============================================================================

PTPServiceServer::PTPServiceServer(std::string socketPath)
    : socketPath_(std::move(socketPath)) {}

PTPServiceServer::~PTPServiceServer() { stop(); }

bool PTPServiceServer::start() {
    if (running_.load(std::memory_order_acquire)) return false;

    sockaddr_un address;
    if (!FillAddress(socketPath_, &address)) {
        std::cerr << "[PTPService] Socket path too long: " << socketPath_
                  << '\n';
        return false;
    }

    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "[PTPService] socket(): " << std::strerror(errno)
                  << '\n';
        return false;
    }

    // A crash leaves the node behind and bind() would fail on it; the daemon
    // is the only writer of this path, so removing it is safe.
    ::unlink(socketPath_.c_str());

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
        std::cerr << "[PTPService] bind(" << socketPath_
                  << "): " << std::strerror(errno) << '\n';
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    // The reader is coreaudiod, not root. Nothing is ever read from a
    // client, so a connection is all anyone gains from this.
    ::chmod(socketPath_.c_str(), 0666);

    if (::listen(listenFd_, 8) != 0) {
        std::cerr << "[PTPService] listen(): " << std::strerror(errno)
                  << '\n';
        ::close(listenFd_);
        listenFd_ = -1;
        ::unlink(socketPath_.c_str());
        return false;
    }

    // Non-blocking accept, so stop() does not wait for a connection that
    // never comes.
    ::fcntl(listenFd_, F_SETFL, O_NONBLOCK);

    running_.store(true, std::memory_order_release);
    acceptThread_ = std::thread(&PTPServiceServer::acceptLoop, this);
    return true;
}

void PTPServiceServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (acceptThread_.joinable()) acceptThread_.join();

    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (int fd : clients_) ::close(fd);
        clients_.clear();
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    ::unlink(socketPath_.c_str());
}

void PTPServiceServer::acceptLoop() {
    while (running_.load(std::memory_order_acquire)) {
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd >= 0) {
            // A reader that stops reading must not block the daemon's
            // publish(): its writes fail instead and it gets dropped.
            ::fcntl(fd, F_SETFL, O_NONBLOCK);
            std::lock_guard<std::mutex> lock(clientsMutex_);
            if (clients_.size() >= PTPServiceServer::kMaxClients) {
                // The list is in accept order, so the front is the oldest
                // connection: the one that has had the longest run of the
                // status it came for.
                ::close(clients_.front());
                clients_.erase(clients_.begin());
            }
            clients_.push_back(fd);
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            std::cerr << "[PTPService] accept(): " << std::strerror(errno)
                      << '\n';
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void PTPServiceServer::publish(PTPServiceStatus status) {
    status.magic = kPTPServiceMagic;
    status.version = kPTPServiceVersion;
    status.length = sizeof(PTPServiceStatus);
    status.sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    status.publishedAtMs = MonotonicMs();

    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto it = clients_.begin(); it != clients_.end();) {
        const ssize_t written = ::send(*it, &status, sizeof(status),
#ifdef MSG_NOSIGNAL
                                       MSG_NOSIGNAL
#else
                                       0
#endif
        );
        if (written == static_cast<ssize_t>(sizeof(status))) {
            ++it;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Reader is behind; skip this round rather than drop it. The
            // next status supersedes this one anyway.
            ++it;
            continue;
        }
        ::close(*it);
        it = clients_.erase(it);
    }
}

int PTPServiceServer::clientCount() const {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    return static_cast<int>(clients_.size());
}

// ============================================================================
// Client
// ============================================================================

PTPServiceClient::PTPServiceClient(std::string socketPath)
    : socketPath_(std::move(socketPath)) {}

PTPServiceClient::~PTPServiceClient() { stop(); }

bool PTPServiceClient::start() {
    if (running_.load(std::memory_order_acquire)) return false;
    running_.store(true, std::memory_order_release);
    readThread_ = std::thread(&PTPServiceClient::readLoop, this);
    return true;
}

void PTPServiceClient::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (readThread_.joinable()) readThread_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool PTPServiceClient::connectOnce() {
    sockaddr_un address;
    if (!FillAddress(socketPath_, &address)) return false;

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))
        != 0) {
        ::close(fd);
        return false;
    }
    // Bounded read, so the loop notices stop() even while the daemon is
    // silent.
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    fd_ = fd;
    connectCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PTPServiceClient::readLoop() {
    PTPServiceStatus incoming{};
    size_t filled = 0;

    while (running_.load(std::memory_order_acquire)) {
        if (fd_ < 0) {
            if (!connectOnce()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            filled = 0;
        }

        auto* bytes = reinterpret_cast<uint8_t*>(&incoming);
        const ssize_t received =
            ::recv(fd_, bytes + filled, sizeof(incoming) - filled, 0);
        if (received > 0) {
            filled += static_cast<size_t>(received);
            if (filled < sizeof(incoming)) continue;   // partial, keep reading
            filled = 0;

            if (incoming.magic != kPTPServiceMagic
                || incoming.version != kPTPServiceVersion
                || incoming.length != sizeof(PTPServiceStatus)) {
                // A daemon from another build: refuse it outright rather than
                // read its fields as if they meant what this build thinks.
                rejectedCount_.fetch_add(1, std::memory_order_relaxed);
                ::close(fd_);
                fd_ = -1;
                continue;
            }
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_ = incoming;
            haveStatus_ = true;
            receivedAt_ = std::chrono::steady_clock::now();
            continue;
        }
        if (received == 0) {                      // daemon closed
            ::close(fd_);
            fd_ = -1;
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;                             // just quiet
        }
        ::close(fd_);
        fd_ = -1;
    }
}

bool PTPServiceClient::hasFreshStatus() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (!haveStatus_) return false;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - receivedAt_)
                         .count();
    return age <= kPTPServiceStaleMs;
}

bool PTPServiceClient::isLocked() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (!haveStatus_) return false;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - receivedAt_)
                         .count();
    if (age > kPTPServiceStaleMs) return false;
    return status_.locked != 0;
}

int64_t PTPServiceClient::getOffsetNs() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return haveStatus_ ? status_.offsetNs : 0;
}

int64_t PTPServiceClient::getPathDelayNs() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return haveStatus_ ? status_.pathDelayNs : 0;
}

double PTPServiceClient::getFrequencyDriftPpb() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return haveStatus_ ? status_.frequencyDriftPpb : 0.0;
}

uint8_t PTPServiceClient::getClockClass() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return haveStatus_ ? status_.clockClass : 255;
}

std::string PTPServiceClient::getGrandmasterID() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (!haveStatus_) return "";
    char text[32];
    (void)std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                  status_.grandmasterIdentity[0], status_.grandmasterIdentity[1],
                  status_.grandmasterIdentity[2], status_.grandmasterIdentity[3],
                  status_.grandmasterIdentity[4], status_.grandmasterIdentity[5],
                  status_.grandmasterIdentity[6], status_.grandmasterIdentity[7]);
    return text;
}

bool PTPServiceClient::lastStatus(PTPServiceStatus* out) const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (!haveStatus_ || out == nullptr) return false;
    *out = status_;
    return true;
}

}  // namespace AES67
