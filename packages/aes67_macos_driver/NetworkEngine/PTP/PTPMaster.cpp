//
// PTPMaster.cpp
// AES67 macOS Driver
//
// Wire format mirrors PTPSlave.cpp exactly (same header layout, same
// Announce field offsets) — a master built here must interoperate with any
// standard PTP slave, including this driver's own PTPSlave.
//

#include "PTPMaster.h"
#include "NetworkEngine/NetworkUtils.h"

#include <cmath>
#include "Driver/AudioThreadPriority.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

namespace AES67 {

namespace {
    constexpr const char* kPTPPrimaryMulticast = "224.0.1.129";
    // Ports come from PTPMasterConfig since 2026-08-31 (overridable for
    // the unprivileged loopback test); these header/size constants stay.
    constexpr uint8_t  kPTPVersion = 2;
    constexpr uint16_t kFlagTwoStep = 0x0200;
    constexpr size_t   kMaxPTPMessageSize = 1500;

    // Same offsets as PTPSlave.cpp's Announce parser — kept in sync
    // deliberately, not shared, because a mismatch here would be caught
    // immediately by TestPTPMaster's round-trip test against PTPSlave's own
    // parser, which is exactly the point of that test.
    constexpr size_t kAnnounceGMPriority1Offset = 47;
    constexpr size_t kAnnounceGMClassOffset = 48;
    constexpr size_t kAnnounceGMAccuracyOffset = 49;
    constexpr size_t kAnnounceGMVarianceOffset = 50;
    constexpr size_t kAnnounceGMPriority2Offset = 52;
    constexpr size_t kAnnounceGMIdentityOffset = 53;
    constexpr size_t kAnnounceStepsRemovedOffset = 61;
    constexpr size_t kAnnounceTimeSourceOffset = 63;
    constexpr size_t kAnnounceMessageSize = 64;
    constexpr size_t kSyncMessageSize = 44;
    constexpr size_t kFollowUpMessageSize = 44;
    // Delay_Resp: header (34) + receiveTimestamp (10) + requestingPortIdentity
    // (10) — the exact layout PTPSlave::handleDelayResp parses.
    constexpr size_t kDelayRespMessageSize = 54;
}

namespace {

// logMessageInterval is log2 seconds (IEEE 1588-2008 sec 7.7.2.1). Announcing
// a rate other than the one actually sent is a lie a conforming slave acts
// on: it times its master-lost window and its Delay_Req rate off these
// fields, so they are derived from the configured intervals rather than
// hard-coded.
int8_t MsToLogInterval(int milliseconds) {
    if (milliseconds <= 0) return 0;
    const double seconds = static_cast<double>(milliseconds) / 1000.0;
    const long rounded = std::lround(std::log2(seconds));
    if (rounded < -128) return -128;
    if (rounded > 127) return 127;
    return static_cast<int8_t>(rounded);
}

// And back: the period the port actually waits, for the interval it
// announces. Nanoseconds because milliseconds cannot hold the fast rates --
// 16 Sync per second is 62.5 ms -- and because 2^n seconds is a whole number
// of nanoseconds for every n down to -9, which is 512 per second and well
// past anything a PTP port sends at. Below that it truncates, by under a
// nanosecond.
// AudioThreadPriority asks for a period in milliseconds as a double, which
// is a hint rather than a rate, so a fractional one is fine there.
double PeriodMs(std::chrono::nanoseconds period) {
    return std::chrono::duration<double, std::milli>(period).count();
}

std::chrono::nanoseconds LogIntervalToNs(int8_t logInterval) {
    constexpr int64_t kNsPerSecond = 1000000000;

    // The reachable range is much narrower than int8_t: MsToLogInterval's
    // input is a positive int of milliseconds, so the largest interval that
    // can be configured is about 24.9 days and what comes back is at most 21.
    // But the parameter is an int8_t, MsToLogInterval clamps to -128 and 127
    // rather than to what this can represent, and a shift of 127 on an int64
    // is undefined behaviour -- not a large number, undefined. Saying so in a
    // comment left it to be believed rather than enforced, which is what the
    // analyser was pointing at.
    //
    // 1e9 is just under 2^30, so a left shift of 33 is the last one that fits
    // in an int64; on the right, 63 shifts the value away to zero and there is
    // nothing beyond it to say. Neither bound is reachable through
    // MsToLogInterval, and clamping to them changes nothing that happens --
    // it only puts the limit where the compiler can see it.
    constexpr int kMaxLeftShift = 33;
    constexpr int kMaxRightShift = 63;

    if (logInterval >= 0) {
        const int shift = logInterval < kMaxLeftShift ? logInterval : kMaxLeftShift;
        return std::chrono::nanoseconds(kNsPerSecond << shift);
    }
    const int shift = -static_cast<int>(logInterval);
    return std::chrono::nanoseconds(
        kNsPerSecond >> (shift < kMaxRightShift ? shift : kMaxRightShift));
}

}  // namespace

// ============================================================================
// Construction / destruction
// ============================================================================

// The two intervals are settled here, once. The wire byte and the period the
// transmit loop waits are derived from the same exponent rather than each
// from config_ separately: while they were two numbers, a configured interval
// that is not a power of two seconds made this port announce one rate and
// send another -- 100 ms announced as 125 -- which is exactly what
// MsToLogInterval's comment above says the derivation exists to prevent.
//
// The price, and it is the honest one: a configured interval that is not a
// power of two seconds is now rounded to one and sent at that rate, instead
// of being sent at the configured rate and misdeclared.
PTPMaster::PTPMaster(const PTPMasterConfig& config, PTPClockSource& clockSource)
    : config_(config),
      clockSource_(clockSource),
      logSyncInterval_(MsToLogInterval(config.syncIntervalMs)),
      logAnnounceInterval_(MsToLogInterval(config.announceIntervalMs)),
      syncPeriod_(LogIntervalToNs(logSyncInterval_)),
      announcePeriod_(LogIntervalToNs(logAnnounceInterval_)) {}

PTPMaster::~PTPMaster() { stop(); }

// ============================================================================
// Lifecycle
// ============================================================================

bool PTPMaster::start() {
    if (running_.load(std::memory_order_acquire)) return false;

    uint8_t mac[6] = {0};
    if (getInterfaceMAC(mac)) {
        selfPortId_.clockIdentity = PTPClockIdentity::fromMAC(mac);
    } else {
        std::cerr << "[PTPMaster] Warning: could not get MAC for "
                  << config_.interfaceName << ", using fallback identity" << std::endl;
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int i = 0; i < 8; ++i)
            selfPortId_.clockIdentity.id[i] = static_cast<uint8_t>((now >> (i * 8)) & 0xFF);
    }
    selfPortId_.portNumber = 1;
    grandmasterIdentity_ = selfPortId_.clockIdentity;

