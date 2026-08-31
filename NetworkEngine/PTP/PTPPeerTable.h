#ifndef PTP_PEER_TABLE_H
#define PTP_PEER_TABLE_H

//
// PTPPeerTable
// AES67 macOS Driver
//
// The aggregation core behind passive PTP peer discovery (see
// PTPPeerObserver): given a stream of observed PTP messages — each reduced to
// (clock identity, message type, source IP, domain) — it keeps one row per
// distinct clock identity, tracks which message types that identity has been
// seen sending, and ages rows out when they stop appearing.
//
// It is deliberately socket-free and time-injected so it can be unit-tested
// on its own (TestPTPPeerTable), exactly like StreamManager::evaluateSinkFollow.
// PTPPeerObserver owns the sockets and thread and feeds this.
//
// Why this exists: a Dolby amplifier (DAC3202/DMA) is a receiver in our
// topology and is silent at the SAP/SDP layer, but as a PTP participant it is
// NOT silent — its messages carry its clock identity (a MAC-derived EUI-64,
// so bytes 0-2 are the vendor OUI). Counting distinct identities by inferred
// role is how "which Dolby elements are on the network, and how many" gets
// answered — including how many chained DMA units there are, since each unit
// is its own PTP slave with its own identity. See
// Docs/dac3202_autodetection_study.md.
//

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace AES67 {

// Role of a peer, inferred from the message types it has been seen sending.
// A grandmaster/master originates Sync/Follow_Up/Announce; a slave sends
// Delay_Req/Pdelay_Req to measure its path delay to the master.
//
// From THIS driver's own point of view the role maps to a direction: a
// Master peer is upstream of us (we follow its clock, it feeds us audio) —
// an INPUT source; a Slave peer is downstream (it follows our clock, we feed
// it) — an OUTPUT sink. That is exactly the CP850/CP950 (master → input) vs
// DAC3202/DMA (slave → output) split.
enum class PTPPeerRole { Unknown, Master, Slave, Mixed };

struct PTPPeerObservation {
    std::array<uint8_t, 8> clockId{};
    std::string sourceIp;                 // last source IP seen for this identity
    int domain{0};                        // last PTP domain seen
    uint32_t messageTypeMask{0};          // bit N set = message type N seen
    std::chrono::steady_clock::time_point firstSeen{};
    std::chrono::steady_clock::time_point lastSeen{};
    uint64_t messageCount{0};

    // Vendor OUI (first 3 bytes of the clock identity == the MAC OUI, because
    // a PTP clock identity is the MAC as EUI-64: OUI, then FF FE, then the
    // rest of the MAC).
    std::array<uint8_t, 3> oui() const { return {clockId[0], clockId[1], clockId[2]}; }

    PTPPeerRole role() const { return roleFromMask(messageTypeMask); }

    // Free-standing so it can be unit-tested without constructing a row.
    static PTPPeerRole roleFromMask(uint32_t mask) {
        // Message type values are the low nibble of PTP byte 0 (see
        // PTPMessageType): Sync=0x00, Delay_Req=0x01, Pdelay_Req=0x02,
        // Follow_Up=0x08, Announce=0x0B.
        const uint32_t masterBits = bit(0x00) | bit(0x08) | bit(0x0B); // Sync, Follow_Up, Announce
        const uint32_t slaveBits  = bit(0x01) | bit(0x02);             // Delay_Req, Pdelay_Req
        const bool m = (mask & masterBits) != 0;
        const bool s = (mask & slaveBits) != 0;
        if (m && s) return PTPPeerRole::Mixed;
        if (m) return PTPPeerRole::Master;
        if (s) return PTPPeerRole::Slave;
        return PTPPeerRole::Unknown;
    }

    static constexpr uint32_t bit(uint8_t msgType) { return 1u << (msgType & 0x1F); }

    std::string ouiString() const { return OuiToString(oui()); }
    std::string clockIdString() const { return ClockIdToString(clockId); }

    static std::string OuiToString(const std::array<uint8_t, 3>& o) {
        return hex2(o[0]) + ":" + hex2(o[1]) + ":" + hex2(o[2]);
    }
    static std::string ClockIdToString(const std::array<uint8_t, 8>& id) {
        std::string s;
        for (size_t i = 0; i < id.size(); ++i) {
            if (i) s += ":";
            s += hex2(id[i]);
        }
        return s;
    }

    static std::string hex2(uint8_t b) {
        static const char* d = "0123456789abcdef";
        std::string s;
        s += d[(b >> 4) & 0xF];
        s += d[b & 0xF];
        return s;
    }
};

class PTPPeerTable {
public:
    // How long a peer may go unheard before it's dropped. PTP masters
    // announce ~1 s and slaves request delay ~1 s, so 10 s tolerates a few
    // lost messages while keeping the list current — the same ten-missed
    // rule SAPListener uses at its own interval.
    static constexpr std::chrono::seconds kPeerTimeout{10};

    // Hard ceiling on tracked peers. The clock identity is copied verbatim
    // from every inbound multicast PTP message, and sweep() only runs when
    // someone queries -- so without a cap, any host on the segment could
    // spoof distinct identities and grow this map without bound between
    // queries (2026-08-31 audit). Same backstop pattern SAPListener uses
    // for discovered sessions: evict the least-recently-seen row.
    static constexpr size_t kMaxPeers{64};

    // Record one observed message. Creates the row on first sight, otherwise
    // updates it (adds the message type, refreshes source IP / domain /
    // lastSeen, bumps the count).
    void record(const std::array<uint8_t, 8>& clockId, uint8_t messageType,
                const std::string& sourceIp, int domain,
                std::chrono::steady_clock::time_point now) {
        if (rows_.find(clockId) == rows_.end() && rows_.size() >= kMaxPeers) {
            evictLeastRecentlySeen();
        }
        auto& row = rows_[clockId];
        if (row.messageCount == 0) {
            row.clockId = clockId;
            row.firstSeen = now;
        }
        row.messageTypeMask |= PTPPeerObservation::bit(messageType);
        if (!sourceIp.empty()) row.sourceIp = sourceIp;
        row.domain = domain;
        row.lastSeen = now;
        ++row.messageCount;
    }

    // Drop rows not heard within kPeerTimeout of `now`.
    void sweep(std::chrono::steady_clock::time_point now) {
        for (auto it = rows_.begin(); it != rows_.end();) {
            if (now - it->second.lastSeen > kPeerTimeout) {
                it = rows_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Current peers, oldest-first is not guaranteed — ordered by clock id.
    std::vector<PTPPeerObservation> peers() const {
        std::vector<PTPPeerObservation> out;
        out.reserve(rows_.size());
        for (const auto& kv : rows_) out.push_back(kv.second);
        return out;
    }

    // Count of peers matching a role, optionally restricted to one OUI.
    size_t countByRole(PTPPeerRole role) const {
        size_t n = 0;
        for (const auto& kv : rows_) if (kv.second.role() == role) ++n;
        return n;
    }

    size_t size() const { return rows_.size(); }
    void clear() { rows_.clear(); }

private:
    void evictLeastRecentlySeen() {
        auto oldest = rows_.begin();
        for (auto it = rows_.begin(); it != rows_.end(); ++it) {
            if (it->second.lastSeen < oldest->second.lastSeen) oldest = it;
        }
        if (oldest != rows_.end()) rows_.erase(oldest);
    }

    std::map<std::array<uint8_t, 8>, PTPPeerObservation> rows_;
};

} // namespace AES67

#endif // PTP_PEER_TABLE_H
