//
// PTPSlave.cpp
// AES67 macOS Driver
// IEEE 1588-2008 PTP Slave-Only Implementation
//
// Implements the four-timestamp offset calculation:
//   t1 = Sync origin timestamp (from Follow_Up in two-step mode)
//   t2 = Sync receive timestamp (our local clock when Sync arrives)
//   t3 = Delay_Req send timestamp (our local clock when we send Delay_Req)
//   t4 = Delay_Req receive timestamp (from Delay_Resp)
//
//   offset    = ((t2 - t1) + (t3 - t4)) / 2
//   pathDelay = ((t2 - t1) - (t3 - t4)) / 2  (same as ((t2-t1)+(t4-t3))/2 )
//

#include "PTPSlave.h"
#include "PTPDiagnostics.h"
#include "NetworkEngine/NetworkUtils.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <unistd.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <chrono>

namespace AES67 {

// ============================================================================
// IEEE 1588 Constants
// ============================================================================

namespace {
    // PTP multicast addresses (IEEE 1588-2008 Section 13.1)
    constexpr const char* kPTPPrimaryMulticast = "224.0.1.129";   // Default domain
    // Peer delay uses its own group: 224.0.0.107, link-local by design so a
    // Pdelay exchange never crosses a bridge (IEEE 1588-2008 Annex D.3).
    constexpr const char* kPTPPeerDelayMulticast = "224.0.0.107";
    constexpr size_t kMinPdelayReqSize = 54;
    constexpr size_t kMinPdelayRespSize = 54;
    constexpr size_t kRequestingPortOffset = 44;

    // PTP UDP ports (IEEE 1588-2008 Section 13.1)
    // Ports come from PTPSlaveConfig since 2026-08-31 (loopback-test knob).

    // PTP header size (IEEE 1588-2008 Section 13.3)
    constexpr size_t kPTPHeaderSize = 34;

    // Message body offsets (after header)
    constexpr size_t kTimestampOffset = 34;  // Origin/receive timestamp starts at byte 34

    // Announce message offsets (after header)
    // Note: originTimestamp at offset 34 and currentUtcOffset at 44 are parsed
    // by position but not stored separately — their values are implicit in the
    // grandmaster clock quality fields that follow.
    constexpr size_t kAnnounceGMPriority1Offset = 47;
    constexpr size_t kAnnounceGMClassOffset = 48;
    constexpr size_t kAnnounceGMAccuracyOffset = 49;
    constexpr size_t kAnnounceGMVarianceOffset = 50;
    constexpr size_t kAnnounceGMPriority2Offset = 52;
    constexpr size_t kAnnounceGMIdentityOffset = 53;
    constexpr size_t kAnnounceStepsRemovedOffset = 61;
    constexpr size_t kAnnounceTimeSourceOffset = 63;

    // Minimum message sizes
    constexpr size_t kMinSyncSize = 44;
    constexpr size_t kMinFollowUpSize = 44;
    constexpr size_t kMinDelayRespSize = 54;
    constexpr size_t kMinAnnounceSize = 64;

    // PTP version
    constexpr uint8_t kPTPVersion = 2;

    // Flag field bits
    constexpr uint16_t kFlagTwoStep = 0x0200;

    // Max receive buffer
    constexpr size_t kMaxPTPMessageSize = 1500;

    // Announce timeout multiplier is configured via PTPSlaveConfig::announceTimeoutMultiplier
}

// ============================================================================
// PTPClockIdentity
// ============================================================================

std::string PTPClockIdentity::toString() const {
    std::ostringstream oss;
    for (size_t i = 0; i < 8; ++i) {
        if (i > 0) oss << ':';
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(id[i]);
    }
    return oss.str();
}

PTPClockIdentity PTPClockIdentity::fromMAC(const uint8_t mac[6]) {
    PTPClockIdentity cid;
    // EUI-48 to EUI-64 conversion (insert FF:FE in the middle)
    cid.id[0] = mac[0];
    cid.id[1] = mac[1];
    cid.id[2] = mac[2];
    cid.id[3] = 0xFF;
    cid.id[4] = 0xFE;
    cid.id[5] = mac[3];
    cid.id[6] = mac[4];
    cid.id[7] = mac[5];
    return cid;
}

// ============================================================================
// PTPSlave Construction / Destruction
// ============================================================================

PTPSlave::PTPSlave(const PTPSlaveConfig& config)
    : config_(config)
    , eventSocket_(-1)
    , generalSocket_(-1)
{
    offsetHistory_.fill(0);
    delayHistory_.fill(0);
}

PTPSlave::~PTPSlave() {
    stop();
}

// ============================================================================
// Lifecycle
// ============================================================================

bool PTPSlave::start() {
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }

    // Build our clock identity from the interface MAC
    uint8_t mac[6] = {0};
    if (getInterfaceMAC(mac)) {
        selfPortId_.clockIdentity = PTPClockIdentity::fromMAC(mac);
    } else {
        // Fallback: use random-ish identity
        std::cerr << "[PTPSlave] Warning: Could not get MAC for "
                  << config_.interfaceName << ", using fallback identity" << std::endl;
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int i = 0; i < 8; ++i) {
            selfPortId_.clockIdentity.id[i] = static_cast<uint8_t>((now >> (i * 8)) & 0xFF);
        }
    }
    selfPortId_.portNumber = config_.portNumber;

    // Create multicast sockets
    if (!createSockets()) {
        std::cerr << "[PTPSlave] Failed to create PTP sockets on "
                  << config_.interfaceName << std::endl;
        return false;
    }

    // Reset state
    hasMaster_ = false;
    locked_.store(false, std::memory_order_release);
    waitingForFollowUp_ = false;
    waitingForDelayResp_ = false;
    consecutiveGoodMeasurements_ = 0;
    offsetHistoryCount_ = 0;
    offsetHistoryIndex_ = 0;
    delayHistoryCount_ = 0;
    delayHistoryIndex_ = 0;
    lastDriftCalcTimeNs_ = 0;
    syncCount_.store(0, std::memory_order_relaxed);
    followUpCount_.store(0, std::memory_order_relaxed);
    delayReqSentCount_.store(0, std::memory_order_relaxed);
    delayRespCount_.store(0, std::memory_order_relaxed);
    announceCount_.store(0, std::memory_order_relaxed);
    domainMismatchCount_.store(0, std::memory_order_relaxed);

    running_.store(true, std::memory_order_release);

    std::cout << "[PTPSlave] Starting PTP slave on " << config_.interfaceName
              << " domain " << config_.domain
              << " (identity: " << selfPortId_.clockIdentity.toString() << ")"
              << std::endl;

    // Start receive thread
    receiveThread_ = std::thread(&PTPSlave::receiveThread, this);

    // Start delay request thread
    delayReqThread_ = std::thread(&PTPSlave::delayReqThread, this);

    return true;
}

void PTPSlave::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(false, std::memory_order_release);

    // Close sockets to unblock recv()
    closeSockets();

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    if (delayReqThread_.joinable()) {
        delayReqThread_.join();
    }

    locked_.store(false, std::memory_order_release);

    std::cout << "[PTPSlave] Stopped. Stats: sync=" << syncCount_.load()
              << " followUp=" << followUpCount_.load()
              << " delayReq=" << delayReqSentCount_.load()
              << " delayResp=" << delayRespCount_.load()
              << " announce=" << announceCount_.load()
              << std::endl;
}