    if (!createSockets()) {
        std::cerr << "[PTPMaster] Failed to create PTP sockets on "
                  << config_.interfaceName << std::endl;
        return false;
    }

    running_.store(true, std::memory_order_release);
    role_.store(PTPMasterRole::Listening, std::memory_order_release);
    startTime_ = std::chrono::steady_clock::now();

    receiveThread_ = std::thread(&PTPMaster::receiveThread, this);
    transmitThread_ = std::thread(&PTPMaster::transmitThread, this);

    return true;
}

void PTPMaster::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);

    if (receiveThread_.joinable()) receiveThread_.join();
    if (transmitThread_.joinable()) transmitThread_.join();

    closeSockets();
    role_.store(PTPMasterRole::Listening, std::memory_order_release);
}

// ============================================================================
// Sockets — same setup as PTPSlave::createSockets(), both sides of the
// same multicast group and ports (IEEE 1588-2008 §13.1)
// ============================================================================

bool PTPMaster::createSockets() {
    eventSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    generalSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (eventSocket_ < 0 || generalSocket_ < 0) {
        std::cerr << "[PTPMaster] Failed to create sockets: " << strerror(errno) << std::endl;
        closeSockets();
        return false;
    }

    for (int sock : {eventSocket_, generalSocket_}) {
        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
        struct timeval tv{0, 250000}; // 250ms — periodic check of running_
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        // The queue this master's Sync and Announce travel in. Unmarked
        // unless the configuration says otherwise.
        if (config_.dscp >= 0) {
            NetworkUtils::setQoSTrafficClass(sock, config_.dscp);
        }
    }

    // Bind BOTH sockets: general for foreign Announce (BMCA input), event
    // for inbound Delay_Req (2026-08-31 — unbound, the master could never
    // hear a slave's Delay_Req, so no slave could measure path delay).
    struct sockaddr_in genAddr{};
    genAddr.sin_family = AF_INET;
    genAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    genAddr.sin_port = htons(config_.generalPort);
    if (bind(generalSocket_, reinterpret_cast<struct sockaddr*>(&genAddr), sizeof(genAddr)) < 0) {
        std::cerr << "[PTPMaster] Failed to bind general socket: " << strerror(errno) << std::endl;
        closeSockets();
        return false;
    }
    struct sockaddr_in evtAddr{};
    evtAddr.sin_family = AF_INET;
    evtAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    evtAddr.sin_port = htons(config_.eventPort);
    if (bind(eventSocket_, reinterpret_cast<struct sockaddr*>(&evtAddr), sizeof(evtAddr)) < 0) {
        std::cerr << "[PTPMaster] Failed to bind event socket: " << strerror(errno) << std::endl;
        closeSockets();
        return false;
    }

    // Resolve the configured interface's IP for the multicast join/send-if.
    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(kPTPPrimaryMulticast);
    struct ifaddrs* ifaddrs_ptr = nullptr;
    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (struct ifaddrs* ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET &&
                config_.interfaceName == ifa->ifa_name) {
                mreq.imr_interface = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr)->sin_addr;
                break;
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }

    for (int sock : {generalSocket_, eventSocket_}) {
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            std::cerr << "[PTPMaster] Failed to join multicast: "
                      << strerror(errno) << std::endl;
            closeSockets();
            return false;
        }
    }

    unsigned char ttl = 1; // Same-segment only — no PTP boundary clock routing here.
    for (int sock : {eventSocket_, generalSocket_}) {
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &mreq.imr_interface, sizeof(mreq.imr_interface));
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }

    return true;
}

