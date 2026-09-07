#include "Grandmaster.h"

#include "Profiles/PtpIntervals.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace AES67::LinuxPtpd {
namespace {

using Clock = std::chrono::steady_clock;

// What to announce, and what it means. The box that does this against a word
// clock announces the same two, for the same reason: a reference that gives
// frequency and an edge per second carries no traceable absolute time, so
// clockClass 6 -- locked to a primary reference such as GPS -- would be a
// lie. 13 is synchronised to an application-specific source, which is what
// this is.
constexpr uint8_t kClockClassLocked = 13;
constexpr uint8_t kClockClassFree = 248;
constexpr uint8_t kTimeSourceLocked = 0x90;  // OTHER
constexpr uint8_t kTimeSourceFree = 0xA0;    // INTERNAL_OSCILLATOR

/// A log2-second interval as a duration. The conversion itself is the shared
/// one (Profiles/PtpIntervals.h): two implementations here already disagreed
/// about it once, and a third rule was not going to help.
std::chrono::nanoseconds intervalFrom(int8_t logSeconds) {
    return std::chrono::nanoseconds(ptpLogIntervalToNanoseconds(logSeconds));
}

}  // namespace

bool Grandmaster::start(std::string& error) {
    profile_ = ptpProfileByName(config_.profileName.c_str());
    if (profile_ == nullptr) {
        error = "no profile called " + config_.profileName +
                " (the table is packages/aes67-profiles)";
        return false;
    }

    // Zero is what the shared conversion returns for an exponent outside the
    // range 1588 allows. Pacing a send loop by it would mean sending as fast
    // as the loop turns, so it is refused here rather than discovered on the
    // wire.
    if (ptpLogIntervalToNanoseconds(profile_->settings.logSyncInterval) == 0 ||
        ptpLogIntervalToNanoseconds(profile_->settings.logAnnounceInterval) == 0) {
        error = std::string("profile ") + profile_->name +
                " carries an interval outside the range this daemon can send at";
        return false;
    }

    port_.clockIdentity = PTPClockIdentity::fromMAC(sockets_.mac().data());
    port_.portNumber = 1;
    port_.domainNumber = profile_->settings.domainNumber;
    port_.majorSdoId = profile_->settings.majorSdoId;

    dataset_.clockIdentity = port_.clockIdentity;
    dataset_.priority1 = config_.priority1;
    dataset_.priority2 = config_.priority2;
    dataset_.clockClass = kClockClassFree;
    dataset_.timeSource = kTimeSourceFree;
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
        return;
    }
    ++announcesSent_;
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
        return;
    }
    ++syncsSent_;
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
        return;
    }
    ++delayResponsesSent_;
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

void Grandmaster::followTheReference() {
    if (reference_ == nullptr) return;

    reference_->service();
    const ReferenceStatus& status = reference_->status();
    if (status.locked == announcedLocked_) return;

    // The dataset changes between one Announce and the next, which is exactly
    // when it should: a clock that has stopped being locked has to stop
    // saying so before the next device decides to follow it.
    announcedLocked_ = status.locked;
    dataset_.clockClass = status.locked ? kClockClassLocked : kClockClassFree;
    dataset_.timeSource = status.locked ? kTimeSourceLocked : kTimeSourceFree;

    std::printf("[ptpd] reference %s: offset %lld ns, drift %.1f ns/s, "
                "announcing clockClass %u\n",
                status.locked ? "locked" : "lost",
                static_cast<long long>(status.offsetNs), status.driftNsps,
                static_cast<unsigned>(dataset_.clockClass));
}

void Grandmaster::reportStatus() {
    // One line a second, whatever is happening, because a daemon that only
    // speaks when something is wrong is a daemon nobody can tell apart from a
    // stopped one. Counters and not rates: a rate averages a gap away.
    // Three states, and the first word of the line is which one: internal is
    // the NIC's own crystal, external is the pulse arriving on the PHC's
    // input, waiting is a reference that is configured and not being
    // followed -- no edge yet, the servo still settling, or the edges gone.
    const ReferenceStatus reference =
        reference_ != nullptr ? reference_->status() : ReferenceStatus{};

    std::printf("[ptpd] %s  clockClass %u  announce %llu  sync %llu  delay_resp %llu"
                "  no-followup %llu  %s",
                nameOf(reference.state()),
                static_cast<unsigned>(dataset_.clockClass),
                static_cast<unsigned long long>(announcesSent_),
                static_cast<unsigned long long>(syncsSent_),
                static_cast<unsigned long long>(delayResponsesSent_),
                static_cast<unsigned long long>(droppedFollowUps_),
                sockets_.hardwareTimestamps() ? "hw" : "sw");

    if (reference.available) {
        std::printf("  offset %+lld ns  drift %+.1f ns/s  edges %llu (%llu dropped,"
                    " last %.1f s ago)",
                    static_cast<long long>(reference.offsetNs), reference.driftNsps,
                    static_cast<unsigned long long>(reference.edges),
                    static_cast<unsigned long long>(reference.rejected),
                    reference.secondsSinceEdge);
    }
    std::printf("\n");
}

void Grandmaster::run(const std::atomic<bool>& running) {
    const auto announcePeriod = intervalFrom(profile_->settings.logAnnounceInterval);
    const auto syncPeriod = intervalFrom(profile_->settings.logSyncInterval);

    auto nextAnnounce = Clock::now();
    auto nextSync = Clock::now();
    auto nextStatus = Clock::now() + std::chrono::seconds(1);

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

        followTheReference();
        servicePort(sockets_.eventFd());
        servicePort(sockets_.generalFd());

        if (now >= nextStatus) {
            reportStatus();
            nextStatus += std::chrono::seconds(1);
            if (nextStatus < now) nextStatus = now + std::chrono::seconds(1);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

}  // namespace AES67::LinuxPtpd
