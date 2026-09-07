//
// Grandmaster.h
// AES67 Linux PTP daemon
// The port itself: what it sends, when, and what it answers.
//
// One role and one only. This announces itself as the grandmaster and never
// becomes a slave: there is no BMCA here, and a second master on the segment
// is a fault to report rather than a negotiation to enter. That is the same
// scope the Teensy box has, and the same honesty -- what is not implemented
// is not half implemented.
//
#pragma once

#include "ExternalReference.h"
#include "PhcClock.h"
#include "PtpSockets.h"
#include "PtpWire.h"

#include "Profiles/PtpProfiles.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace AES67::LinuxPtpd {

struct GrandmasterConfig {
    std::string interfaceName = "eth0";
    /// The name of a profile in packages/aes67-profiles: "aes67",
    /// "aes67-tight", "default1588" or "gptp". The five numbers it fixes are
    /// not repeated here.
    std::string profileName = "aes67";
    uint8_t priority1 = 128;
    uint8_t priority2 = 128;
    int16_t currentUtcOffset = 37;
    bool verbose = false;
};

class Grandmaster {
public:
    /// `reference` is optional: without one this announces a free-running
    /// clock, which is what the announced clockClass then says.
    Grandmaster(PtpSockets& sockets, PhcClock& clock, const GrandmasterConfig& config,
                ExternalReference* reference = nullptr)
        : sockets_(sockets), clock_(clock), config_(config), reference_(reference) {}

    /// Resolves the profile and builds the announce dataset from the clock.
    /// Fails when the profile name is not one of the shared table's.
    bool start(std::string& error);

    /// Runs until `running` goes false. One thread: at PTP rates there is
    /// nothing to gain from two, and a single thread is one fewer thing that
    /// can see a half-written dataset.
    void run(const std::atomic<bool>& running);

private:
    void sendAnnounce();
    void followTheReference();
    void sendSyncPair();
    void servicePort(int fd);
    void handleDelayReq(const PTPHeader& header, uint64_t receiveTimeNs);

    PtpSockets& sockets_;
    PhcClock& clock_;
    GrandmasterConfig config_;
    ExternalReference* reference_ = nullptr;
    bool announcedLocked_ = false;

    const PtpProfile* profile_ = nullptr;
    PortContext port_{};
    AnnounceDataset dataset_{};

    uint16_t announceSequence_ = 0;
    uint16_t syncSequence_ = 0;
    uint64_t droppedFollowUps_ = 0;
};

}  // namespace AES67::LinuxPtpd