void PTPMaster::closeSockets() {
    if (eventSocket_ >= 0) { close(eventSocket_); eventSocket_ = -1; }
    if (generalSocket_ >= 0) { close(generalSocket_); generalSocket_ = -1; }
}

bool PTPMaster::getInterfaceMAC(uint8_t mac[6]) const {
    struct ifaddrs* ifaddrs_ptr = nullptr;
    if (getifaddrs(&ifaddrs_ptr) != 0) return false;
    bool found = false;
    for (struct ifaddrs* ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
        if (config_.interfaceName != ifa->ifa_name) continue;
        auto* sdl = reinterpret_cast<struct sockaddr_dl*>(ifa->ifa_addr);
        if (sdl->sdl_alen == 6) { std::memcpy(mac, LLADDR(sdl), 6); found = true; break; }
    }
    freeifaddrs(ifaddrs_ptr);
    return found;
}

// ============================================================================
// BMCA
// ============================================================================

PTPAnnounceData PTPMaster::ourAnnounceData() const {
    PTPAnnounceData data{};
    data.masterPortId = selfPortId_;
    data.grandmasterIdentity = grandmasterIdentity_;
    data.grandmasterClockClass = clockSource_.clockClass();
    data.grandmasterClockAccuracy = static_cast<uint8_t>(clockSource_.clockAccuracy());
    data.grandmasterOffsetScaledLogVariance = 0xFFFF; // unknown/worst — honest default, no variance estimator yet
    data.grandmasterPriority1 = config_.priority1;
    data.grandmasterPriority2 = config_.priority2;
    data.stepsRemoved = 0; // we are the grandmaster, not relaying
    data.timeSource = 0xA0; // INTERNAL_OSCILLATOR, §7.6.2.6 Table 7 — true for both clock sources today
    data.logAnnounceInterval = logAnnounceInterval_;
    return data;
}

std::optional<PTPAnnounceData> PTPMaster::currentCompetitor() const {
    std::lock_guard<std::mutex> lock(competitorMutex_);
    return competitor_;
}

