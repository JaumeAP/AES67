//
// MDNSBrowser.cpp
// AES67 macOS Driver
//
// DNS-SD browsing over the system responder (dns_sd.h). The shape is
// SAPListener's — pimpl, one background thread, a mutex-guarded table
// that sweeps as it is read — because the two are siblings: both are
// best-effort discovery surfaces whose failure must never take the
// driver down with them.
//
// Why the system responder rather than our own mDNS implementation:
// macOS already runs mDNSResponder on port 5353, and a second responder
// on the same host fights it for the port. dns_sd is also the API
// Bonjour-registered gear expects to interoperate with, and it ships
// with every macOS — no dependency added.
//

#include "NetworkEngine/Discovery/MDNSBrowser.h"
#include "NetworkEngine/SelectWait.h"

#include <dns_sd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace AES67 {

namespace {
/// How long select() waits before the loop re-checks running_. Same
/// 250 ms cadence the PTP sockets use for the same reason: a stop()
/// must not wait on network traffic to be noticed.
constexpr int kSelectTimeoutMs = 250;

/// A resolve is a second, short-lived DNS-SD operation per instance.
/// Capping how long we wait for one keeps a silent responder from
/// pinning a slot forever.
constexpr int kResolveTimeoutSeconds = 5;
} // namespace

class MDNSBrowser::Impl {
public:
    explicit Impl(std::string serviceType) : serviceType_(std::move(serviceType)) {}

    ~Impl() { stop(); }

    bool start() {
        if (running_.load(std::memory_order_acquire)) return false;

        DNSServiceErrorType error = DNSServiceBrowse(
            &browseRef_, /*flags=*/0, kDNSServiceInterfaceIndexAny,
            serviceType_.c_str(), /*domain=*/nullptr, &Impl::browseReply, this);
        if (error != kDNSServiceErr_NoError || browseRef_ == nullptr) {
            browseRef_ = nullptr;
            return false;
        }

        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&Impl::run, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (thread_.joinable()) thread_.join();
        if (browseRef_ != nullptr) {
            DNSServiceRefDeallocate(browseRef_);
            browseRef_ = nullptr;
        }
    }

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    void registerCallback(const MDNSServiceCallback& callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = callback;
    }

    std::vector<MDNSService> services() {
        std::lock_guard<std::mutex> lock(mutex_);
        sweepLocked();
        std::vector<MDNSService> out;
        out.reserve(services_.size());
        for (const auto& entry : services_) out.push_back(entry.second);
        return out;
    }

private:
    /// The browse callback: an instance appeared or went away. Resolving
    /// happens inline on this thread — DNS-SD resolves are cheap and this
    /// thread has nothing else to do while it waits.
    static void DNSSD_API browseReply(DNSServiceRef, DNSServiceFlags flags,
                                      uint32_t interfaceIndex,
                                      DNSServiceErrorType errorCode,
                                      const char* serviceName,
                                      const char* regtype,
                                      const char* replyDomain,
                                      void* context) {
        auto* self = static_cast<Impl*>(context);
        if (self == nullptr || errorCode != kDNSServiceErr_NoError) return;

        const std::string key = std::string(serviceName) + "|" + regtype + "|" + replyDomain;

        if ((flags & kDNSServiceFlagsAdd) == 0) {
            // A goodbye: the responder withdrew the instance. Removing it
            // now is the normal path; kServiceTimeout only covers the host
            // that vanished without saying so.
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->services_.erase(key);
            return;
        }

        MDNSService service;
        service.name = serviceName;
        service.type = regtype;
        service.domain = replyDomain;
        service.lastSeen = std::chrono::steady_clock::now();
        self->resolve(service, interfaceIndex);
    }

    /// Turns an instance name into host/port/address.
    void resolve(MDNSService service, uint32_t interfaceIndex) {
        DNSServiceRef resolveRef = nullptr;
        DNSServiceErrorType error = DNSServiceResolve(
            &resolveRef, /*flags=*/0, interfaceIndex,
            service.name.c_str(), service.type.c_str(), service.domain.c_str(),
            &Impl::resolveReply, &service);
        if (error != kDNSServiceErr_NoError || resolveRef == nullptr) {
            // Unresolved is still worth recording: a Manager app can show
            // that something is there even when its host will not answer.
            store(service);
            return;
        }

        pump(resolveRef, kResolveTimeoutSeconds);
        DNSServiceRefDeallocate(resolveRef);

        if (service.port != 0 && service.address.empty()) {
            resolveAddress(service, interfaceIndex);
        }
        store(service);
    }

