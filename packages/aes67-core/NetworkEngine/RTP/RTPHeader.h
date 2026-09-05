//
// RTPHeader.h
// AES67 macOS Driver
// The RTP wire header (RFC 3550 §5.1) and its payload types, with no
// dependency on sockets or on any platform.
//
// Split out of SimpleRTP.h so that building or reading an RTP header does not
// drag in <sys/socket.h>. SimpleRTP.h is the transport; this is the format, and
// the two have different audiences: anything that speaks RTP over a stack this
// project does not own -- ESP32 firmware over lwIP, a test harness, a parser --
// needs the format alone. It is part of AES67_CORE_SOURCES' promise (see the
// README) precisely because it has nothing platform-specific in it.
//
// The byte-order helpers are hand-written rather than htons/ntohs for the same
// reason: those live in <arpa/inet.h>, which would defeat the point.
//

#pragma once

#include <cstdint>
#include <cstddef>

namespace AES67 {
namespace RTP {

namespace detail {

// RTP is big-endian on the wire. On a big-endian host these are identities,
// which is what the compile-time check below is for -- the swap is applied
// only where it is actually needed, exactly as htons/ntohs would.
constexpr bool kHostIsLittleEndian =
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#else
    true;  // every platform this code has run on; revisit if that changes
#endif

constexpr uint16_t swap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

constexpr uint32_t swap32(uint32_t v) {
    return ((v >> 24) & 0x000000FFu) | ((v >> 8) & 0x0000FF00u) |
           ((v << 8) & 0x00FF0000u) | ((v << 24) & 0xFF000000u);
}

constexpr uint16_t hostToNetwork16(uint16_t v) { return kHostIsLittleEndian ? swap16(v) : v; }
constexpr uint32_t hostToNetwork32(uint32_t v) { return kHostIsLittleEndian ? swap32(v) : v; }

}  // namespace detail

//
// RTP Header (RFC 3550 Section 5.1)
//
// 0                   1                   2                   3
// 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|X|  CC   |M|     PT      |       sequence number         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           timestamp                           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |           synchronization source (SSRC) identifier            |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//
#pragma pack(push, 1)
struct RTPHeader {
    // Byte 0
    uint8_t cc:4;          // CSRC count
    uint8_t extension:1;   // Extension bit
    uint8_t padding:1;     // Padding bit
    uint8_t version:2;     // Version (always 2)

    // Byte 1
    uint8_t payloadType:7; // Payload type
    uint8_t marker:1;      // Marker bit

    // Bytes 2-3
    uint16_t sequenceNumber;

    // Bytes 4-7
    uint32_t timestamp;

    // Bytes 8-11
    uint32_t ssrc;

    // Convert to network byte order
    void toNetworkOrder() {
        sequenceNumber = detail::hostToNetwork16(sequenceNumber);
        timestamp = detail::hostToNetwork32(timestamp);
        ssrc = detail::hostToNetwork32(ssrc);
    }

    // Convert from network byte order. The swap is its own inverse, so this is
    // the same operation under a name that says which direction the caller is
    // going.
    void toHostOrder() {
        sequenceNumber = detail::hostToNetwork16(sequenceNumber);
        timestamp = detail::hostToNetwork32(timestamp);
        ssrc = detail::hostToNetwork32(ssrc);
    }
};
#pragma pack(pop)

static_assert(sizeof(RTPHeader) == 12, "RTP header must be 12 bytes");

//
// RTP Payload Types (RFC 3551)
//
constexpr uint8_t PT_PCMU = 0;      // G.711 μ-law
constexpr uint8_t PT_GSM = 3;       // GSM
constexpr uint8_t PT_G723 = 4;      // G.723
constexpr uint8_t PT_PCMA = 8;      // G.711 A-law
constexpr uint8_t PT_L16_2CH = 10;  // L16 stereo
constexpr uint8_t PT_L16_1CH = 11;  // L16 mono
constexpr uint8_t PT_DYNAMIC = 96;  // Dynamic payload types start here

//
// AES67 uses dynamic payload types (96-127) for L16/L24
//
constexpr uint8_t PT_AES67_L16 = 96;
constexpr uint8_t PT_AES67_L24 = 97;

}  // namespace RTP
}  // namespace AES67