void PTPMaster::evaluateBMCA() {
    // A clock that can't legally be a grandmaster (see PTPBMCA.h) never
    // transmits, full stop — no amount of network silence changes that.
    if (clockSource_.clockClass() == kPTPClockClassSlaveOnly) {
        role_.store(PTPMasterRole::Passive, std::memory_order_release);
        return;
    }

    std::optional<PTPAnnounceData> competitor;
    {
        std::lock_guard<std::mutex> lock(competitorMutex_);
        const auto timeout =
            announcePeriod_ * config_.announceReceiptTimeoutMultiplier;
        if (competitor_.has_value() &&
            std::chrono::steady_clock::now() - competitorLastSeen_ > timeout) {
            competitor_.reset(); // they went quiet — no longer a competitor
        }
        competitor = competitor_;
    }

    if (!competitor.has_value()) {
        // Nobody else heard, or they timed out — including during the
        // initial listen window, once it's elapsed.
        const auto listenWindow =
            announcePeriod_ * config_.announceReceiptTimeoutMultiplier;
        if (role_.load(std::memory_order_acquire) == PTPMasterRole::Listening &&
            std::chrono::steady_clock::now() - startTime_ < listenWindow) {
            return; // still in the initial listen window — wait it out
        }
        role_.store(PTPMasterRole::Master, std::memory_order_release);
        return;
    }

    const auto winner = bmcaCompare(ourAnnounceData(), *competitor);
    role_.store(winner == PTPBMCAWinner::A ? PTPMasterRole::Master : PTPMasterRole::Passive,
                std::memory_order_release);
}

void PTPMaster::handleForeignAnnounce(const PTPHeader& header, const uint8_t* data, size_t len) {
    if (header.domainNumber != static_cast<uint8_t>(config_.domain)) return;
    if (len < kAnnounceMessageSize) return;

    PTPAnnounceData announce{};
    announce.masterPortId = header.sourcePortIdentity;
    announce.grandmasterPriority1 = data[kAnnounceGMPriority1Offset];
    announce.grandmasterClockClass = data[kAnnounceGMClassOffset];
    announce.grandmasterClockAccuracy = data[kAnnounceGMAccuracyOffset];
    announce.grandmasterOffsetScaledLogVariance =
        static_cast<uint16_t>((data[kAnnounceGMVarianceOffset] << 8) | data[kAnnounceGMVarianceOffset + 1]);
    announce.grandmasterPriority2 = data[kAnnounceGMPriority2Offset];
    for (size_t i = 0; i < 8; ++i)
        announce.grandmasterIdentity.id[i] = data[kAnnounceGMIdentityOffset + i];
    announce.stepsRemoved =
        static_cast<uint16_t>((data[kAnnounceStepsRemovedOffset] << 8) | data[kAnnounceStepsRemovedOffset + 1]);
    announce.timeSource = data[kAnnounceTimeSourceOffset];
    announce.logAnnounceInterval = header.logMessageInterval;
    announce.lastReceived = std::chrono::steady_clock::now();

    // Hearing our own Announce looped back (multicast on the same host, or
    // a switch that reflects it) isn't a competitor.
    if (announce.grandmasterIdentity == grandmasterIdentity_) return;

    foreignAnnounceCount_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(competitorMutex_);
    // Keep whichever known-live foreign clock currently wins BMCA — not
    // just "the last one heard": with more than one foreign master on the
    // segment, always compare against the best of them, not whichever
    // happened to arrive most recently.
    if (!competitor_.has_value() || bmcaCompare(announce, *competitor_) == PTPBMCAWinner::A) {
        competitor_ = announce;
    }
    competitorLastSeen_ = announce.lastReceived;
}

// ============================================================================
// Threads
// ============================================================================

void PTPMaster::receiveThread() {
    AudioThreadPriority::configureForRealTime(PeriodMs(announcePeriod_));

    uint8_t buf[kMaxPTPMessageSize];
    while (running_.load(std::memory_order_acquire)) {
        // Both sockets, one thread: general carries foreign Announce (BMCA
        // input), event carries inbound Delay_Req. The 250 ms SO_RCVTIMEO
        // set in createSockets() keeps each read from blocking past the
        // running_ check; reading them alternately is enough at PTP rates
        // (a handful of messages per second per socket).
        for (int sock : {generalSocket_, eventSocket_}) {
            ssize_t received = recv(sock, buf, sizeof(buf), 0);
            if (received <= 0) continue; // timeout or error — loop re-checks running_

            if (static_cast<size_t>(received) < 34) continue; // shorter than a PTP header
            // t4 for a Delay_Req: sampled as close to reception as we get.
            const uint64_t receiptNs = clockSource_.currentTimeNs();

            PTPHeader header{};
            header.transportAndType = buf[0];
            header.versionPTP = buf[1];
            header.domainNumber = buf[4];
            header.logMessageInterval = static_cast<int8_t>(buf[33]);
            for (size_t i = 0; i < 8; ++i) header.sourcePortIdentity.clockIdentity.id[i] = buf[20 + i];
            header.sourcePortIdentity.portNumber = static_cast<uint16_t>((buf[28] << 8) | buf[29]);
            const uint16_t sequenceId = static_cast<uint16_t>((buf[30] << 8) | buf[31]);

            if (header.getMessageType() == PTPMessageType::Announce) {
                handleForeignAnnounce(header, buf, static_cast<size_t>(received));
            } else if (header.getMessageType() == PTPMessageType::Delay_Req) {
                if (header.domainNumber == static_cast<uint8_t>(config_.domain) &&
                    role_.load(std::memory_order_acquire) == PTPMasterRole::Master) {
                    handleDelayReq(header, sequenceId, receiptNs);
                }
            }
        }
    }
}