// ============================================================================
// Status Queries
// ============================================================================

std::string PTPSlave::getGrandmasterID() const {
    std::lock_guard<std::mutex> lock(masterMutex_);
    if (hasMaster_) {
        return grandmasterIdentity_.toString();
    }
    return "";
}

void PTPSlave::setMeasurementCallback(PTPMeasurementCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    measurementCallback_ = std::move(cb);
}

void PTPSlave::updateDiagnostics(PTPDiagnostics& diag) const {
    diag.isLocked = locked_.load(std::memory_order_acquire);
    diag.currentOffset = static_cast<double>(offsetNs_.load(std::memory_order_acquire));
    diag.offsetNs = offsetNs_.load(std::memory_order_acquire);
    diag.frequencyOffset = frequencyDriftPpb_.load(std::memory_order_acquire) / 1000.0; // PPB to PPM
    diag.currentDomain = config_.domain;

    diag.syncMessagesReceived = syncCount_.load(std::memory_order_relaxed);
    diag.followUpMessagesReceived = followUpCount_.load(std::memory_order_relaxed);
    diag.delayReqMessagesSent = delayReqSentCount_.load(std::memory_order_relaxed);
    diag.delayRespMessagesReceived = delayRespCount_.load(std::memory_order_relaxed);
    diag.announceMessagesReceived = announceCount_.load(std::memory_order_relaxed);
    diag.domainMismatchErrors = domainMismatchCount_.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(masterMutex_);
        diag.isConnected = hasMaster_;
        if (hasMaster_) {
            diag.masterClockID = grandmasterIdentity_.toString();
            diag.clockClass = static_cast<int>(currentMaster_.grandmasterClockClass);
            diag.clockAccuracy = static_cast<int>(currentMaster_.grandmasterClockAccuracy);
        } else {
            diag.masterClockID = "";
        }
    }
}

// ============================================================================
// Socket Management
// ============================================================================

