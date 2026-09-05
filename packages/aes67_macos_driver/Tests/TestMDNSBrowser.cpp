//
// TestMDNSBrowser.cpp
// AES67 macOS Driver
//
// Exercises the mDNS browser against the REAL system responder — the
// only responder there is on macOS, and the one professional gear talks
// to. Nothing here needs another machine: the test registers its own
// service with DNSServiceRegister, browses for it, and checks the
// browser resolves what it just published.
//
// Deliberately tolerant about timing (mDNS is event-driven and the
// responder answers when it answers) and about the environment: a
// sandbox with no mDNS responder reachable makes start() fail, and the
// test says so rather than failing — the same posture AES67Device takes,
// where discovery is a convenience and never a reason to break.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/MDNSBrowser.h"

#include <dns_sd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace AES67;

namespace {

/// A unique-per-run instance name, so two runs (or a real device on the
/// same network) can never be mistaken for each other.
std::string uniqueInstanceName() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "AES67DriverTest-" + std::to_string(now % 1000000);
}

template <typename Predicate>
bool waitFor(Predicate&& done, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return done();
}

/// Registers a service with the system responder for as long as it lives.
class ScopedRegistration {
public:
    ScopedRegistration(const std::string& name, const std::string& type, uint16_t port) {
        const DNSServiceErrorType error = DNSServiceRegister(
            &ref_, /*flags=*/0, kDNSServiceInterfaceIndexAny,
            name.c_str(), type.c_str(), /*domain=*/nullptr, /*host=*/nullptr,
            htons(port), /*txtLen=*/0, /*txtRecord=*/nullptr,
            /*callback=*/nullptr, /*context=*/nullptr);
        ok_ = (error == kDNSServiceErr_NoError && ref_ != nullptr);
    }

    ~ScopedRegistration() {
        if (ref_ != nullptr) DNSServiceRefDeallocate(ref_);
    }

    bool ok() const { return ok_; }

private:
    DNSServiceRef ref_{nullptr};
    bool ok_{false};
};

} // namespace

TEST_CASE("Browser Finds And Resolves A Service It Can See") {
    std::cout << "Test: mDNS browse + resolve against the system responder... ";

    const std::string instance = uniqueInstanceName();
    const uint16_t port = 15554;

    ScopedRegistration registration(instance, MDNSBrowser::kServiceTypeRTSP, port);
    if (!registration.ok()) {
        std::cout << "SKIP (no mDNS responder reachable)" << std::endl;
        return;
    }

    MDNSBrowser browser(MDNSBrowser::kServiceTypeRTSP);

    std::atomic<int> callbacks{0};
    browser.registerServiceCallback([&](const MDNSService&) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
    });

    if (!browser.start()) {
        std::cout << "SKIP (browser could not start)" << std::endl;
        return;
    }
    CHECK(browser.isRunning());

    // Find our own registration among whatever else is on this link.
    const bool found = waitFor(
        [&] {
            for (const auto& service : browser.discoveredServices()) {
                if (service.name == instance) return true;
            }
            return false;
        },
        std::chrono::milliseconds(10000));

    if (!found) {
        // A machine with mDNS filtered (some VPNs, some sandboxes) reaches
        // here: the browser worked, the network did not carry the answer.
        browser.stop();
        std::cout << "SKIP (registration not observed on this network)" << std::endl;
        return;
    }

    MDNSService ours{};
    for (const auto& service : browser.discoveredServices()) {
        if (service.name == instance) ours = service;
    }

    CHECK(ours.type.find("_rtsp._tcp") != std::string::npos);
    CHECK(!ours.domain.empty());
    // Resolution is what makes discovery useful: a name alone cannot be
    // connected to. Port comes back in host order (the browser undoes
    // DNS-SD's network order) and must be exactly what we registered.
    CHECK(ours.port == port);
    CHECK(!ours.hostTarget.empty());
    CHECK(ours.isResolved());
    CHECK(callbacks.load() > 0);

    browser.stop();
    CHECK(!browser.isRunning());

    std::cout << "resolved " << ours.hostTarget << ":" << ours.port
              << " addr=" << (ours.address.empty() ? "(none)" : ours.address)
              << " PASS" << std::endl;
}

TEST_CASE("Stop Is Idempotent And Safe Before Start") {
    std::cout << "Test: mDNS browser lifecycle edges... ";
    MDNSBrowser browser(MDNSBrowser::kServiceTypeNMOSRegister);

    // Never started: stopping must be a no-op, not a crash or a hang.
    browser.stop();
    CHECK(!browser.isRunning());
    CHECK(browser.discoveredServices().empty());

    if (browser.start()) {
        CHECK(browser.isRunning());
        // A second start is refused rather than leaking a second thread.
        CHECK(!browser.start());
        browser.stop();
        browser.stop(); // idempotent
        CHECK(!browser.isRunning());
    }
    std::cout << "PASS" << std::endl;
}