void PTPMaster::transmitThread() {
    AudioThreadPriority::configureForRealTime(PeriodMs(syncPeriod_));

    auto lastAnnounce = std::chrono::steady_clock::now() - announcePeriod_;
    auto lastSync = std::chrono::steady_clock::now();
    auto lastBMCATick = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    while (running_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();

        if (now - lastBMCATick >= std::chrono::milliseconds(200)) {
            evaluateBMCA();
            lastBMCATick = now;
        }

        if (role_.load(std::memory_order_acquire) == PTPMasterRole::Master) {
            if (now - lastAnnounce >= announcePeriod_) {
                sendAnnounce();
                lastAnnounce = now;
            }
            if (now - lastSync >= syncPeriod_) {
                sendSyncAndFollowUp();
                lastSync = now;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ============================================================================
// Message construction — mirrors PTPSlave.cpp's Delay_Req builder
// ============================================================================

bool PTPMaster::sendAnnounce() {
    if (generalSocket_ < 0) return false;

    uint8_t msg[kAnnounceMessageSize];
    std::memset(msg, 0, sizeof(msg));

    const uint16_t seqId = announceSequenceId_++;
    const PTPAnnounceData data = ourAnnounceData();

    msg[0] = static_cast<uint8_t>(PTPMessageType::Announce);
    msg[1] = kPTPVersion;
    msg[2] = 0; msg[3] = static_cast<uint8_t>(kAnnounceMessageSize);
    msg[4] = static_cast<uint8_t>(config_.domain);
    // flags = 0, correction = 0, reserved = 0

    for (int i = 0; i < 8; ++i) msg[20 + i] = selfPortId_.clockIdentity.id[i];
    msg[28] = static_cast<uint8_t>((selfPortId_.portNumber >> 8) & 0xFF);
    msg[29] = static_cast<uint8_t>(selfPortId_.portNumber & 0xFF);
    msg[30] = static_cast<uint8_t>((seqId >> 8) & 0xFF);
    msg[31] = static_cast<uint8_t>(seqId & 0xFF);
    msg[32] = 5; // controlField: Announce, IEEE 1588-2008 Table 23
    msg[33] = static_cast<uint8_t>(logAnnounceInterval_);

    // originTimestamp (34-43) and currentUtcOffset (44-45) left zero —
    // AES67 doesn't use them; every AES67 clock already treats PTP time as
    // TAI-equivalent per the Media Profile.
    msg[46] = 0; // reserved

    msg[kAnnounceGMPriority1Offset] = data.grandmasterPriority1;
    msg[kAnnounceGMClassOffset] = data.grandmasterClockClass;
    msg[kAnnounceGMAccuracyOffset] = data.grandmasterClockAccuracy;
    msg[kAnnounceGMVarianceOffset] = static_cast<uint8_t>((data.grandmasterOffsetScaledLogVariance >> 8) & 0xFF);
    msg[kAnnounceGMVarianceOffset + 1] = static_cast<uint8_t>(data.grandmasterOffsetScaledLogVariance & 0xFF);
    msg[kAnnounceGMPriority2Offset] = data.grandmasterPriority2;
    for (int i = 0; i < 8; ++i) msg[kAnnounceGMIdentityOffset + i] = data.grandmasterIdentity.id[i];
    msg[kAnnounceStepsRemovedOffset] = static_cast<uint8_t>((data.stepsRemoved >> 8) & 0xFF);
    msg[kAnnounceStepsRemovedOffset + 1] = static_cast<uint8_t>(data.stepsRemoved & 0xFF);
    msg[kAnnounceTimeSourceOffset] = data.timeSource;

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(kPTPPrimaryMulticast);
    dest.sin_port = htons(config_.generalPort);

    ssize_t sent = sendto(generalSocket_, msg, sizeof(msg), 0,
                          reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    if (sent < 0) {
        std::cerr << "[PTPMaster] Failed to send Announce: " << strerror(errno) << std::endl;
        return false;
    }
    announceSentCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool PTPMaster::sendSyncAndFollowUp() {
    if (eventSocket_ < 0 || generalSocket_ < 0) return false;

    const uint16_t seqId = syncSequenceId_++;

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(kPTPPrimaryMulticast);

    // --- Sync (two-step: origin timestamp is zero here, real time goes in
    //     Follow_Up — AES67-2018 requires two-step clocks, PTPSlave.h) ---
    uint8_t sync[kSyncMessageSize];
    std::memset(sync, 0, sizeof(sync));
    sync[0] = static_cast<uint8_t>(PTPMessageType::Sync);
    sync[1] = kPTPVersion;
    sync[2] = 0; sync[3] = static_cast<uint8_t>(kSyncMessageSize);
    sync[4] = static_cast<uint8_t>(config_.domain);
    sync[6] = static_cast<uint8_t>((kFlagTwoStep >> 8) & 0xFF);
    sync[7] = static_cast<uint8_t>(kFlagTwoStep & 0xFF);
    for (int i = 0; i < 8; ++i) sync[20 + i] = selfPortId_.clockIdentity.id[i];
    sync[28] = static_cast<uint8_t>((selfPortId_.portNumber >> 8) & 0xFF);
    sync[29] = static_cast<uint8_t>(selfPortId_.portNumber & 0xFF);
    sync[30] = static_cast<uint8_t>((seqId >> 8) & 0xFF);
    sync[31] = static_cast<uint8_t>(seqId & 0xFF);
    sync[32] = 0; // controlField: Sync
    // Sync's own interval, not Announce's.
    sync[33] = static_cast<uint8_t>(logSyncInterval_);

    // t1: our clock, sampled as close to the send() call as practical.
    const uint64_t t1 = clockSource_.currentTimeNs();

    dest.sin_port = htons(config_.eventPort);
    if (sendto(eventSocket_, sync, sizeof(sync), 0,
               reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) < 0) {
        std::cerr << "[PTPMaster] Failed to send Sync: " << strerror(errno) << std::endl;
        return false;
    }

    // --- Follow_Up: carries the precise t1 ---
    uint8_t followUp[kFollowUpMessageSize];
    std::memset(followUp, 0, sizeof(followUp));
    followUp[0] = static_cast<uint8_t>(PTPMessageType::Follow_Up);
    followUp[1] = kPTPVersion;
    followUp[2] = 0; followUp[3] = static_cast<uint8_t>(kFollowUpMessageSize);
    followUp[4] = static_cast<uint8_t>(config_.domain);
    for (int i = 0; i < 8; ++i) followUp[20 + i] = selfPortId_.clockIdentity.id[i];
    followUp[28] = static_cast<uint8_t>((selfPortId_.portNumber >> 8) & 0xFF);
    followUp[29] = static_cast<uint8_t>(selfPortId_.portNumber & 0xFF);
    followUp[30] = static_cast<uint8_t>((seqId >> 8) & 0xFF); // same sequenceId as the Sync it follows
    followUp[31] = static_cast<uint8_t>(seqId & 0xFF);
    followUp[32] = 2; // controlField: Follow_Up
    followUp[33] = 0;

    const PTPTimestamp t1ts(t1);
    followUp[34] = static_cast<uint8_t>((t1ts.secondsHi >> 8) & 0xFF);
    followUp[35] = static_cast<uint8_t>(t1ts.secondsHi & 0xFF);
    followUp[36] = static_cast<uint8_t>((t1ts.secondsLo >> 24) & 0xFF);
    followUp[37] = static_cast<uint8_t>((t1ts.secondsLo >> 16) & 0xFF);
    followUp[38] = static_cast<uint8_t>((t1ts.secondsLo >> 8) & 0xFF);
    followUp[39] = static_cast<uint8_t>(t1ts.secondsLo & 0xFF);
    followUp[40] = static_cast<uint8_t>((t1ts.nanoseconds >> 24) & 0xFF);
    followUp[41] = static_cast<uint8_t>((t1ts.nanoseconds >> 16) & 0xFF);
    followUp[42] = static_cast<uint8_t>((t1ts.nanoseconds >> 8) & 0xFF);
    followUp[43] = static_cast<uint8_t>(t1ts.nanoseconds & 0xFF);

    dest.sin_port = htons(config_.generalPort);
    if (sendto(generalSocket_, followUp, sizeof(followUp), 0,
               reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) < 0) {
        std::cerr << "[PTPMaster] Failed to send Follow_Up: " << strerror(errno) << std::endl;
        return false;
    }

    syncSentCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PTPMaster::handleDelayReq(const PTPHeader& header, uint16_t sequenceId, uint64_t t4Ns) {
    if (generalSocket_ < 0) return;

    uint8_t msg[kDelayRespMessageSize];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = static_cast<uint8_t>(PTPMessageType::Delay_Resp);
    msg[1] = kPTPVersion;
    msg[2] = 0; msg[3] = static_cast<uint8_t>(kDelayRespMessageSize);
    msg[4] = static_cast<uint8_t>(config_.domain);
    for (int i = 0; i < 8; ++i) msg[20 + i] = selfPortId_.clockIdentity.id[i];
    msg[28] = static_cast<uint8_t>((selfPortId_.portNumber >> 8) & 0xFF);
    msg[29] = static_cast<uint8_t>(selfPortId_.portNumber & 0xFF);
    // sequenceId echoes the Delay_Req's — that is how the slave matches
    // the answer to its own request (PTPSlave::handleDelayResp).
    msg[30] = static_cast<uint8_t>((sequenceId >> 8) & 0xFF);
    msg[31] = static_cast<uint8_t>(sequenceId & 0xFF);
    msg[32] = 3; // controlField: Delay_Resp, IEEE 1588-2008 Table 23
    // logMinDelayReqInterval: the rate this master asks Delay_Req at.
    msg[33] = static_cast<uint8_t>(config_.logMinDelayReqInterval);

    // receiveTimestamp: t4, when the Delay_Req reached us.
    const PTPTimestamp t4ts(t4Ns);
    msg[34] = static_cast<uint8_t>((t4ts.secondsHi >> 8) & 0xFF);
    msg[35] = static_cast<uint8_t>(t4ts.secondsHi & 0xFF);
    msg[36] = static_cast<uint8_t>((t4ts.secondsLo >> 24) & 0xFF);
    msg[37] = static_cast<uint8_t>((t4ts.secondsLo >> 16) & 0xFF);
    msg[38] = static_cast<uint8_t>((t4ts.secondsLo >> 8) & 0xFF);
    msg[39] = static_cast<uint8_t>(t4ts.secondsLo & 0xFF);
    msg[40] = static_cast<uint8_t>((t4ts.nanoseconds >> 24) & 0xFF);
    msg[41] = static_cast<uint8_t>((t4ts.nanoseconds >> 16) & 0xFF);
    msg[42] = static_cast<uint8_t>((t4ts.nanoseconds >> 8) & 0xFF);
    msg[43] = static_cast<uint8_t>(t4ts.nanoseconds & 0xFF);

    // requestingPortIdentity: the slave's own identity, copied from the
    // request — the slave rejects a Delay_Resp naming anyone else.
    for (int i = 0; i < 8; ++i) msg[44 + i] = header.sourcePortIdentity.clockIdentity.id[i];
    msg[52] = static_cast<uint8_t>((header.sourcePortIdentity.portNumber >> 8) & 0xFF);
    msg[53] = static_cast<uint8_t>(header.sourcePortIdentity.portNumber & 0xFF);

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(kPTPPrimaryMulticast);
    dest.sin_port = htons(config_.generalPort);

    if (sendto(generalSocket_, msg, sizeof(msg), 0,
               reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) < 0) {
        std::cerr << "[PTPMaster] Failed to send Delay_Resp: " << strerror(errno) << std::endl;
        return;
    }
    delayRespSentCount_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace AES67
