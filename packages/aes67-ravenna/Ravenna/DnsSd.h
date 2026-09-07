//
// DnsSd.h
// AES67 RAVENNA session layer
// The DNS records that make a session discoverable, as bytes.
//
// RAVENNA finds sessions with DNS-SD over mDNS (RFC 6762 and 6763): a device
// asks for _rtsp._tcp on the local link and gets back, for each session, a
// PTR naming the instance, an SRV saying which host and port to DESCRIBE, a
// TXT, and an A with the address. RAVENNA's own subtype, _ravenna_session,
// is what separates its sessions from every other RTSP service on the
// network -- a camera's stream is also _rtsp._tcp.
//
// Encoding is here and sending is not, so the packet can be checked byte for
// byte in a test rather than with a capture.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AES67::Ravenna {

/// The link-local group and port of mDNS (RFC 6762 sec 3).
inline constexpr char kMdnsGroup[] = "224.0.0.251";
inline constexpr uint16_t kMdnsPort = 5353;

/// What RAVENNA advertises under. The service is plain RTSP; the subtype is
/// what makes it a RAVENNA session rather than any other RTSP service.
inline constexpr char kRtspService[] = "_rtsp._tcp.local";
inline constexpr char kRavennaSessionSubtype[] = "_ravenna_session._sub._rtsp._tcp.local";

/// Record types and the class, from RFC 1035, plus the cache-flush bit
/// mDNS puts in the top of the class field (RFC 6762 sec 10.2).
inline constexpr uint16_t kTypeA = 1;
inline constexpr uint16_t kTypePTR = 12;
inline constexpr uint16_t kTypeTXT = 16;
inline constexpr uint16_t kTypeSRV = 33;
inline constexpr uint16_t kClassIN = 1;
inline constexpr uint16_t kCacheFlush = 0x8000;

/// Seconds. RFC 6763 sec 10: two minutes for the records that name a host,
/// 75 minutes for the rest. A session that goes away is withdrawn with a
/// goodbye rather than waited out, so the long one costs nothing.
inline constexpr uint32_t kHostRecordTtl = 120;
inline constexpr uint32_t kServiceRecordTtl = 4500;

/// What an NMOS node registers itself as, for a controller browsing the link
/// rather than a registry (IS-04 peer-to-peer discovery).
inline constexpr char kNmosNodeService[] = "_nmos-node._tcp.local";

/// One advertised service: a RAVENNA session, or the NMOS node itself.
struct SessionAdvertisement {
    std::string instanceName;   ///< what a person sees in a session list
    std::string hostName;       ///< "box.local", the name the SRV points at
    uint16_t port = 554;        ///< where the server answers
    uint32_t addressV4 = 0;     ///< host byte order
    std::vector<std::string> txtEntries;  ///< "key=value", RFC 6763 sec 6

    /// The service this is advertised under, and the subtype if it has one.
    /// A RAVENNA session is RTSP with RAVENNA's subtype; an NMOS node is its
    /// own service with none.
    std::string serviceType = kRtspService;
    std::string subtype = kRavennaSessionSubtype;
};

/// A DNS name as length-prefixed labels ending in a zero byte. No compression
/// pointers: they save a few dozen bytes on a packet sent once a minute and
/// they are where hand-written encoders go wrong.
std::vector<uint8_t> encodeName(const std::string& name);

/// The answer to a query for our service: PTR, SRV, TXT and A, in that order,
/// as one mDNS response packet with the authoritative bit set.
///
/// `includeSubtype` adds the second PTR, from the subtype to the same
/// instance, which is what a RAVENNA device browses for. A service with no
/// subtype gets one PTR whatever this says.
std::vector<uint8_t> buildAnnouncement(const SessionAdvertisement& session,
                                       bool includeSubtype = true);

/// The same records with a TTL of zero, which is how RFC 6762 sec 10.1 says
/// a service is withdrawn. Sent when a session stops, so nobody waits 75
/// minutes for it to expire.
std::vector<uint8_t> buildGoodbye(const SessionAdvertisement& session,
                                  bool includeSubtype = true);

/// The question names in a query packet, lowercased. Anything unparseable
/// returns empty rather than throwing: this reads packets from the network.
std::vector<std::string> parseQueryNames(const uint8_t* packet, size_t length);

}  // namespace AES67::Ravenna
