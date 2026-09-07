#include "Grandmaster.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace AES67::LinuxPtpd {
namespace {

using Clock = std::chrono::steady_clock;

/// A log2-second interval as a duration. IEEE 1588 carries the intervals as
/// exponents, and every rate in this daemon comes from one.
std::chrono::nanoseconds intervalFrom(int8_t logSeconds) {
    if (logSeconds >= 0) {
        return std::chrono::seconds(1LL << logSeconds);
    }
    return std::chrono::nanoseconds(1000000000LL >> (-logSeconds));
}

}  // namespace

bool Grandmaster::start(std::string& error) {
    profile_ = ptpProfileByName(config_.profileName.c_str());
    if (profile_ == nullptr) {
        error = "no profile called " + config_.profileName +
                " (the table is packages/aes67-profiles)";
        return false;
    }

    port_.clockIdentity = PTPClockIdentity::fromMAC(sockets_.mac().data());
    port_.portNumber = 1;
    port_.domainNumber = profile_->settings.domainNumber;
    port_.majorSdoId = profile_->settings.majorSdoId;

    dataset_.clockIdentity = port_.clockIdentity;
    dataset_.priority1 = config_.priority1;
    dataset_.priority2 = config_.priority2;
    dataset_.clockClass = clock_.clockClass();
    dataset_.clockAccuracy = static_cast<uint8_t>(clock_.clockAccuracy());
    dataset_.currentUtcOffset = config_.currentUtcOffset;
    // The offset is a number this machine was told, not one it derived, so it
    // is announced as present but not as valid. A slave that needs UTC takes
    // it from somewhere it can trust.
    dataset_.currentUtcOffsetValid = false;

    std::printf("[ptpd] %s on %s: domain %u, sync log %d, announce log %d, "
                "delayreq log %d, clock %s, %s timestamps\n",
                profile_->name, config_.interfaceName.c_str(),
                static_cast<unsigned>(port_.domainNumber),
                static_cast<int>(profile_->settings.logSyncInterval),
                static_cast<int>(profile_->settings.logAnnounceInterval),
                static_cast<int>(profile_->settings.logMinDelayReqInterval),
                clock_.name().c_str(),
                sockets_.hardwareTimestamps() ? "hardware" : "software");
    return true;
}

void Grandmaster::sendAnnounce() {
    uint8_t message[kAnnounceSize];
    const size_t length =
        buildAnnounce(message, sizeof(message), port_, dataset_, announceSequence_++,
                      profile_->settings.logAnnounceInterval, clock_.currentTimeNs());

    std::string error;
    if (!sockets_.sendGeneral(message, length, error)) {
        std::fprintf(stderr, "[ptpd] announce: %s\n", error.c_str());
    }
}

void Grandmaster::sendSyncPair() {
    const uint16_t sequence = syncSequence_++;

    uint8_t sync[kSyncSize];
    const size_t syncLength =
        buildSync(sync, sizeof(sync), port_, sequence, profile_->settings.logSyncInterval);

    std::string error;
    uint64_t transmitTimeNs = 0;
    if (!sockets_.sendEvent(sync, syncLength, transmitTimeNs, error)) {
        std::fprintf(stderr, "[ptpd] sync: %s\n", error.c_str());
        return;
    }

    if (transmitTimeNs == 0) {
        // No stamp came back. Sending a Follow_Up carrying a time we did not
        // measure would be worse than sending none: the slave would correct
        // itself against a number that is not when the Sync left.
        ++droppedFollowUps_;
        if (config_.verbose) {
            std::fprintf(stderr,
                         "[ptpd] sync %u went out with no transmit timestamp; "
                         "no Follow_Up sent (%llu so far)\n",
                         static_cast<unsigned>(sequence),
                         static_cast<unsigned long long>(droppedFollowUps_));
        }
        return;
    }

    uint8_t followUp[kFollowUpSize];
    const size_t followUpLength =
        buildFollowUp(followUp, sizeof(followUp), port_, sequence,
                      profile_->settings.logSyncInterval, transmitTimeNs);
    if (!sockets_.sendGeneral(followUp, followUpLength, error)) {
        std::fprintf(stderr, "[ptpd] follow_up: %s\n", error.c_str());
    }
}

void Grandmaster::handleDelayReq(const PTPHeader& header, uint64_t receiveTimeNs) {
    uint8_t response[kDelayRespSize];
    const size_t length =
        buildDelayResp(response, sizeof(response), port_, header.sourcePortIdentity,
                       header.sequenceId, profile_->settings.logMinDelayReqInterval,
                       receiveTimeNs);

    std::string error;
    if (!sockets_.sendGeneral(response, length, error)) {
        std::fprintf(stderr, "[ptpd] delay_resp: %s\n", error.c_str());
    }
}

void Grandmaster::servicePort(int fd) {
    uint8_t buffer[kMaxMessageSize];
    size_t length = 0;
    uint64_t receiveTimeNs = 0;

    while (sockets_.receive(fd, buffer, sizeof(buffer), length, receiveTimeNs)) {
        PTPHeader header{};
        if (!parseHeader(buffer, length, header)) continue;
        if (header.domainNumber != port_.domainNumber) continue;
        if (header.sourcePortIdentity.clockIdentity == port_.clockIdentity) continue;

        switch (header.getMessageType()) {
            case PTPMessageType::Delay_Req:
                // t2 is when the request arrived. A zero here means the NIC
                // gave us nothing, and answering with the software time would
                // put an unmeasured number in the slave's path delay.
                if (receiveTimeNs == 0) receiveTimeNs = clock_.currentTimeNs();
                handleDelayReq(header, receiveTimeNs);
                break;

            case PTPMessageType::Announce:
                // Another master is announcing on our domain. This daemon
                // does not arbitrate; it says so and keeps going, which is
                // the honest behaviour for something with no BMCA.
                if (config_.verbose) {
                    std::fprintf(stderr,
                                 "[ptpd] another Announce on domain %u from %s\n",
                                 static_cast<unsigned>(header.domainNumber),
                                 header.sourcePortIdentity.clockIdentity
                                     .toString().c_str());
                }
                break;

            default:
                break;
        }
    }
}

void Grandmaster::run(const std::atomic<bool>& running) {
    const auto announcePeriod = intervalFrom(profile_->settings.logAnnounceInterval);
    const auto syncPeriod = intervalFrom(profile_->settings.logSyncInterval);

    auto nextAnnounce = Clock::now();
    auto nextSync = Clock::now();

    while (running.load(std::memory_order_acquire)) {
        const auto now = Clock::now();

        if (now >= nextAnnounce) {
            sendAnnounce();
            nextAnnounce += announcePeriod;
            // A machine that was stopped (a suspend, a long stall) would
            // otherwise send a burst catching up on every message it missed.
            if (nextAnnounce < now) nextAnnounce = now + announcePeriod;
        }

        if (now >= nextSync) {
            sendSyncPair();
            nextSync += syncPeriod;
            if (nextSync < now) nextSync = now + syncPeriod;
        }

        servicePort(sockets_.eventFd());
        servicePort(sockets_.generalFd());

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

}  // namespace AES67::LinuxPtpd
