#ifndef RTCP_RECEIVER_TABLE_H
#define RTCP_RECEIVER_TABLE_H

//
// RTCPReceiverTable
// AES67 macOS Driver
//
// The second detection vector for downstream Dolby gear, next to the passive
// PTP one (PTPPeerTable). A device receiving one of this driver's transmit
// streams — a DAC3202 or DMA amplifier — should, if it implements RTCP, send
// Receiver Reports back on the session's RTCP port. Each RR carries the
// reporting receiver's own SSRC, so counting the distinct reporter SSRCs on a
// transmit stream's RTCP port counts its receivers, independent of whether
// those receivers say anything on PTP.
//
// This is the aggregation + parse core: socket-free and time-injected so it
// can be unit-tested on its own (TestRTCPReceiverTable), exactly like
// PTPPeerTable. RTCPMonitor owns the sockets and feeds this.
//
// Honest caveat, recorded where the code lives: whether a Dolby amplifier
// emits RTCP at all is not documented in its manuals — this vector only helps
// if it does. It is additive to the PTP vector and the manual chain config,
// never the sole source of truth.
//

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace AES67 {

// RTCP payload types (RFC 3550 §12.1).
enum : uint8_t {
    kRTCP_SR   = 200, // Sender Report
    kRTCP_RR   = 201, // Receiver Report
    kRTCP_SDES = 202, // Source Description (CNAME etc.)
    kRTCP_BYE  = 203,
    kRTCP_APP  = 204,
};

struct RTCPReporter {
    uint32_t ssrc{0};
    std::string sourceIp;   // last source IP seen
    std::string cname;      // from SDES, when present
    std::chrono::steady_clock::time_point firstSeen{};
    std::chrono::steady_clock::time_point lastSeen{};
    uint64_t packetCount{0};
};

// One parsed report: a reporter SSRC, and optionally its CNAME.
struct RTCPParseResult {
    std::vector<uint32_t> reporterSSRCs;                 // SR/RR sender SSRCs
    std::vector<std::pair<uint32_t, std::string>> cnames; // (ssrc, cname) from SDES
    bool valid{false};                                   // structurally sound RTCP
};

class RTCPReceiverTable {
public:
    // Same ten-missed-report rule as the other discovery tables, at RTCP's
    // usual ~5 s minimum reporting interval → 60 s. RTCP intervals scale with
    // session bandwidth but 5 s is the RFC 3550 floor, so this is generous.
    static constexpr std::chrono::seconds kReporterTimeout{60};

    // Parse a (possibly compound) RTCP packet. Extracts the sender SSRC of
    // every SR/RR and any CNAMEs from SDES. Fully bounds-checked: a truncated
    // or malformed packet yields valid=false and whatever was safely read
    // before the break. Never reads out of bounds.
    static RTCPParseResult parse(const uint8_t* data, size_t len) {
        RTCPParseResult out;
        size_t off = 0;
        bool sawAny = false;
        while (off + 4 <= len) {
            const uint8_t b0 = data[off];
            const uint8_t version = (b0 >> 6) & 0x03;
            if (version != 2) { out.valid = false; return out; }
            const uint8_t pt = data[off + 1];
            const uint16_t lenWords = static_cast<uint16_t>((data[off + 2] << 8) | data[off + 3]);
            const size_t pktBytes = (static_cast<size_t>(lenWords) + 1) * 4;
            if (off + pktBytes > len) { out.valid = sawAny; return out; }
            sawAny = true;

            if (pt == kRTCP_SR || pt == kRTCP_RR) {
                // Sender/reporter SSRC is bytes 4..7 of the RTCP packet.
                if (pktBytes >= 8) {
                    const uint32_t ssrc =
                        (static_cast<uint32_t>(data[off + 4]) << 24) |
                        (static_cast<uint32_t>(data[off + 5]) << 16) |
                        (static_cast<uint32_t>(data[off + 6]) << 8) |
                        static_cast<uint32_t>(data[off + 7]);
                    out.reporterSSRCs.push_back(ssrc);
                }
            } else if (pt == kRTCP_SDES) {
                parseSDES(data + off, pktBytes, out);
            }
            off += pktBytes;
        }
        out.valid = sawAny;
        return out;
    }

    void record(uint32_t ssrc, const std::string& sourceIp, const std::string& cname,
                std::chrono::steady_clock::time_point now) {
        auto& r = rows_[ssrc];
        if (r.packetCount == 0) { r.ssrc = ssrc; r.firstSeen = now; }
        if (!sourceIp.empty()) r.sourceIp = sourceIp;
        if (!cname.empty()) r.cname = cname;
        r.lastSeen = now;
        ++r.packetCount;
    }

    void sweep(std::chrono::steady_clock::time_point now) {
        for (auto it = rows_.begin(); it != rows_.end();) {
            if (now - it->second.lastSeen > kReporterTimeout) it = rows_.erase(it);
            else ++it;
        }
    }

    std::vector<RTCPReporter> reporters() const {
        std::vector<RTCPReporter> out;
        out.reserve(rows_.size());
        for (const auto& kv : rows_) out.push_back(kv.second);
        return out;
    }

    size_t size() const { return rows_.size(); }
    void clear() { rows_.clear(); }

private:
    // SDES: after the 4-byte RTCP header, a list of chunks, each = 4-byte SSRC
    // then TLV items terminated by a type-0 byte, padded to a 4-byte boundary.
    // We pull CNAME (type 1) only.
    static void parseSDES(const uint8_t* pkt, size_t pktBytes, RTCPParseResult& out) {
        size_t p = 4; // past the RTCP header
        while (p + 4 <= pktBytes) {
            const uint32_t ssrc = (static_cast<uint32_t>(pkt[p]) << 24) |
                                  (static_cast<uint32_t>(pkt[p + 1]) << 16) |
                                  (static_cast<uint32_t>(pkt[p + 2]) << 8) |
                                  static_cast<uint32_t>(pkt[p + 3]);
            p += 4;
            // TLV items until a 0 type byte.
            while (p < pktBytes) {
                const uint8_t type = pkt[p++];
                if (type == 0) break; // end of this chunk's items
                if (p >= pktBytes) return; // malformed
                const uint8_t itemLen = pkt[p++];
                if (p + itemLen > pktBytes) return; // malformed
                if (type == 1 /*CNAME*/) {
                    out.cnames.emplace_back(
                        ssrc, std::string(reinterpret_cast<const char*>(pkt + p), itemLen));
                }
                p += itemLen;
            }
            // Advance to the next 4-byte boundary (chunk padding).
            p = (p + 3) & ~static_cast<size_t>(3);
        }
    }

    std::map<uint32_t, RTCPReporter> rows_;
};

} // namespace AES67

#endif // RTCP_RECEIVER_TABLE_H
