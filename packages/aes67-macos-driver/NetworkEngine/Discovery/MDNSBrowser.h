#ifndef MDNS_BROWSER_H
#define MDNS_BROWSER_H

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AES67 {

/// One service instance seen on the local link over mDNS/DNS-SD.
///
/// SAP (SAPListener) hears whoever shouts their SDP on 224.2.127.254;
/// this hears whoever REGISTERS a service, which is how professional
/// AES67 gear is routinely published — Merging's RAVENNA driver resolves
/// sessions through `_rtsp._tcp` and registers with `_nmos-register._tcp`
/// (read off the installed 2.1.42605 bundle, 2026-08-31). Neither
/// mechanism sees everything the other does, which is exactly why both
/// belong here: a device that announces by SAP but registers no service,
/// and a device that registers but never announces, are both common.
///
/// What this deliberately does NOT do is fetch the SDP. Resolving gives
/// host and port; retrieving the description over RTSP DESCRIBE is
/// RTSPClient's job, and keeping that split means this class stays a
/// pure discovery surface with no protocol client inside it.
struct MDNSService {
    std::string name;        ///< Instance name, as registered ("Studio Mic 1").
    std::string type;        ///< Service type, e.g. "_rtsp._tcp".
    std::string domain;      ///< Usually "local."
    std::string hostTarget;  ///< Resolved host ("device.local.") — empty until resolved.
    std::string address;     ///< Resolved IPv4 literal — empty until resolved.
    uint16_t port{0};        ///< Resolved port — 0 until resolved.

    /// When the browser last heard about this instance. mDNS is
    /// event-driven rather than periodic, so an entry only disappears
    /// when the responder actively withdraws it (see kServiceTimeout for
    /// the backstop).
    std::chrono::steady_clock::time_point lastSeen{};

    bool isResolved() const { return port != 0 && !address.empty(); }
};

using MDNSServiceCallback = std::function<void(const MDNSService&)>;

/// Browses one DNS-SD service type on the local link and resolves what it
/// finds. Built on the system's dns_sd API (Bonjour), which every macOS
/// carries — no new dependency, and no second mDNS responder competing
/// with the system one for port 5353, which is what a hand-rolled
/// implementation would end up doing.
class MDNSBrowser {
public:
    /// The service types AES67 gear is published under. `_rtsp._tcp` is
    /// the one that matters today: a RAVENNA/AES67 sender registers it and
    /// serves its SDP over RTSP DESCRIBE at the resolved host and port.
    static constexpr const char* kServiceTypeRTSP = "_rtsp._tcp";
    /// NMOS registry, for the day IS-04 lands here — browsing for it costs
    /// nothing and tells a Manager app whether a registry exists at all.
    static constexpr const char* kServiceTypeNMOSRegister = "_nmos-register._tcp";

    /// A service that has not been heard from in this long is dropped.
    /// mDNS responders send goodbye packets on shutdown, so this is a
    /// backstop for the ungraceful case (a machine yanked off the
    /// network), not the normal removal path — hence generous.
    static constexpr std::chrono::seconds kServiceTimeout{600};

    explicit MDNSBrowser(std::string serviceType = kServiceTypeRTSP);
    ~MDNSBrowser();

    MDNSBrowser(const MDNSBrowser&) = delete;
    MDNSBrowser& operator=(const MDNSBrowser&) = delete;

    /// Starts browsing. Returns false when the system responder cannot be
    /// reached — like SAP discovery, that is a lost convenience and never
    /// a reason to fail the driver.
    bool start();
    void stop();
    bool isRunning() const;

    /// Called on every resolved service, from the browser's own thread.
    /// Register before start(); the callback must not block.
    void registerServiceCallback(const MDNSServiceCallback& callback);

    /// Everything currently known, expired entries swept as it reads —
    /// same contract as SAPListener::getDiscoveredStreams().
    std::vector<MDNSService> discoveredServices() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace AES67

#endif // MDNS_BROWSER_H