    static void DNSSD_API resolveReply(DNSServiceRef, DNSServiceFlags,
                                       uint32_t, DNSServiceErrorType errorCode,
                                       const char*, const char* hostTarget,
                                       uint16_t port, uint16_t, const unsigned char*,
                                       void* context) {
        auto* service = static_cast<MDNSService*>(context);
        if (service == nullptr || errorCode != kDNSServiceErr_NoError) return;
        service->hostTarget = hostTarget != nullptr ? hostTarget : "";
        service->port = ntohs(port); // DNS-SD hands the port in network order
    }

    /// Second hop: hostname -> IPv4 literal, so a caller can open a socket
    /// without a second name lookup of its own.
    void resolveAddress(MDNSService& service, uint32_t interfaceIndex) {
        if (service.hostTarget.empty()) return;
        DNSServiceRef addrRef = nullptr;
        DNSServiceErrorType error = DNSServiceGetAddrInfo(
            &addrRef, /*flags=*/0, interfaceIndex, kDNSServiceProtocol_IPv4,
            service.hostTarget.c_str(), &Impl::addressReply, &service);
        if (error != kDNSServiceErr_NoError || addrRef == nullptr) return;
        pump(addrRef, kResolveTimeoutSeconds);
        DNSServiceRefDeallocate(addrRef);
    }

    static void DNSSD_API addressReply(DNSServiceRef, DNSServiceFlags,
                                       uint32_t, DNSServiceErrorType errorCode,
                                       const char*, const struct sockaddr* address,
                                       uint32_t, void* context) {
        auto* service = static_cast<MDNSService*>(context);
        if (service == nullptr || errorCode != kDNSServiceErr_NoError) return;
        if (address == nullptr || address->sa_family != AF_INET) return;
        char text[INET_ADDRSTRLEN] = {0};
        const auto* in4 = reinterpret_cast<const struct sockaddr_in*>(address);
        if (inet_ntop(AF_INET, &in4->sin_addr, text, sizeof(text)) != nullptr) {
            service->address = text;
        }
    }

    /// Drives one DNS-SD operation's socket until it answers or the
    /// budget runs out. dns_sd never calls back on its own thread: a
    /// reply only arrives while someone processes the ref's socket.
    void pump(DNSServiceRef ref, int timeoutSeconds) {
        const int fd = DNSServiceRefSockFD(ref);
        if (fd < 0) return;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
        while (std::chrono::steady_clock::now() < deadline &&
               running_.load(std::memory_order_acquire)) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
            const SelectOutcome outcome = waitReadable(fd, &readfds, kSelectTimeoutMs);
            if (outcome == SelectOutcome::Failed) return;
            if (outcome == SelectOutcome::Timeout ||
                outcome == SelectOutcome::Interrupted) continue; // a signal is not an answer
            if (DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) return;
            return; // one reply is all any of these operations owes us
        }
    }

    void store(const MDNSService& service) {
        MDNSServiceCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::string key = service.name + "|" + service.type + "|" + service.domain;
            services_[key] = service;
            sweepLocked();
            callback = callback_;
        }
        // Outside the lock: a callback that reaches back into this object
        // (StreamManager does, through AES67Device) must not deadlock.
        if (callback) callback(service);
    }

    void sweepLocked() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = services_.begin(); it != services_.end();) {
            if (now - it->second.lastSeen > MDNSBrowser::kServiceTimeout) {
                it = services_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void run() {
        const int fd = DNSServiceRefSockFD(browseRef_);
        while (running_.load(std::memory_order_acquire)) {
            if (fd < 0) break;
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
            const SelectOutcome outcome = waitReadable(fd, &readfds, kSelectTimeoutMs);
            if (outcome == SelectOutcome::Failed) break;
            if (outcome == SelectOutcome::Timeout ||
                outcome == SelectOutcome::Interrupted) continue; // re-check running_
            if (DNSServiceProcessResult(browseRef_) != kDNSServiceErr_NoError) break;
        }
    }

    std::string serviceType_;
    DNSServiceRef browseRef_{nullptr};
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    std::map<std::string, MDNSService> services_;
    MDNSServiceCallback callback_;
};

MDNSBrowser::MDNSBrowser(std::string serviceType)
    : pimpl_(std::make_unique<Impl>(std::move(serviceType))) {}

MDNSBrowser::~MDNSBrowser() = default;

bool MDNSBrowser::start() { return pimpl_->start(); }
void MDNSBrowser::stop() { pimpl_->stop(); }
bool MDNSBrowser::isRunning() const { return pimpl_->isRunning(); }

void MDNSBrowser::registerServiceCallback(const MDNSServiceCallback& callback) {
    pimpl_->registerCallback(callback);
}

std::vector<MDNSService> MDNSBrowser::discoveredServices() const {
    return pimpl_->services();
}

} // namespace AES67
