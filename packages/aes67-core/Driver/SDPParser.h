//
// SDPParser.h
// AES67 macOS Driver - Build #1
// SDP (Session Description Protocol) parser with Riedel Artist compatibility
//

#pragma once

#include "../Shared/Types.h"
#include <string>
#include <vector>
#include <optional>
#include <map>

namespace AES67 {

//
// SDP Session
//
// Represents a complete SDP session description for an AES67 audio stream
// Compatible with Riedel Artist SDP format
//
struct SDPSession {
    // Session-level information
    std::string sessionName;        // s=
    std::string sessionInfo;        // i=
    uint64_t sessionID{0};          // o= (session ID)
    uint64_t sessionVersion{0};     // o= (version)

    // Origin
    // o=<username> <sess-id> <sess-version> <nettype> <addrtype> <unicast-address>
    // (RFC 4566 §5.2). The two type fields used to default the other way
    // round -- nettype "IP4", addrtype "IN" -- which the parser never showed,
    // since it fills both from the line it reads, and every generated session
    // that was not first parsed went out as "o=- 1 1 IP4 IN ". Dante Controller
    // keys a flow on this line's address and session id (RtpFlowIdentity).
    std::string originUsername{"-"};       // o= (username, usually "-")
    std::string originAddress;             // o= (unicast address)
    std::string originNetworkType{"IN"};   // o= <nettype>, "IN"
    std::string originAddressType{"IP4"};  // o= <addrtype>, "IP4"

    // Connection (c=)
    std::string connectionAddress;   // Usually multicast IP
    std::string connectionType{"IN"};
    std::string connectionNetwork{"IP4"};
    uint8_t ttl{32};                // TTL for multicast

    // Timing (t=)
    uint64_t timeStart{0};          // Usually 0 (permanent session)
    uint64_t timeStop{0};           // Usually 0 (permanent session)

    // Media (m=)
    std::string mediaType{"audio"};
    uint16_t port{5004};
    std::string transport{"RTP/AVP"};
    uint8_t payloadType{96};        // Dynamic payload type

    // Audio format (a=rtpmap)
    std::string encoding{"L24"};    // L16, L24, or AM824
    uint32_t sampleRate{48000};
    uint16_t numChannels{2};

    // Packet timing (a=ptime, a=framecount)
    //
    // Microseconds, not milliseconds: SDP's a=ptime is a DECIMAL number of
    // milliseconds, and the values that matter below 1 ms are real —
    // ST 2110-30 Levels B and C are 125 us. Holding this as integer
    // milliseconds silently truncated "a=ptime:0.125" to zero.
    uint32_t ptimeUs{1000};         // Packet time in MICROSECONDS
    // Samples per packet (a=framecount, a RAVENNA extension). 0 = not
    // specified in the SDP, in which case packet size is derived from
    // ptimeUs. A nonzero default here would mask a sub-millisecond a=ptime
    // that came without a framecount — see RTPTransmitter/RTPReceiver.
    uint32_t framecount{0};          // 0 = derive from ptime

    // Source filter (a=source-filter)
    std::string sourceAddress;      // Unicast source IP

    // PTP timing (a=ts-refclk, a=mediaclk)
    int ptpDomain{0};               // -1 = no PTP
    std::string ptpMasterMAC;       // PTP grandmaster MAC address
    bool ptpTraceable{false};        // a=ts-refclk:ptp=IEEE1588-2008:traceable
                                    // (grandmaster locked to a traceable primary
                                    // reference, e.g. GPS); per RFC 7273 no
                                    // gmid/domain is pinned. Mutually exclusive
                                    // with a named ptpMasterMAC.
    std::string mediaClockType{"direct=0"};  // Media clock reference

    // Per-source DSCP override for THIS transmit stream. -1 (default) = use
    // the active profile's recommendedDscp. Driver-side transmit property,
    // not an SDP attribute (DSCP is a network egress marking, absent from
    // RFC 4566), so it is neither parsed from nor written to the wire SDP;
    // it lets one stream be marked differently from the profile default.
    int dscp{-1};

    // Additional attributes
    std::string direction{"recvonly"};  // sendonly, recvonly, sendrecv, inactive
    std::map<std::string, std::string> customAttributes;

    // Validation
    bool isValid() const;
    std::vector<std::string> getValidationErrors() const;
};

//
// SDP Parser
//
// Parses and generates SDP files according to RFC 4566 with AES67 extensions
// Fully compatible with Riedel Artist SDP format
//
class SDPParser {
public:
    //
    // Parse SDP from file
    //
    static std::optional<SDPSession> parseFile(const std::string& filepath);

    //
    // Parse SDP from string
    //
    static std::optional<SDPSession> parseString(const std::string& sdp);

    //
    // Generate SDP string from session
    //
    static std::string generate(const SDPSession& session);

    //
    // Write SDP session to file
    //
    static bool writeFile(const SDPSession& session, const std::string& filepath);

    //
    // Validate SDP session
    //
    static bool validate(const SDPSession& session, std::vector<std::string>* errors = nullptr);

    //
    // Create default SDP session for transmit
    //
    static SDPSession createDefaultTxSession(
        const std::string& name,
        const std::string& sourceIP,
        const std::string& multicastIP,
        uint16_t port,
        uint16_t numChannels,
        uint32_t sampleRate,
        const std::string& encoding = "L24"
    );

    //
    // Extract stream info from SDP session
    //
    static StreamInfo toStreamInfo(const SDPSession& session);

    //
    // Create SDP session from stream info
    //
    static SDPSession fromStreamInfo(const StreamInfo& info);

private:
    // Parsing helper functions
    static bool parseSessionLine(const std::string& line, SDPSession& session);
    static bool parseOriginLine(const std::string& line, SDPSession& session);
    static bool parseConnectionLine(const std::string& line, SDPSession& session);
    static bool parseTimingLine(const std::string& line, SDPSession& session);
    static bool parseMediaLine(const std::string& line, SDPSession& session);
    static bool parseAttributeLine(const std::string& line, SDPSession& session);

    // Attribute parsers (a=)
    static bool parseRTPMapAttribute(const std::string& value, SDPSession& session);
    static bool parsePTimeAttribute(const std::string& value, SDPSession& session);
    static bool parseFrameCountAttribute(const std::string& value, SDPSession& session);
    static bool parseSourceFilterAttribute(const std::string& value, SDPSession& session);
    static bool parsePTPRefClockAttribute(const std::string& value, SDPSession& session);
    static bool parseMediaClockAttribute(const std::string& value, SDPSession& session);
    static bool parseDirectionAttribute(const std::string& value, SDPSession& session);

    // Generation helper functions
    static std::string generateOriginLine(const SDPSession& session);
    static std::string generateConnectionLine(const SDPSession& session);
    static std::string generateMediaLine(const SDPSession& session);
    static std::vector<std::string> generateAttributes(const SDPSession& session);

    // Utility functions
    static std::vector<std::string> splitLines(const std::string& text);
    static std::vector<std::string> splitString(const std::string& str, char delimiter);
    static std::string trim(const std::string& str);
    static bool startsWith(const std::string& str, const std::string& prefix);

    // Validation helpers
    static bool isValidIPv4(const std::string& ip);
    static bool isValidPort(uint16_t port);
    static bool isValidSampleRate(uint32_t sampleRate);
    static bool isValidEncoding(const std::string& encoding);
};

} // namespace AES67
