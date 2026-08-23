//
// PTPMaster.cpp
// AES67 macOS Driver
//
// Wire format mirrors PTPSlave.cpp exactly (same header layout, same
// Announce field offsets) — a master built here must interoperate with any
// standard PTP slave, including this driver's own PTPSlave.
//

#include "PTPMaster.h"
#include "../../Driver/AudioThreadPriority.h"

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
    constexpr uint16_t kPTPEventPort   = 319;
    constexpr uint16_t kPTPGeneralPort = 320;
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
}

// ============================================================================
// Construction / destruction
// ============================================================================

PTPMaster::PTPMaster(const PTPMasterConfig& config, PTPClockSource& clockSource)
    : config_(config), clockSource_(clockSource) {}

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
    }

    // Bind general socket (that's the one we also receive foreign Announce
    // on — Announce is a general message, IEEE 1588-2008 Table 15).
    struct sockaddr_in genAddr{};
    genAddr.sin_family = AF_INET;
    genAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    genAddr.sin_port = htons(kPTPGeneralPort);
    if (bind(generalSocket_, reinterpret_cast<struct sockaddr*>(&genAddr), sizeof(genAddr)) < 0) {
        std::cerr << "[PTPMaster] Failed to bind general socket: " << strerror(errno) << std::endl;
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

    if (setsockopt(generalSocket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::cerr << "[PTPMaster] Failed to join multicast on general socket: "
                  << strerror(errno) << std::endl;
        closeSockets();
        return false;
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
    data.logAnnounceInterval = 0; // 2^0 = 1s
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
        const auto timeout = std::chrono::milliseconds(
            config_.announceIntervalMs * config_.announceReceiptTimeoutMultiplier);
        if (competitor_.has_value() &&
            std::chrono::steady_clock::now() - competitorLastSeen_ > timeout) {
            competitor_.reset(); // they went quiet — no longer a competitor
        }
        competitor = competitor_;
    }

    if (!competitor.has_value()) {
        // Nobody else heard, or they timed out — including during the
        // initial listen window, once it's elapsed.
        const auto listenWindow = std::chrono::milliseconds(
            config_.announceIntervalMs * config_.announceReceiptTimeoutMultiplier);
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
    AudioThreadPriority::configureForRealTime(static_cast<double>(config_.announceIntervalMs));

    uint8_t buf[kMaxPTPMessageSize];
    while (running_.load(std::memory_order_acquire)) {
        ssize_t received = recv(generalSocket_, buf, sizeof(buf), 0);
        if (received <= 0) continue; // timeout or error — loop re-checks running_

        if (static_cast<size_t>(received) < 34) continue; // shorter than a PTP header

        PTPHeader header{};
        header.transportAndType = buf[0];
        header.versionPTP = buf[1];
        header.domainNumber = buf[4];
        header.logMessageInterval = static_cast<int8_t>(buf[33]);
        for (size_t i = 0; i < 8; ++i) header.sourcePortIdentity.clockIdentity.id[i] = buf[20 + i];
        header.sourcePortIdentity.portNumber = static_cast<uint16_t>((buf[28] << 8) | buf[29]);

        if (header.getMessageType() == PTPMessageType::Announce) {
            handleForeignAnnounce(header, buf, static_cast<size_t>(received));
        }
    }
}

void PTPMaster::transmitThread() {
    AudioThreadPriority::configureForRealTime(static_cast<double>(config_.syncIntervalMs));

    auto lastAnnounce = std::chrono::steady_clock::now() - std::chrono::milliseconds(config_.announceIntervalMs);
    auto lastSync = std::chrono::steady_clock::now();
    auto lastBMCATick = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    while (running_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();

        if (now - lastBMCATick >= std::chrono::milliseconds(200)) {
            evaluateBMCA();
            lastBMCATick = now;
        }

        if (role_.load(std::memory_order_acquire) == PTPMasterRole::Master) {
            if (now - lastAnnounce >= std::chrono::milliseconds(config_.announceIntervalMs)) {
                sendAnnounce();
                lastAnnounce = now;
            }
            if (now - lastSync >= std::chrono::milliseconds(config_.syncIntervalMs)) {
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
    msg[33] = 0; // logMessageInterval: 2^0 = 1s

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
    dest.sin_port = htons(kPTPGeneralPort);

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
    sync[33] = 0; // logMessageInterval: 2^0 = 1s (Sync's own interval, not Announce's)

    // t1: our clock, sampled as close to the send() call as practical.
    const uint64_t t1 = clockSource_.currentTimeNs();

    dest.sin_port = htons(kPTPEventPort);
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

    dest.sin_port = htons(kPTPGeneralPort);
    if (sendto(generalSocket_, followUp, sizeof(followUp), 0,
               reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) < 0) {
        std::cerr << "[PTPMaster] Failed to send Follow_Up: " << strerror(errno) << std::endl;
        return false;
    }

    syncSentCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace AES67
