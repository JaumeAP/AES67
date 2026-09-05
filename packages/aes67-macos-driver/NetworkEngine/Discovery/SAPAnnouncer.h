#ifndef SAP_ANNOUNCER_H
#define SAP_ANNOUNCER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AES67 {

// The counterpart to SAPListener: instead of learning about other people's
// sources, this periodically SENDS SAP announcements for this driver's own
// transmit streams, so remote AES67/Dante receivers can discover and
// subscribe to what we offer. Listening alone makes us able to find others;
// announcing is what makes others able to find us.
//
// RFC 2974: version-1 SAP packets to UDP 9875 on the SAP groups, repeated on
// an interval; a session that stops being repeated ages out at the receiver,
// and a session ended cleanly is withdrawn with a deletion packet (type bit
// set) carrying the same Message ID Hash + originating source it was
// announced with.
//
// The set of streams to announce is pulled from a caller-supplied provider on
// each cycle, so the announcer needs no hooks into stream lifecycle: it just
// diffs successive snapshots and emits deletions for what disappeared.
class SAPAnnouncer {
public:
    // Returns the SDP text of every source to announce right now. Called on
    // the announcer's own thread, off the real-time path.
    using SessionProvider = std::function<std::vector<std::string>()>;

    // How often to repeat each announcement. 30 s is the usual RFC 2974
    // result in practice and pairs with SAPListener::kSessionTimeout (300 s
    // = ten missed repeats) at the far end.
    static constexpr std::chrono::seconds kAnnounceInterval{30};

    SAPAnnouncer();
    ~SAPAnnouncer();

    // interfaceIp is the local IPv4 used as the SAP originating source and as
    // the multicast egress interface. Empty = default interface, 0.0.0.0 as
    // originating source.
    bool initialize(const std::string& interfaceIp = "");

    // provider is polled every kAnnounceInterval on the sender thread.
    bool start(SessionProvider provider);

    // Sends a deletion for everything still announced, then stops.
    void stop();

    // The Message ID Hash this announces an SDP body with (RFC 2974 §6): a
    // content hash, so an edited body announces as a changed session and the
    // deletion that withdraws a body carries the value it went out with.
    static uint16_t messageIdHash(const std::string& sdp);

    // The datagram this puts on the wire for one session, header and body.
    // `originatingSource` is an IPv4 address in network byte order.
    //
    // Static and public so the packets can be checked against the parser
    // that has to read them — SAPListener::parseAnnouncement — without a
    // socket or a 30-second wait. Announcing was at zero coverage until
    // 2026-09-04, and a header this driver alone agrees with would be
    // invisible to every receiver on the network.
    static std::vector<uint8_t> buildPacket(const std::string& sdp, uint16_t msgIdHash,
                                            uint32_t originatingSource, bool deletion);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace AES67

#endif // SAP_ANNOUNCER_H
