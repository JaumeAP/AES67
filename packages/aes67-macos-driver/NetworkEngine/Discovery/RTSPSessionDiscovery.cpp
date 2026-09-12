//
// RTSPSessionDiscovery.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/RTSPSessionDiscovery.h"

#include "Driver/SDPParser.h"
#include "NetworkEngine/Discovery/SDPFetcher.h"

#include <chrono>
#include <utility>

namespace AES67 {

namespace {

/// The default fetcher: a DESCRIBE through SDPFetcher, which owns the
/// timeouts and the size cap. A function pointer rather than a call inside
/// describe() so a test can put anything at the other end.
std::string fetchOverRtsp(const std::string& url, std::string& error) {
    const SDPFetchResult result = SDPFetcher::fetch(url);
    if (!result.ok()) {
        error = result.error;
        return {};
    }
    return result.text;
}

} // namespace

RTSPSessionDiscovery::RTSPSessionDiscovery(SessionDirectory& directory)
    : directory_(directory) {}

RTSPSessionDiscovery::~RTSPSessionDiscovery() {
    stop();
}

RTSPSessionDiscovery::DescribeResult RTSPSessionDiscovery::describe(const MDNSService& service,
                                                                   Fetcher fetch) {
    DescribeResult result;
    if (!service.isResolved()) {
        result.error = "service not resolved";
        return result;
    }

    // The instance name is the path RAVENNA publishes a session under, and
    // the service's own TXT record is what a device uses to say otherwise.
    // MDNSBrowser does not read TXT today, so the URL is built from what it
    // does resolve; a device that serves its description elsewhere answers
    // with an error and is simply not listed, which is better than listing
    // a session nobody can fetch.
    const std::string url = "rtsp://" + service.address + ":" +
                            std::to_string(service.port) + "/by-name/" + service.name;

    std::string error;
    const std::string sdp = fetch(url, error);
    if (sdp.empty()) {
        result.error = error.empty() ? "empty description" : error;
        return result;
    }

    const std::optional<SDPSession> parsed = SDPParser::parseString(sdp);
    if (!parsed) {
        result.error = "description did not parse";
        return result;
    }

    result.entry.sessionName = parsed->sessionName.empty() ? service.name : parsed->sessionName;
    result.entry.sourceAddress = service.address;
    result.entry.multicastAddress = parsed->connectionAddress;
    result.entry.port = static_cast<int>(parsed->port);
    result.entry.ptpDomain = parsed->ptpDomain;
    result.entry.sessionDescription = sdp;
    result.entry.sources = {DiscoverySource::RTSP};
    result.entry.identity = SessionDirectory::identityOf(sdp, result.entry.multicastAddress,
                                                         result.entry.port);
    result.ok = true;
    return result;
}

bool RTSPSessionDiscovery::start() {
    if (running_.load(std::memory_order_acquire)) return true;

    browser_.registerServiceCallback([this](const MDNSService& service) {
        // Browser thread: queue and return. Everything slow happens on the
        // worker.
        if (service.isResolved()) enqueue(service);
    });

    if (!browser_.start()) {
        return false;
    }

    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this]() { workerLoop(); });
    return true;
}

void RTSPSessionDiscovery::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    browser_.stop();
    queueSignal_.notify_all();
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.clear();
}

void RTSPSessionDiscovery::enqueue(const MDNSService& service) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        // One pending describe per service: a responder that repeats itself
        // should not make the worker ask the same device five times.
        for (const MDNSService& pending : queue_) {
            if (pending.name == service.name && pending.address == service.address &&
                pending.port == service.port) {
                return;
            }
        }
        queue_.push_back(service);
    }
    queueSignal_.notify_one();
}

void RTSPSessionDiscovery::workerLoop() {
    auto lastRefresh = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        MDNSService service;
        bool haveService = false;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueSignal_.wait_for(lock, std::chrono::seconds(1), [this]() {
                return !queue_.empty() || !running_.load(std::memory_order_acquire);
            });
            if (!running_.load(std::memory_order_acquire)) return;
            if (!queue_.empty()) {
                service = queue_.front();
                queue_.pop_front();
                haveService = true;
            }
        }

        if (haveService) {
            const DescribeResult result = describe(service, &fetchOverRtsp);
            if (result.ok) {
                directory_.offer(result.entry);
            }
            continue;
        }

        // Nothing waiting: ask again for what is still registered, so a
        // description that changed is noticed and a device that is still
        // there never ages out of the directory.
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRefresh < kRefreshInterval) continue;
        lastRefresh = now;

        for (const MDNSService& known : browser_.discoveredServices()) {
            if (known.isResolved()) enqueue(known);
        }
    }
}

} // namespace AES67