bool PTPSlave::createSockets() {
    // --- Event socket (port 319) ---
    eventSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (eventSocket_ < 0) {
        std::cerr << "[PTPSlave] Failed to create event socket: "
                  << strerror(errno) << std::endl;
        return false;
    }

    // Allow address reuse (multiple PTP instances or coexistence with other PTP software)
    int reuse = 1;
    setsockopt(eventSocket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(eventSocket_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    // Enable SO_TIMESTAMP for kernel-level receive timestamps
    int timestampOn = 1;
    setsockopt(eventSocket_, SOL_SOCKET, SO_TIMESTAMP, &timestampOn, sizeof(timestampOn));

    // Set receive timeout to allow periodic check of running_ flag
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 250000; // 250ms
    setsockopt(eventSocket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Bind to event port
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config_.eventPort);

    if (bind(eventSocket_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[PTPSlave] Failed to bind event socket to port "
                  << config_.eventPort << ": " << strerror(errno) << std::endl;
        closeSockets();
        return false;
    }

    // Join PTP multicast group on our interface
    struct ip_mreq mreq;
    std::memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(kPTPPrimaryMulticast);

    // Get interface IP address for the multicast join
    struct ifaddrs* ifaddrs_ptr = nullptr;
    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (struct ifaddrs* ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family == AF_INET &&
                config_.interfaceName == ifa->ifa_name) {
                mreq.imr_interface = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr)->sin_addr;
                break;
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }

    if (setsockopt(eventSocket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::cerr << "[PTPSlave] Failed to join multicast " << kPTPPrimaryMulticast
                  << " on event socket: " << strerror(errno) << std::endl;
        closeSockets();
        return false;
    }

    // Peer delay lives on its own group; joined only when it is the
    // configured mechanism, so an end-to-end setup sees no change.
    if (config_.delayMechanism == DelayMechanism::PeerToPeer) {
        struct ip_mreq peerMreq = mreq;
        peerMreq.imr_multiaddr.s_addr = inet_addr(kPTPPeerDelayMulticast);
        if (setsockopt(eventSocket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &peerMreq,
                       sizeof(peerMreq)) < 0) {
            std::cerr << "[PTPSlave] Failed to join multicast "
                      << kPTPPeerDelayMulticast << " on event socket: "
                      << strerror(errno) << std::endl;
            closeSockets();
            return false;
        }
    }

    // Set outgoing multicast interface
    setsockopt(eventSocket_, IPPROTO_IP, IP_MULTICAST_IF,
               &mreq.imr_interface, sizeof(mreq.imr_interface));

    // Set multicast TTL
    uint8_t ttl = 128;
    setsockopt(eventSocket_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Multicast loopback: off unless the config asks for it (see
    // PTPSlaveConfig::multicastLoopback for why the test needs it on).
    uint8_t loop = config_.multicastLoopback ? 1 : 0;
    setsockopt(eventSocket_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    // The queue this port's PTP travels in. Unmarked by default, which is
    // what it has always sent; a marked segment needs to be told.
    if (config_.dscp >= 0) {
        NetworkUtils::setQoSTrafficClass(eventSocket_, config_.dscp);
    }

    // --- General socket (port 320) ---
    generalSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (generalSocket_ < 0) {
        std::cerr << "[PTPSlave] Failed to create general socket: "
                  << strerror(errno) << std::endl;
        closeSockets();
        return false;
    }

    setsockopt(generalSocket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(generalSocket_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    // Receive timeout for general socket
    setsockopt(generalSocket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (config_.dscp >= 0) {
        NetworkUtils::setQoSTrafficClass(generalSocket_, config_.dscp);
    }

    // Bind to general port
    struct sockaddr_in gaddr;
    std::memset(&gaddr, 0, sizeof(gaddr));
    gaddr.sin_family = AF_INET;
    gaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    gaddr.sin_port = htons(config_.generalPort);

    if (bind(generalSocket_, reinterpret_cast<struct sockaddr*>(&gaddr), sizeof(gaddr)) < 0) {
        std::cerr << "[PTPSlave] Failed to bind general socket to port "
                  << config_.generalPort << ": " << strerror(errno) << std::endl;
        closeSockets();
        return false;
    }

    // Join multicast on general socket too
    if (setsockopt(generalSocket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::cerr << "[PTPSlave] Failed to join multicast " << kPTPPrimaryMulticast
                  << " on general socket: " << strerror(errno) << std::endl;
        closeSockets();
        return false;
    }

    // Pdelay_Resp_Follow_Up is a general message, so the peer group has to be
    // joined on this socket as well.
    if (config_.delayMechanism == DelayMechanism::PeerToPeer) {
        struct ip_mreq peerMreq = mreq;
        peerMreq.imr_multiaddr.s_addr = inet_addr(kPTPPeerDelayMulticast);
        if (setsockopt(generalSocket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &peerMreq,
                       sizeof(peerMreq)) < 0) {
            std::cerr << "[PTPSlave] Failed to join multicast "
                      << kPTPPeerDelayMulticast << " on general socket: "
                      << strerror(errno) << std::endl;
            closeSockets();
            return false;
        }
    }

    // The general socket was receive-only until peer delay gave it something
    // to send (Pdelay_Resp_Follow_Up), and a socket with no outgoing
    // multicast interface answers "No route to host".
    setsockopt(generalSocket_, IPPROTO_IP, IP_MULTICAST_IF,
               &mreq.imr_interface, sizeof(mreq.imr_interface));
    setsockopt(generalSocket_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(generalSocket_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    std::cout << "[PTPSlave] Sockets created: event=" << config_.eventPort
              << " general=" << config_.generalPort
              << " multicast=" << kPTPPrimaryMulticast << std::endl;

    return true;
}

void PTPSlave::closeSockets() {
    if (eventSocket_ >= 0) {
        close(eventSocket_);
        eventSocket_ = -1;
    }
    if (generalSocket_ >= 0) {
        close(generalSocket_);
        generalSocket_ = -1;
    }
}

// ============================================================================
// Receive Thread — handles Sync (event), Follow_Up (general), Announce
// ============================================================================

void PTPSlave::receiveThread() {
    uint8_t buf[kMaxPTPMessageSize];

    while (running_.load(std::memory_order_acquire)) {
        // Use select() to monitor both sockets
        fd_set readfds;
        FD_ZERO(&readfds);

        int maxfd = -1;
        if (eventSocket_ >= 0) {
            FD_SET(eventSocket_, &readfds);
            maxfd = std::max(maxfd, eventSocket_);
        }
        if (generalSocket_ >= 0) {
            FD_SET(generalSocket_, &readfds);
            maxfd = std::max(maxfd, generalSocket_);
        }

        if (maxfd < 0) break;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 250000; // 250ms timeout

        int ret = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue; // Timeout

        // Check event socket (Sync; Delay_Resp is tolerated here too --
        // it belongs on the general socket per Table 15, and that is
        // where it is really handled since 2026-08-31, but accepting it
        // on 319 costs nothing and covers a master that misplaces it)
        if (eventSocket_ >= 0 && FD_ISSET(eventSocket_, &readfds)) {
            // Use recvmsg to get kernel timestamp
            struct msghdr msg;
            struct iovec iov;
            char control[256];

            iov.iov_base = buf;
            iov.iov_len = sizeof(buf);
            msg.msg_name = nullptr;
            msg.msg_namelen = 0;
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);
            msg.msg_flags = 0;

            ssize_t n = recvmsg(eventSocket_, &msg, 0);
            if (n >= static_cast<ssize_t>(kPTPHeaderSize)) {
                // Extract kernel receive timestamp if available
                uint64_t receiveTimeNs = getSystemTimeNs(); // fallback
                struct cmsghdr* cmsg;
                for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
                     cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMP) {
                        struct timeval* tvp = reinterpret_cast<struct timeval*>(CMSG_DATA(cmsg));
                        receiveTimeNs = static_cast<uint64_t>(tvp->tv_sec) * 1000000000ULL +
                                        static_cast<uint64_t>(tvp->tv_usec) * 1000ULL;
                        break;
                    }
                }

                PTPHeader header;
                if (parseHeader(buf, static_cast<size_t>(n), header)) {
                    // Profile check: a gPTP master (majorSdoId 1) on this
                    // domain used to be followed as if it were ours.
                    if (config_.enforceMajorSdoId
                        && header.getMajorSdoId() != config_.majorSdoId) {
                        sdoIdMismatchCount_.fetch_add(1, std::memory_order_relaxed);
                    } else if (header.domainNumber != config_.domain) {
                        domainMismatchCount_.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        switch (header.getMessageType()) {
                            case PTPMessageType::Sync:
                                handleSync(header, buf, static_cast<size_t>(n), receiveTimeNs);
                                break;
                            case PTPMessageType::Delay_Resp:
                                handleDelayResp(header, buf, static_cast<size_t>(n));
                                break;
                            case PTPMessageType::Pdelay_Req:
                                handlePdelayReq(header, buf, static_cast<size_t>(n),
                                                receiveTimeNs);
                                break;
                            case PTPMessageType::Pdelay_Resp:
                                handlePdelayResp(header, buf, static_cast<size_t>(n),
                                                 receiveTimeNs);
                                break;
                            default:
                                break;
                        }
                    }
                }
            }
        }

        // Check general socket (Follow_Up, Announce)
        if (generalSocket_ >= 0 && FD_ISSET(generalSocket_, &readfds)) {
            ssize_t n = recv(generalSocket_, buf, sizeof(buf), 0);
            if (n >= static_cast<ssize_t>(kPTPHeaderSize)) {
                PTPHeader header;
                if (parseHeader(buf, static_cast<size_t>(n), header)) {
                    // Profile check: a gPTP master (majorSdoId 1) on this
                    // domain used to be followed as if it were ours.
                    if (config_.enforceMajorSdoId
                        && header.getMajorSdoId() != config_.majorSdoId) {
                        sdoIdMismatchCount_.fetch_add(1, std::memory_order_relaxed);
                    } else if (header.domainNumber != config_.domain) {
                        domainMismatchCount_.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        switch (header.getMessageType()) {
                            case PTPMessageType::Follow_Up:
                                handleFollowUp(header, buf, static_cast<size_t>(n));
                                break;
                            case PTPMessageType::Announce:
                                handleAnnounce(header, buf, static_cast<size_t>(n));
                                break;
                            case PTPMessageType::Pdelay_Resp_FU:
                                handlePdelayRespFollowUp(header, buf,
                                                         static_cast<size_t>(n));
                                break;
                            case PTPMessageType::Delay_Resp:
                                // Delay_Resp is a GENERAL message (IEEE
                                // 1588-2008 Table 15, port 320) -- it was
                                // only handled on the event socket until
                                // 2026-08-31, so a standards-compliant
                                // grandmaster's answer was silently
                                // dropped and the slave could never
                                // measure path delay. Found by the new
                                // PTP loopback test: master sent 21
                                // Delay_Resp, slave counted 0.
                                handleDelayResp(header, buf, static_cast<size_t>(n));
                                break;
                            default:
                                break;
                        }
                    }
                }
            }
        }

        // Check for announce timeout (master lost)
        {
            std::lock_guard<std::mutex> lock(masterMutex_);
            if (hasMaster_) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - currentMaster_.lastReceived).count();
                // Timed against the interval the master announces, not the
                // one configured here: a grandmaster sending Announce every
                // 2 s was declared lost after 3 s.
                int announceIntervalMs = config_.announceIntervalMs;
                if (config_.followAdvertisedIntervals) {
                    const int advertised = advertisedAnnounceIntervalMs_.load(
                        std::memory_order_relaxed);
                    if (advertised > 0) announceIntervalMs = advertised;
                }
                int timeoutMs = announceIntervalMs * config_.announceTimeoutMultiplier;
                if (elapsed > timeoutMs) {
                    std::cerr << "[PTPSlave] Announce timeout — master lost after "
                              << elapsed << "ms" << std::endl;
                    hasMaster_ = false;
                    locked_.store(false, std::memory_order_release);
                    consecutiveGoodMeasurements_ = 0;
                }
            }
        }
    }
}

// ============================================================================
// Delay Request Thread
// ============================================================================

void PTPSlave::delayReqThread() {
    while (running_.load(std::memory_order_acquire)) {
        // End to end needs a master to ask: the exchange is with it, and its
        // answer is meaningless before the first Sync. Peer delay does not --
        // it measures the link to the neighbour, which is a property of the
        // cable and is measured whether or not a grandmaster has been chosen
        // (IEEE 1588-2008 sec 11.4.1).
        bool shouldSend = config_.delayMechanism == DelayMechanism::PeerToPeer;
        if (!shouldSend) {
            {
                std::lock_guard<std::mutex> lock(masterMutex_);
                shouldSend = hasMaster_;
            }
            {
                std::lock_guard<std::mutex> lock(syncMutex_);
                shouldSend = shouldSend
                             && (syncCount_.load(std::memory_order_relaxed) > 0);
            }
        }

        if (shouldSend) {
            if (config_.delayMechanism == DelayMechanism::PeerToPeer) {
                sendPdelayReq();
            } else {
                sendDelayReq();
            }
        }

        // The advertised rate wins over the configured one once a
        // Delay_Resp has carried it; the configured value is what this slave
        // uses until then, and if the master advertises nothing usable.
        int intervalMs = config_.delayReqIntervalMs;
        if (config_.followAdvertisedIntervals) {
            const int advertised =
                advertisedDelayReqIntervalMs_.load(std::memory_order_relaxed);
            if (advertised > 0) intervalMs = advertised;
        }
        auto sleepTime = std::chrono::milliseconds(intervalMs);
        auto deadline = std::chrono::steady_clock::now() + sleepTime;

        while (running_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}


// Path delay, filtered the same way whichever mechanism measured it: keep the
// minimum of the recent history, since queueing only ever adds to a
// measurement and the smallest sample is the one least contaminated by it.
void PTPSlave::storeFilteredPathDelay(int64_t delayNs) {
    if (delayNs < 0) return;
    delayHistory_[delayHistoryIndex_] = delayNs;
    delayHistoryIndex_ = (delayHistoryIndex_ + 1) % kDelayFilterSize;
    if (delayHistoryCount_ < kDelayFilterSize) delayHistoryCount_++;

    int64_t minDelay = delayNs;
    for (size_t i = 0; i < delayHistoryCount_; ++i) {
        minDelay = std::min(minDelay, delayHistory_[i]);
    }
    pathDelayNs_.store(minDelay, std::memory_order_release);
}

// ============================================================================
// Peer delay (IEEE 1588-2008 sec 11.4)
// ============================================================================

namespace {

// The header every peer-delay message shares. `length` is the message length
// the type carries; `logInterval` is 0x7F for the ones the standard says
// carry no rate.
void BuildPeerHeader(uint8_t* msg, PTPMessageType type, size_t length,
                     uint8_t domain, const PTPPortIdentity& source,
                     uint16_t sequenceId, int8_t logInterval, bool twoStep) {
    std::memset(msg, 0, length);
    msg[0] = static_cast<uint8_t>(type);
    msg[1] = kPTPVersion;
    msg[2] = static_cast<uint8_t>((length >> 8) & 0xFF);
    msg[3] = static_cast<uint8_t>(length & 0xFF);
    msg[4] = domain;
    if (twoStep) msg[6] = 0x02;      // flagField octet 0, twoStepFlag
    for (int i = 0; i < 8; ++i) msg[20 + i] = source.clockIdentity.id[i];
    msg[28] = static_cast<uint8_t>((source.portNumber >> 8) & 0xFF);
    msg[29] = static_cast<uint8_t>(source.portNumber & 0xFF);
    msg[30] = static_cast<uint8_t>((sequenceId >> 8) & 0xFF);
    msg[31] = static_cast<uint8_t>(sequenceId & 0xFF);
    msg[32] = 5;                     // controlField: "all others", Table 23
    msg[33] = static_cast<uint8_t>(logInterval);
}

void WriteTimestamp(uint8_t* msg, size_t offset, uint64_t nanoseconds) {
    const PTPTimestamp ts(nanoseconds);
    msg[offset]     = static_cast<uint8_t>((ts.secondsHi >> 8) & 0xFF);
    msg[offset + 1] = static_cast<uint8_t>(ts.secondsHi & 0xFF);
    msg[offset + 2] = static_cast<uint8_t>((ts.secondsLo >> 24) & 0xFF);
    msg[offset + 3] = static_cast<uint8_t>((ts.secondsLo >> 16) & 0xFF);
    msg[offset + 4] = static_cast<uint8_t>((ts.secondsLo >> 8) & 0xFF);
    msg[offset + 5] = static_cast<uint8_t>(ts.secondsLo & 0xFF);
    msg[offset + 6] = static_cast<uint8_t>((ts.nanoseconds >> 24) & 0xFF);
    msg[offset + 7] = static_cast<uint8_t>((ts.nanoseconds >> 16) & 0xFF);
    msg[offset + 8] = static_cast<uint8_t>((ts.nanoseconds >> 8) & 0xFF);
    msg[offset + 9] = static_cast<uint8_t>(ts.nanoseconds & 0xFF);
}

void WritePortIdentity(uint8_t* msg, size_t offset, const PTPPortIdentity& id) {
    for (int i = 0; i < 8; ++i) msg[offset + i] = id.clockIdentity.id[i];
    msg[offset + 8] = static_cast<uint8_t>((id.portNumber >> 8) & 0xFF);
    msg[offset + 9] = static_cast<uint8_t>(id.portNumber & 0xFF);
}

}  // namespace

bool PTPSlave::sendPdelayReq() {
    if (eventSocket_ < 0) return false;

    uint16_t seqId;
    {
        std::lock_guard<std::mutex> lock(pdelayMutex_);
        seqId = ++pdelayReqSequenceId_;
    }

    uint8_t msg[kMinPdelayReqSize];
    BuildPeerHeader(msg, PTPMessageType::Pdelay_Req, sizeof(msg),
                    static_cast<uint8_t>(config_.domain), selfPortId_, seqId,
                    config_.logMinPdelayReqInterval, false);
    // originTimestamp is left zero: t1 is our own send time, taken below, and
    // the responder never reads the field (sec 11.4.2).

    struct sockaddr_in dest;
    std::memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(kPTPPeerDelayMulticast);
    dest.sin_port = htons(config_.eventPort);

    const uint64_t t1 = getSystemTimeNs();
    const ssize_t sent = sendto(eventSocket_, msg, sizeof(msg), 0,
                                reinterpret_cast<struct sockaddr*>(&dest),
                                sizeof(dest));
    if (sent < 0) {
        std::cerr << "[PTPSlave] Failed to send Pdelay_Req: " << strerror(errno)
                  << std::endl;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pdelayMutex_);
        t1_pdelayReqSendTimeNs_ = t1;
        t4_pdelayRespReceiveTimeNs_ = 0;
        t2_pdelayRequestReceipt_ = PTPTimestamp();
        pdelayCorrectionNs_ = 0;
        waitingForPdelayResp_ = true;
        waitingForPdelayFollowUp_ = false;
    }
    pdelayReqSentCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// A peer-to-peer port that never answers leaves its neighbour unable to
// measure the link, so this end responds even though it is slave-only.
bool PTPSlave::sendPdelayResp(const PTPHeader& request, uint64_t receiptTimeNs) {
    if (eventSocket_ < 0 || generalSocket_ < 0) return false;

    uint8_t resp[kMinPdelayRespSize];
    BuildPeerHeader(resp, PTPMessageType::Pdelay_Resp, sizeof(resp),
                    static_cast<uint8_t>(config_.domain), selfPortId_,
                    request.sequenceId, 0x7F, true);
    WriteTimestamp(resp, kTimestampOffset, receiptTimeNs);   // t2
    WritePortIdentity(resp, kRequestingPortOffset, request.sourcePortIdentity);

    struct sockaddr_in dest;
    std::memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(kPTPPeerDelayMulticast);
    dest.sin_port = htons(config_.eventPort);
    if (sendto(eventSocket_, resp, sizeof(resp), 0,
               reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) < 0) {
        std::cerr << "[PTPSlave] Failed to send Pdelay_Resp: " << strerror(errno)
                  << std::endl;
        return false;
    }

    // Two-step: the turnaround this end took is carried in the Follow_Up as
    // t3, so the requester can subtract it.
    const uint64_t t3 = getSystemTimeNs();
    uint8_t followUp[kMinPdelayRespSize];
    BuildPeerHeader(followUp, PTPMessageType::Pdelay_Resp_FU, sizeof(followUp),
                    static_cast<uint8_t>(config_.domain), selfPortId_,
                    request.sequenceId, 0x7F, false);
    WriteTimestamp(followUp, kTimestampOffset, t3);
    WritePortIdentity(followUp, kRequestingPortOffset,
                      request.sourcePortIdentity);

    dest.sin_port = htons(config_.generalPort);
    if (sendto(generalSocket_, followUp, sizeof(followUp), 0,
               reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) < 0) {
        std::cerr << "[PTPSlave] Failed to send Pdelay_Resp_Follow_Up: "
                  << strerror(errno) << std::endl;
        return false;
    }

    pdelayReqAnsweredCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PTPSlave::handlePdelayReq(const PTPHeader& header, const uint8_t* data,
                               size_t len, uint64_t receiveTimeNs) {
    (void)data;
    if (config_.delayMechanism != DelayMechanism::PeerToPeer) return;
    if (!config_.respondToPdelayReq) return;
    if (len < kMinPdelayReqSize) return;
    // Our own request coming back through multicast loopback is not a peer.
    if (header.sourcePortIdentity == selfPortId_) return;
    sendPdelayResp(header, receiveTimeNs);
}

void PTPSlave::handlePdelayResp(const PTPHeader& header, const uint8_t* data,
                                size_t len, uint64_t receiveTimeNs) {
    if (config_.delayMechanism != DelayMechanism::PeerToPeer) return;
    if (len < kMinPdelayRespSize) return;

    PTPPortIdentity requestingPort;
    parsePortIdentity(data, kRequestingPortOffset, requestingPort);
    if (!(requestingPort == selfPortId_)) return;      // answering someone else

    const bool isTwoStep = (header.flagField & kFlagTwoStep) != 0;
    bool complete = false;
    {
        std::lock_guard<std::mutex> lock(pdelayMutex_);
        if (!waitingForPdelayResp_) return;
        if (header.sequenceId != pdelayReqSequenceId_) return;

        parseTimestamp(data, kTimestampOffset, t2_pdelayRequestReceipt_);
        t4_pdelayRespReceiveTimeNs_ = receiveTimeNs;
        pdelayCorrectionNs_ += header.correctionField >> 16;
        waitingForPdelayResp_ = false;
        waitingForPdelayFollowUp_ = isTwoStep;
        complete = !isTwoStep;
    }
    pdelayRespCount_.fetch_add(1, std::memory_order_relaxed);

    // One-step responder: it folded its turnaround into the correction field,
    // so there is nothing left to wait for and t3 - t2 is zero.
    if (complete) completePdelay(0);
}

void PTPSlave::handlePdelayRespFollowUp(const PTPHeader& header,
                                        const uint8_t* data, size_t len) {
    if (config_.delayMechanism != DelayMechanism::PeerToPeer) return;
    if (len < kMinPdelayRespSize) return;

    PTPPortIdentity requestingPort;
    parsePortIdentity(data, kRequestingPortOffset, requestingPort);
    if (!(requestingPort == selfPortId_)) return;

    int64_t t3MinusT2 = 0;
    {
        std::lock_guard<std::mutex> lock(pdelayMutex_);
        if (!waitingForPdelayFollowUp_) return;
        if (header.sequenceId != pdelayReqSequenceId_) return;

        PTPTimestamp t3;
        parseTimestamp(data, kTimestampOffset, t3);
        t3MinusT2 = static_cast<int64_t>(t3.toNanoseconds())
                    - static_cast<int64_t>(t2_pdelayRequestReceipt_.toNanoseconds());
        pdelayCorrectionNs_ += header.correctionField >> 16;
        waitingForPdelayFollowUp_ = false;
    }
    pdelayRespFollowUpCount_.fetch_add(1, std::memory_order_relaxed);
    completePdelay(t3MinusT2);
}

// meanLinkDelay = ((t4 - t1) - (t3 - t2) - corrections) / 2, IEEE 1588-2008
// sec 11.4.2. It goes through the same filter and the same published path
// delay as the end-to-end measurement, so the offset arithmetic downstream
// does not care which mechanism produced it.
void PTPSlave::completePdelay(int64_t t3MinusT2Ns) {
    int64_t t1Ns = 0;
    int64_t t4Ns = 0;
    int64_t correctionNs = 0;
    {
        std::lock_guard<std::mutex> lock(pdelayMutex_);
        if (t1_pdelayReqSendTimeNs_ == 0 || t4_pdelayRespReceiveTimeNs_ == 0) {
            return;
        }
        t1Ns = static_cast<int64_t>(t1_pdelayReqSendTimeNs_);
        t4Ns = static_cast<int64_t>(t4_pdelayRespReceiveTimeNs_);
        correctionNs = pdelayCorrectionNs_;
    }

    const int64_t linkDelay =
        ((t4Ns - t1Ns) - t3MinusT2Ns - correctionNs) / 2;
    storeFilteredPathDelay(linkDelay);
}

// ============================================================================
// Message Parsing
// ============================================================================

// IEEE 1588-2008 sec 7.7.2.1: logMessageInterval is log2 seconds, and 0x7F
// means the message is not being sent. Anything outside the configured bounds
// is refused rather than obeyed -- an advertised -12 would be 4096 Delay_Req a
// second at a master that probably meant something else.
int PTPSlave::logIntervalToMs(int8_t logInterval) const {
    if (logInterval == 0x7F) return 0;
    if (logInterval < config_.minLogInterval
        || logInterval > config_.maxLogInterval) {
        return 0;
    }
    const double seconds = std::pow(2.0, static_cast<double>(logInterval));
    const int milliseconds = static_cast<int>(std::lround(seconds * 1000.0));
    return milliseconds > 0 ? milliseconds : 0;
}

bool PTPSlave::parseHeader(const uint8_t* data, size_t len, PTPHeader& header) {
    if (len < kPTPHeaderSize) return false;

    header.transportAndType = data[0];
    header.versionPTP = data[1] & 0x0F;

    // Verify PTP version
    if (header.versionPTP != kPTPVersion) return false;

    header.messageLength = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    header.domainNumber = data[4];
    header.reserved1 = data[5];
    header.flagField = (static_cast<uint16_t>(data[6]) << 8) | data[7];

    // Correction field: 8 bytes, signed, nanoseconds * 2^16
    header.correctionField = 0;
    for (int i = 0; i < 8; ++i) {
        header.correctionField = (header.correctionField << 8) | data[8 + i];
    }

    header.reserved2 = 0;
    for (int i = 0; i < 4; ++i) {
        header.reserved2 = (header.reserved2 << 8) | data[16 + i];
    }

    parsePortIdentity(data, 20, header.sourcePortIdentity);

    header.sequenceId = (static_cast<uint16_t>(data[30]) << 8) | data[31];
    header.controlField = data[32];
    header.logMessageInterval = static_cast<int8_t>(data[33]);

    return true;
}

bool PTPSlave::parseTimestamp(const uint8_t* data, size_t offset, PTPTimestamp& ts) {
    ts.secondsHi = (static_cast<uint16_t>(data[offset]) << 8) |
                    data[offset + 1];
    ts.secondsLo = (static_cast<uint32_t>(data[offset + 2]) << 24) |
                    (static_cast<uint32_t>(data[offset + 3]) << 16) |
                    (static_cast<uint32_t>(data[offset + 4]) << 8) |
                    data[offset + 5];
    ts.nanoseconds = (static_cast<uint32_t>(data[offset + 6]) << 24) |
                     (static_cast<uint32_t>(data[offset + 7]) << 16) |
                     (static_cast<uint32_t>(data[offset + 8]) << 8) |
                     data[offset + 9];
    return true;
}

void PTPSlave::parseClockIdentity(const uint8_t* data, size_t offset, PTPClockIdentity& id) {
    for (int i = 0; i < 8; ++i) {
        id.id[i] = data[offset + i];
    }
}

void PTPSlave::parsePortIdentity(const uint8_t* data, size_t offset, PTPPortIdentity& pid) {
    parseClockIdentity(data, offset, pid.clockIdentity);
    pid.portNumber = (static_cast<uint16_t>(data[offset + 8]) << 8) | data[offset + 9];
}

// ============================================================================
// Message Handlers
// ============================================================================

void PTPSlave::handleSync(const PTPHeader& header, const uint8_t* data, size_t len,
                           uint64_t receiveTimeNs) {
    if (len < kMinSyncSize) return;

    syncCount_.fetch_add(1, std::memory_order_relaxed);

    bool isTwoStep = (header.flagField & kFlagTwoStep) != 0;

    // syncMutex_ is scoped to the record block: calculateOffsetAndDelay()
    // re-acquires it itself, and std::mutex is non-recursive (2026-08-31,
    // second round -- the same family of self-deadlock the delayMutex_ fix
    // addressed, found by the new PTP loopback test the moment a real
    // master/slave pair exchanged messages).
    bool computeNow = false;
    {
        std::lock_guard<std::mutex> lock(syncMutex_);

        // Store t2 (our receive timestamp)
        t2_receiveTimeNs_ = receiveTimeNs;
        lastSyncSequenceId_ = header.sequenceId;
        advertisedSyncIntervalMs_.store(logIntervalToMs(header.logMessageInterval),
                                        std::memory_order_relaxed);
        syncCorrectionField_ = header.correctionField;

        if (isTwoStep) {
            // Two-step: wait for Follow_Up with the precise t1
            waitingForFollowUp_ = true;
            // Parse origin timestamp from Sync (informational only in two-step)
            parseTimestamp(data, kTimestampOffset, syncOriginTimestamp_);
        } else {
            // One-step: origin timestamp in Sync IS t1
            parseTimestamp(data, kTimestampOffset, t1_syncOriginTimestamp_);
            waitingForFollowUp_ = false;
            computeNow = true; // with existing delay, outside the lock
        }
    }

    if (computeNow) calculateOffsetAndDelay();
}

void PTPSlave::handleFollowUp(const PTPHeader& header, const uint8_t* data, size_t len) {
    if (len < kMinFollowUpSize) return;

    followUpCount_.fetch_add(1, std::memory_order_relaxed);

    // Same scoping rule as handleSync/handleDelayResp: record under the
    // lock, compute after releasing it. This is the path every two-step
    // (i.e. every AES67) Follow_Up takes, so holding syncMutex_ across
    // calculateOffsetAndDelay() deadlocked the slave's receive thread on
    // the very first Follow_Up from a real master (2026-08-31).
    {
        std::lock_guard<std::mutex> lock(syncMutex_);

        // Follow_Up must match the Sync we're waiting for
        if (!waitingForFollowUp_) return;
        if (header.sequenceId != lastSyncSequenceId_) return;

        // Parse the precise origin timestamp (t1)
        parseTimestamp(data, kTimestampOffset, t1_syncOriginTimestamp_);

        // Add Follow_Up correction to Sync correction
        // Both are in nanoseconds * 2^16 fixed point
        int64_t totalCorrectionFixed = syncCorrectionField_ + header.correctionField;

        // Convert correction from fixed-point (ns * 2^16) to nanoseconds
        int64_t correctionNs = totalCorrectionFixed >> 16;

        // Apply correction to t1
        uint64_t t1Ns = t1_syncOriginTimestamp_.toNanoseconds();
        t1Ns += static_cast<uint64_t>(correctionNs);
        t1_syncOriginTimestamp_ = PTPTimestamp(t1Ns);

        waitingForFollowUp_ = false;
    }

    // Now we have t1 and t2 — compute offset (using existing path delay)
    calculateOffsetAndDelay();
}

void PTPSlave::handleDelayResp(const PTPHeader& header, const uint8_t* data, size_t len) {
    if (len < kMinDelayRespSize) return;

    delayRespCount_.fetch_add(1, std::memory_order_relaxed);

    // logMinDelayReqInterval: the rate this master wants Delay_Req at
    // (IEEE 1588-2008 sec 9.5.11.2). Recorded whatever the message turns out
    // to be for, so a Delay_Resp meant for another port still tells us the
    // rate the master is asking of everyone.
    if (config_.followAdvertisedIntervals) {
        const int advertised = logIntervalToMs(header.logMessageInterval);
        if (advertised > 0) {
            advertisedDelayReqIntervalMs_.store(advertised,
                                                std::memory_order_relaxed);
        }
    }

    // delayMutex_ is scoped to the parse-and-record block and MUST be
    // released before calculateOffsetAndDelay(), which re-acquires it
    // itself (line ~913). The old code declared the guard at function
    // scope with only a comment claiming "we release delay first":
    // std::mutex is non-recursive, so the first real Delay_Resp
    // self-deadlocked the PTP receive thread forever, and stop()'s
    // join() with it (2026-08-31 audit, confirmed twice over).
    {
        std::lock_guard<std::mutex> lock(delayMutex_);

        // Must match our Delay_Req
        if (!waitingForDelayResp_) return;
        if (header.sequenceId != delayReqSequenceId_) return;

        // Verify the requesting port identity matches ours (bytes 44-53)
        PTPPortIdentity requestingPort;
        parsePortIdentity(data, 44, requestingPort);
        if (!(requestingPort == selfPortId_)) return;

        // Parse t4 (master's receive timestamp of our Delay_Req)
        parseTimestamp(data, kTimestampOffset, t4_delayRespReceiveTimestamp_);

        // Apply correction field
        int64_t correctionNs = header.correctionField >> 16;
        uint64_t t4Ns = t4_delayRespReceiveTimestamp_.toNanoseconds();
        t4Ns += static_cast<uint64_t>(correctionNs);
        t4_delayRespReceiveTimestamp_ = PTPTimestamp(t4Ns);

        waitingForDelayResp_ = false;
    }

    // Calculate with new delay information (acquires its own locks)
    calculateOffsetAndDelay();
}

void PTPSlave::handleAnnounce(const PTPHeader& header, const uint8_t* data, size_t len) {
    if (len < kMinAnnounceSize) return;

    announceCount_.fetch_add(1, std::memory_order_relaxed);

    // Parse announce data
    PTPAnnounceData announce;
    announce.masterPortId = header.sourcePortIdentity;
    announce.grandmasterPriority1 = data[kAnnounceGMPriority1Offset];
    announce.grandmasterClockClass = data[kAnnounceGMClassOffset];
    announce.grandmasterClockAccuracy = data[kAnnounceGMAccuracyOffset];
    announce.grandmasterOffsetScaledLogVariance =
        (static_cast<uint16_t>(data[kAnnounceGMVarianceOffset]) << 8) |
        data[kAnnounceGMVarianceOffset + 1];
    announce.grandmasterPriority2 = data[kAnnounceGMPriority2Offset];
    parseClockIdentity(data, kAnnounceGMIdentityOffset, announce.grandmasterIdentity);
    announce.stepsRemoved =
        (static_cast<uint16_t>(data[kAnnounceStepsRemovedOffset]) << 8) |
        data[kAnnounceStepsRemovedOffset + 1];
    announce.timeSource = data[kAnnounceTimeSourceOffset];
    announce.logAnnounceInterval = header.logMessageInterval;
    if (config_.followAdvertisedIntervals) {
        const int advertised = logIntervalToMs(header.logMessageInterval);
        if (advertised > 0) {
            advertisedAnnounceIntervalMs_.store(advertised,
                                                std::memory_order_relaxed);
        }
    }
    announce.lastReceived = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(masterMutex_);

    if (!hasMaster_) {
        // Accept this master (simplified BMCA — first announce wins for slave-only)
        currentMaster_ = announce;
        grandmasterIdentity_ = announce.grandmasterIdentity;
        hasMaster_ = true;

        clockClass_.store(announce.grandmasterClockClass, std::memory_order_release);
        clockAccuracy_.store(announce.grandmasterClockAccuracy, std::memory_order_release);

        std::cout << "[PTPSlave] Accepted master: "
                  << announce.grandmasterIdentity.toString()
                  << " class=" << static_cast<int>(announce.grandmasterClockClass)
                  << " accuracy=0x" << std::hex << static_cast<int>(announce.grandmasterClockAccuracy)
                  << std::dec << std::endl;
    } else {
        // Simple BMCA: prefer lower priority1, then lower class, then lower priority2
        bool isBetter = false;
        if (announce.grandmasterPriority1 < currentMaster_.grandmasterPriority1) {
            isBetter = true;
        } else if (announce.grandmasterPriority1 == currentMaster_.grandmasterPriority1) {
            if (announce.grandmasterClockClass < currentMaster_.grandmasterClockClass) {
                isBetter = true;
            } else if (announce.grandmasterClockClass == currentMaster_.grandmasterClockClass) {
                if (announce.grandmasterPriority2 < currentMaster_.grandmasterPriority2) {
                    isBetter = true;
                }
            }
        }

        if (isBetter) {
            std::cout << "[PTPSlave] Switching to better master: "
                      << announce.grandmasterIdentity.toString() << std::endl;
            currentMaster_ = announce;
            grandmasterIdentity_ = announce.grandmasterIdentity;
            clockClass_.store(announce.grandmasterClockClass, std::memory_order_release);
            clockAccuracy_.store(announce.grandmasterClockAccuracy, std::memory_order_release);

            // Reset lock on master change
            locked_.store(false, std::memory_order_release);
            consecutiveGoodMeasurements_ = 0;
        } else if (announce.grandmasterIdentity == grandmasterIdentity_) {
            // Same master, refresh timeout
            currentMaster_.lastReceived = announce.lastReceived;
        }
    }
}

// ============================================================================
// Delay Request Transmission
// ============================================================================

bool PTPSlave::sendDelayReq() {
    if (eventSocket_ < 0) return false;

    // Build Delay_Req message (44 bytes: 34 header + 10 timestamp)
    uint8_t msg[44];
    std::memset(msg, 0, sizeof(msg));

    uint16_t seqId;
    {
        std::lock_guard<std::mutex> lock(delayMutex_);
        // PRE-increment, so delayReqSequenceId_ holds the id actually
        // SENT: handleDelayResp matches the answer against this member,
        // and with post-increment it held sent+1, so every well-formed
        // Delay_Resp was rejected on sequence mismatch and path delay
        // stayed 0 forever -- against any master, not just ours
        // (2026-08-31, found by the PTP loopback test: 21 Delay_Resp
        // received, 0 accepted). Starting at 1 rather than 0 is
        // immaterial: PTP sequence ids are opaque and wrap.
        seqId = ++delayReqSequenceId_;
    }

    // Header
    msg[0] = static_cast<uint8_t>(PTPMessageType::Delay_Req); // transportSpecific=0 | messageType
    msg[1] = kPTPVersion;
    msg[2] = 0; // messageLength high byte
    msg[3] = 44; // messageLength low byte
    msg[4] = static_cast<uint8_t>(config_.domain);
    // flags = 0
    // correction = 0
    // reserved = 0

    // Source port identity (bytes 20-29)
    for (int i = 0; i < 8; ++i) {
        msg[20 + i] = selfPortId_.clockIdentity.id[i];
    }
    msg[28] = static_cast<uint8_t>((selfPortId_.portNumber >> 8) & 0xFF);
    msg[29] = static_cast<uint8_t>(selfPortId_.portNumber & 0xFF);

    // Sequence ID (bytes 30-31)
    msg[30] = static_cast<uint8_t>((seqId >> 8) & 0xFF);
    msg[31] = static_cast<uint8_t>(seqId & 0xFF);

    // Control field = 1 for Delay_Req
    msg[32] = 1;

    // logMessageInterval = 0x7F (not applicable)
    msg[33] = 0x7F;

    // Origin timestamp (bytes 34-43) = 0 (we use t3 from our send time)

    // Record t3 right before sending
    uint64_t t3 = getSystemTimeNs();

    // Send to multicast
    struct sockaddr_in dest;
    std::memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(kPTPPrimaryMulticast);
    dest.sin_port = htons(config_.eventPort);

    ssize_t sent = sendto(eventSocket_, msg, sizeof(msg), 0,
                          reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    if (sent < 0) {
        std::cerr << "[PTPSlave] Failed to send Delay_Req: "
                  << strerror(errno) << std::endl;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(delayMutex_);
        t3_delayReqSendTimeNs_ = t3;
        waitingForDelayResp_ = true;
    }

    delayReqSentCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ============================================================================
// Offset and Delay Calculation
// ============================================================================

void PTPSlave::calculateOffsetAndDelay() {
    // We need t1, t2 to compute offset (using existing delay estimate).
    // When we also have t3, t4 we compute the full offset+delay.
    //
    // offset    = ((t2 - t1) + (t3 - t4)) / 2
    // pathDelay = ((t2 - t1) - (t3 - t4)) / 2
    //
    // With only t1, t2:
    // offset_approx = (t2 - t1) - pathDelay

    int64_t t1Ns = 0;
    int64_t t3Ns = 0;
    uint64_t t2Ns = 0;

    {
        std::lock_guard<std::mutex> lock(syncMutex_);
        if (waitingForFollowUp_) return; // Don't have t1 yet
        if (t1_syncOriginTimestamp_.isZero()) return;
        t1Ns = static_cast<int64_t>(t1_syncOriginTimestamp_.toNanoseconds());
        t2Ns = t2_receiveTimeNs_;
    }

    int64_t t2SignedNs = static_cast<int64_t>(t2Ns);

    // Check if we have delay measurement (t3, t4)
    bool haveDelay = false;
    int64_t t4Ns = 0;
    {
        std::lock_guard<std::mutex> lock(delayMutex_);
        if (!waitingForDelayResp_ && t3_delayReqSendTimeNs_ != 0 &&
            !t4_delayRespReceiveTimestamp_.isZero()) {
            haveDelay = true;
            t3Ns = static_cast<int64_t>(t3_delayReqSendTimeNs_);
            t4Ns = static_cast<int64_t>(t4_delayRespReceiveTimestamp_.toNanoseconds());
        }
    }

    int64_t offset = 0;
    int64_t delay = 0;

    if (haveDelay) {
        // Full four-timestamp calculation
        // offset    = ((t2 - t1) + (t3 - t4)) / 2
        // pathDelay = ((t2 - t1) + (t4 - t3)) / 2
        int64_t ms2slave = t2SignedNs - t1Ns;    // t2 - t1
        int64_t slave2m = t4Ns - t3Ns;           // t4 - t3

        offset = (ms2slave - slave2m) / 2;
        delay  = (ms2slave + slave2m) / 2;

        storeFilteredPathDelay(delay);
    } else {
        // Approximate offset using stored path delay
        int64_t storedDelay = pathDelayNs_.load(std::memory_order_acquire);
        offset = (t2SignedNs - t1Ns) - storedDelay;
    }

    // Filter offset (moving average)
    offsetHistory_[offsetHistoryIndex_] = offset;
    offsetHistoryIndex_ = (offsetHistoryIndex_ + 1) % kOffsetFilterSize;
    if (offsetHistoryCount_ < kOffsetFilterSize) offsetHistoryCount_++;

    // Compute filtered offset (average)
    int64_t filteredOffset = 0;
    for (size_t i = 0; i < offsetHistoryCount_; ++i) {
        filteredOffset += offsetHistory_[i];
    }
    filteredOffset /= static_cast<int64_t>(offsetHistoryCount_);

    // Store computed offset
    offsetNs_.store(filteredOffset, std::memory_order_release);

    // Drift estimation
    uint64_t nowNs = getSystemTimeNs();
    if (lastDriftCalcTimeNs_ != 0) {
        uint64_t dtNs = nowNs - lastDriftCalcTimeNs_;
        if (dtNs > 500000000ULL) { // Update drift every 500ms minimum
            int64_t dOffset = filteredOffset - lastDriftCalcOffsetNs_;
            // drift in ppb = (dOffset_ns / dt_ns) * 1e9
            double driftPpb = (static_cast<double>(dOffset) / static_cast<double>(dtNs)) * 1e9;

            // Smooth drift
            double prevDrift = frequencyDriftPpb_.load(std::memory_order_acquire);
            double smoothed = prevDrift * 0.9 + driftPpb * 0.1;
            frequencyDriftPpb_.store(smoothed, std::memory_order_release);

            lastDriftCalcTimeNs_ = nowNs;
            lastDriftCalcOffsetNs_ = filteredOffset;
        }
    } else {
        lastDriftCalcTimeNs_ = nowNs;
        lastDriftCalcOffsetNs_ = filteredOffset;
    }

    // Lock detection
    if (std::abs(filteredOffset) < kLockToleranceNs) {
        if (consecutiveGoodMeasurements_ < kLockThreshold * 2) {
            consecutiveGoodMeasurements_++;
        }
    } else {
        consecutiveGoodMeasurements_ = std::max(0, consecutiveGoodMeasurements_ - 1);
    }

    bool wasLocked = locked_.load(std::memory_order_acquire);
    bool nowLocked = consecutiveGoodMeasurements_ >= kLockThreshold;

    if (nowLocked != wasLocked) {
        locked_.store(nowLocked, std::memory_order_release);
        if (nowLocked) {
            std::cout << "[PTPSlave] LOCKED to master — offset="
                      << filteredOffset << "ns delay="
                      << pathDelayNs_.load(std::memory_order_acquire) << "ns" << std::endl;
        } else {
            std::cout << "[PTPSlave] Lock LOST — offset="
                      << filteredOffset << "ns" << std::endl;
        }
    }

    // Invoke callback
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (measurementCallback_) {
            PTPMeasurement m;
            m.offsetFromMasterNs = filteredOffset;
            m.meanPathDelayNs = pathDelayNs_.load(std::memory_order_acquire);
            m.frequencyDriftPpb = frequencyDriftPpb_.load(std::memory_order_acquire);
            {
                std::lock_guard<std::mutex> mlock(masterMutex_);
                m.grandmasterID = grandmasterIdentity_;
                m.clockClass = clockClass_.load(std::memory_order_acquire);
                m.clockAccuracy = clockAccuracy_.load(std::memory_order_acquire);
            }
            m.valid = true;
            measurementCallback_(m);
        }
    }
}

// ============================================================================
// Utility
// ============================================================================

uint64_t PTPSlave::getSystemTimeNs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

bool PTPSlave::getInterfaceMAC(uint8_t mac[6]) const {
    struct ifaddrs* ifaddrs_ptr = nullptr;
    if (getifaddrs(&ifaddrs_ptr) != 0) return false;

    bool found = false;
    for (struct ifaddrs* ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_LINK) continue;
        if (config_.interfaceName != ifa->ifa_name) continue;

        struct sockaddr_dl* sdl = reinterpret_cast<struct sockaddr_dl*>(ifa->ifa_addr);
        if (sdl->sdl_alen == 6) {
            std::memcpy(mac, LLADDR(sdl), 6);
            found = true;
            break;
        }
    }

    freeifaddrs(ifaddrs_ptr);
    return found;
}

} // namespace AES67
