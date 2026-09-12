#ifndef RTSP_SESSION_DISCOVERY_H
#define RTSP_SESSION_DISCOVERY_H

//
// RTSPSessionDiscovery
// AES67 macOS Driver
//
// The half of the network SAP does not hear: gear that registers
// `_rtsp._tcp` and hands its SDP to whoever asks with DESCRIBE. RAVENNA
// works this way -- Merging's driver resolves sessions through that service
// and announces nothing on the SAP group -- so a unit could be found by this
// driver's MDNSBrowser and still never reach the Manager app, which only ever
// saw the SAP list.
//
// This joins the two halves that already existed: MDNSBrowser finds the
// service and resolves it, SDPFetcher asks it to describe itself, and the
// result goes into the SessionDirectory beside the announced ones.
//
// The fetch happens on a worker of its own. MDNSBrowser's callback runs on
// the browser's thread and must not block, and a DESCRIBE is a socket round
// trip to a device that may be slow or gone: doing it inline would stall
// every other resolution behind the worst-behaved device on the link.
//

#include "NetworkEngine/Discovery/MDNSBrowser.h"
#include "NetworkEngine/Discovery/SessionDirectory.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace AES67 {

class RTSPSessionDiscovery {
public:
    /// How long a described session waits before it is asked again. A
    /// session's description can change (a channel count, a packet time),
    /// and nothing in RTSP pushes that: the only way to notice is to ask
    /// again. Well inside SessionDirectory::kSessionTimeout, so a device
    /// that is still registered never ages out of the list.
    static constexpr std::chrono::seconds kRefreshInterval{120};

    /// `directory` outlives this object: it is the device's, not ours.
    explicit RTSPSessionDiscovery(SessionDirectory& directory);
    ~RTSPSessionDiscovery();

    RTSPSessionDiscovery(const RTSPSessionDiscovery&) = delete;
    RTSPSessionDiscovery& operator=(const RTSPSessionDiscovery&) = delete;

    /// Starts browsing and the fetch worker. False when the system responder
    /// cannot be reached -- a lost convenience, never a reason to fail the
    /// driver, same as SAP discovery.
    bool start();
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    /// Describes one resolved service and files the result. Public and
    /// synchronous so the path from a service to a directory entry can be
    /// tested without a browser, a thread or a network: pass what a resolve
    /// would have produced, point `fetch` at anything, get the entry back.
    struct DescribeResult {
        bool ok{false};
        std::string error;
        DiscoveredSessionEntry entry;
    };

    using Fetcher = std::string (*)(const std::string& url, std::string& error);

    static DescribeResult describe(const MDNSService& service, Fetcher fetch);

private:
    void workerLoop();
    void enqueue(const MDNSService& service);

    SessionDirectory& directory_;
    MDNSBrowser browser_{MDNSBrowser::kServiceTypeRTSP};

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex queueMutex_;
    std::condition_variable queueSignal_;
    std::deque<MDNSService> queue_;
};

} // namespace AES67

#endif // RTSP_SESSION_DISCOVERY_H
