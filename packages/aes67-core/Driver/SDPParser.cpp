//
// SDPParser.cpp
// AES67 macOS Driver - Build #2
// SDP parser implementation with full Riedel Artist compatibility
//

#include "SDPParser.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <ctime>

namespace AES67 {

namespace {

/// Microseconds -> the decimal count of milliseconds SDP's a=ptime wants.
/// 1000 -> "1", 125 -> "0.125", 333 -> "0.333". Trailing zeros trimmed so
/// the common case stays the plain "1" every other implementation writes.
std::string formatPTimeMs(uint32_t ptimeUs) {
    char buf[32];
    (void)std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(ptimeUs) / 1000.0);
    std::string out(buf);
    if (out.find('.') != std::string::npos) {
        out.erase(out.find_last_not_of('0') + 1);
        if (!out.empty() && out.back() == '.') out.pop_back();
    }
    return out;
}

} // namespace


// ============================================================================
// Validation
// ============================================================================

bool SDPSession::isValid() const {
    return getValidationErrors().empty();
}

std::vector<std::string> SDPSession::getValidationErrors() const {
    std::vector<std::string> errors;

    if (sessionName.empty()) {
        errors.push_back("Session name (s=) is required");
    }

    if (connectionAddress.empty()) {
        errors.push_back("Connection address (c=) is required");
    }

    if (port == 0) {
        errors.push_back("Port must be non-zero");
    }

    if (encoding != "L16" && encoding != "L24" && encoding != "AM824") {
        errors.push_back("Invalid encoding: " + encoding);
    }

    if (sampleRate == 0) {
        errors.push_back("Sample rate must be non-zero");
    }

    if (numChannels == 0) {
        errors.push_back("Channel count must be non-zero");
    }

    return errors;
}

// ============================================================================
// Parsing
// ============================================================================

std::optional<SDPSession> SDPParser::parseFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return parseString(buffer.str());
}

std::optional<SDPSession> SDPParser::parseString(const std::string& sdp) {
    SDPSession session;
    auto lines = splitLines(sdp);

    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') {
            continue;  // Skip empty lines and comments
        }

        if (line.size() < 2 || line[1] != '=') {
            continue;  // Invalid line format
        }

        char type = line[0];
        std::string value = line.substr(2);

        switch (type) {
            case 'v':  // Version (should be 0)
                break;

            case 'o':  // Origin
                if (!parseOriginLine(value, session)) {
                    return std::nullopt;
                }
                break;

            case 's':  // Session name
                session.sessionName = trim(value);
                break;

            case 'i':  // Session information
                session.sessionInfo = trim(value);
                break;

            case 'c':  // Connection
                if (!parseConnectionLine(value, session)) {
                    return std::nullopt;
                }
                break;

            case 't':  // Timing
                if (!parseTimingLine(value, session)) {
                    return std::nullopt;
                }
                break;

            case 'm':  // Media
                if (!parseMediaLine(value, session)) {
                    return std::nullopt;
                }
                break;

            case 'a':  // Attribute
                if (!parseAttributeLine(value, session)) {
                    return std::nullopt;
                }
                break;

            default:
                // Unknown line type, ignore
                break;
        }
    }

    // Validate parsed session
    if (!session.isValid()) {
        return std::nullopt;
    }

    return session;
}

namespace {

/// Parses a PTP domain number, returning `fallback` when the text is not one.
///
/// Exists so the two call sites below do not each need a try/catch whose
/// handler is empty -- an empty catch reads as "we did not think about it",
/// and here the intent is specific: keep the default and carry on parsing.
int parseDomainOr(const std::string& text, int fallback) {
    try {
        return std::stoi(text);
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace

bool SDPParser::parseOriginLine(const std::string& line, SDPSession& session) {
    // Format: o=<username> <sess-id> <sess-version> <nettype> <addrtype> <unicast-address>
    auto parts = splitString(line, ' ');
    if (parts.size() < 6) {
        return false;
    }

    session.originUsername = parts[0];

    try {
        session.sessionID = std::stoull(parts[1]);
        session.sessionVersion = std::stoull(parts[2]);
    } catch (...) {
        return false;
    }

    session.originNetworkType = parts[3];
    session.originAddressType = parts[4];
    session.originAddress = parts[5];

    return true;
}

bool SDPParser::parseConnectionLine(const std::string& line, SDPSession& session) {
    // Format: c=<nettype> <addrtype> <connection-address>
    // Example: c=IN IP4 239.69.83.171/32
    auto parts = splitString(line, ' ');
    if (parts.size() < 3) {
        return false;
    }

    session.connectionType = parts[0];
    session.connectionNetwork = parts[1];

    const std::string& addrPart = parts[2];

    // Handle multicast with TTL: address/ttl
    auto slashPos = addrPart.find('/');
    if (slashPos != std::string::npos) {
        session.connectionAddress = addrPart.substr(0, slashPos);
        try {
            session.ttl = std::stoi(addrPart.substr(slashPos + 1));
        } catch (...) {
            session.ttl = 32;  // Default TTL
        }
    } else {
        session.connectionAddress = addrPart;
    }

    return true;
}

bool SDPParser::parseTimingLine(const std::string& line, SDPSession& session) {
    // Format: t=<start-time> <stop-time>
    auto parts = splitString(line, ' ');
    if (parts.size() < 2) {
        return false;
    }

    try {
        session.timeStart = std::stoull(parts[0]);
        session.timeStop = std::stoull(parts[1]);
    } catch (...) {
        return false;
    }

    return true;
}

bool SDPParser::parseMediaLine(const std::string& line, SDPSession& session) {
    // Format: m=<media> <port> <proto> <fmt> ...
    // Example: m=audio 5004 RTP/AVP 96
    auto parts = splitString(line, ' ');
    if (parts.size() < 4) {
        return false;
    }

    session.mediaType = parts[0];

    try {
        session.port = std::stoi(parts[1]);
    } catch (...) {
        return false;
    }

    session.transport = parts[2];

    try {
        session.payloadType = std::stoi(parts[3]);
    } catch (...) {
        return false;
    }

    return true;
}

bool SDPParser::parseAttributeLine(const std::string& line, SDPSession& session) {
    // Format: a=<attribute> or a=<attribute>:<value>
    auto colonPos = line.find(':');
    std::string attribute, value;

    if (colonPos != std::string::npos) {
        attribute = line.substr(0, colonPos);
        value = line.substr(colonPos + 1);
    } else {
        attribute = line;
    }

    // Parse specific attributes
    if (attribute == "rtpmap") {
        return parseRTPMapAttribute(value, session);
    } else if (attribute == "ptime") {
        return parsePTimeAttribute(value, session);
    } else if (attribute == "framecount") {
        return parseFrameCountAttribute(value, session);
    } else if (attribute == "source-filter") {
        return parseSourceFilterAttribute(value, session);
    } else if (attribute == "ts-refclk") {
        return parsePTPRefClockAttribute(value, session);
    } else if (attribute == "mediaclk") {
        return parseMediaClockAttribute(value, session);
    } else if (attribute == "recvonly" || attribute == "sendonly" ||
               attribute == "sendrecv" || attribute == "inactive") {
        session.direction = attribute;
        return true;
    } else {
        // Store unknown attributes
        session.customAttributes[attribute] = value;
        return true;
    }
}

bool SDPParser::parseRTPMapAttribute(const std::string& value, SDPSession& session) {
    // Format: <payload type> <encoding name>/<clock rate>/<encoding params>
    // Example: 96 L24/48000/2
    auto parts = splitString(value, ' ');
    if (parts.size() < 2) {
        return false;
    }

    auto formatParts = splitString(parts[1], '/');
    if (formatParts.size() < 2) {
        return false;
    }

    session.encoding = formatParts[0];

    try {
        session.sampleRate = std::stoul(formatParts[1]);
        if (formatParts.size() >= 3) {
            session.numChannels = std::stoi(formatParts[2]);
        }
    } catch (...) {
        return false;
    }

    return true;
}

bool SDPParser::parsePTimeAttribute(const std::string& value, SDPSession& session) {
    // a=ptime is a decimal count of MILLISECONDS ("1", "0.125", "0.25").
    // Parsed with stod, not stoul: stoul("0.125") yields 0, which is how
    // every sub-millisecond packet time — including ST 2110-30 Levels B
    // and C at 125 us — used to be silently discarded here.
    try {
        const double ms = std::stod(value);
        if (!(ms > 0.0) || ms > 1000.0) {
            return false; // Zero, negative, NaN, or absurdly long
        }
        // std::lround, not an int cast of (x + 0.5): the guard above rules out
        // negatives today, and the cast is correct while it does, but it is
        // correct by accident. lround says what is meant.
        session.ptimeUs = static_cast<uint32_t>(std::lround(ms * 1000.0));
        return true;
    } catch (...) {
        return false;
    }
}

bool SDPParser::parseFrameCountAttribute(const std::string& value, SDPSession& session) {
    try {
        session.framecount = std::stoul(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool SDPParser::parseSourceFilterAttribute(const std::string& value, SDPSession& session) {
    // Format: incl IN IP4 <dst-address> <src-list>
    // Example: incl IN IP4 239.69.83.171 192.168.1.100
    // Note: SDP source-filter values often have a leading space after the colon
    // (e.g., "a=source-filter: incl ..."), so trim before splitting to avoid
    // an empty first token that shifts all indices.
    auto parts = splitString(trim(value), ' ');
    if (parts.size() >= 5) {
        session.sourceAddress = parts[4];
        return true;
    }
    return false;
}

bool SDPParser::parsePTPRefClockAttribute(const std::string& value, SDPSession& session) {
    // RFC 7273 traceable form: ptp=IEEE1588-2008:traceable — the source's
    // grandmaster is locked to a traceable primary reference (e.g. GPS) and
    // pins no specific gmid/domain, so the receiver must not lock to an
    // identity that is allowed to change.
    if (value.find("traceable") != std::string::npos) {
        session.ptpTraceable = true;
        session.ptpMasterMAC.clear();
        return true;
    }

    // Two grandmaster forms exist in the wild:
    //   ptp=IEEE1588-2008:<gmid>:<domain>            (RFC 7273's example, bare
    //                                                 number — what Dante,
    //                                                 RAVENNA, ST 2110 and the
    //                                                 AES67 Linux daemon emit,
    //                                                 and what generate() writes)
    //   ptp=IEEE1588-2008:<gmid>:domain-nmbr=<n>     (RFC 7273's ABNF, which
    //                                                 some tools follow)
    // and the domain may be absent entirely. Accept all of them. The gmid is
    // an EUI-64, usually hyphen-separated; the field separator is ':'.
    const std::string prefix = "ptp=IEEE1588-2008:";
    const size_t at = value.find(prefix);
    if (at != std::string::npos) {
        std::string rest = value.substr(at + prefix.size());
        // Trim trailing whitespace.
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\r' ||
                                 rest.back() == '\n' || rest.back() == '\t')) {
            rest.pop_back();
        }
        const size_t nmbr = rest.find(":domain-nmbr=");
        if (nmbr != std::string::npos) {
            session.ptpMasterMAC = rest.substr(0, nmbr);
            // A malformed domain number leaves the field at its default rather
            // than failing the whole description: a receiver can still open the
            // stream, it just does not know which PTP domain to expect. Stated
            // rather than swallowed, which is what an empty catch does.
            session.ptpDomain = parseDomainOr(rest.substr(nmbr + 13), session.ptpDomain);
            return true;
        }
        // Otherwise a trailing ":<digits>" is the bare domain number.
        const size_t lastColon = rest.rfind(':');
        if (lastColon != std::string::npos && lastColon + 1 < rest.size()) {
            const std::string tail = rest.substr(lastColon + 1);
            const bool allDigits = !tail.empty() &&
                tail.find_first_not_of("0123456789") == std::string::npos;
            if (allDigits) {
                session.ptpMasterMAC = rest.substr(0, lastColon);
                session.ptpDomain = parseDomainOr(tail, session.ptpDomain);
                return true;
            }
        }
        // gmid only, no domain — keep the gmid, leave the domain default.
        session.ptpMasterMAC = rest;
        return true;
    }

    // Unrecognized ts-refclk variant: it is informational, so never reject the
    // whole SDP over it — an interop mismatch here must not drop a valid stream.
    return true;
}

bool SDPParser::parseMediaClockAttribute(const std::string& value, SDPSession& session) {
    session.mediaClockType = value;
    return true;
}

// ============================================================================
// Generation
// ============================================================================

std::string SDPParser::generate(const SDPSession& session) {
    std::ostringstream sdp;

    // Version
    sdp << "v=0\n";

    // Origin
    sdp << generateOriginLine(session) << "\n";

    // Session name and info
    sdp << "s=" << session.sessionName << "\n";
    if (!session.sessionInfo.empty()) {
        sdp << "i=" << session.sessionInfo << "\n";
    }

    // Connection
    sdp << generateConnectionLine(session) << "\n";

    // Timing
    sdp << "t=" << session.timeStart << " " << session.timeStop << "\n";

    // Media
    sdp << generateMediaLine(session) << "\n";

    // Attributes
    auto attributes = generateAttributes(session);
    for (const auto& attr : attributes) {
        sdp << attr << "\n";
    }

    return sdp.str();
}

std::string SDPParser::generateOriginLine(const SDPSession& session) {
    std::ostringstream line;

    uint64_t sessId = session.sessionID;
    if (sessId == 0) {
        sessId = static_cast<uint64_t>(std::time(nullptr));
    }

    line << "o=" << session.originUsername
         << " " << sessId
         << " " << session.sessionVersion
         << " " << session.originNetworkType
         << " " << session.originAddressType
         << " " << session.originAddress;

    return line.str();
}

std::string SDPParser::generateConnectionLine(const SDPSession& session) {
    std::ostringstream line;
    line << "c=" << session.connectionType
         << " " << session.connectionNetwork
         << " " << session.connectionAddress;

    if (session.ttl != 0) {
        line << "/" << static_cast<int>(session.ttl);
    }

    return line.str();
}

std::string SDPParser::generateMediaLine(const SDPSession& session) {
    std::ostringstream line;
    line << "m=" << session.mediaType
         << " " << session.port
         << " " << session.transport
         << " " << static_cast<int>(session.payloadType);
    return line.str();
}

std::vector<std::string> SDPParser::generateAttributes(const SDPSession& session) {
    std::vector<std::string> attributes;

    // rtpmap
    std::ostringstream rtpmap;
    rtpmap << "a=rtpmap:" << static_cast<int>(session.payloadType)
           << " " << session.encoding
           << "/" << session.sampleRate
           << "/" << session.numChannels;
    attributes.push_back(rtpmap.str());

    // ptime
    attributes.push_back("a=ptime:" + formatPTimeMs(session.ptimeUs));

    // framecount — only when we actually have one; 0 means "derive from
    // ptime", and emitting "a=framecount:0" would be a malformed line.
    if (session.framecount > 0) {
        attributes.push_back("a=framecount:" + std::to_string(session.framecount));
    }

    // direction
    attributes.push_back("a=" + session.direction);

    // source-filter
    if (!session.sourceAddress.empty()) {
        std::ostringstream sourceFilter;
        sourceFilter << "a=source-filter: incl IN IP4 "
                    << session.connectionAddress << " "
                    << session.sourceAddress;
        attributes.push_back(sourceFilter.str());
    }

    // PTP reference clock. The traceable form omits gmid and domain (RFC
    // 7273) and takes precedence over a named grandmaster.
    //
    // The domain goes out as a bare number -- ":0" -- and not as RFC 7273's
    // ABNF "domain-nmbr=0". The bare form is the RFC's own example, AES67's
    // example, what ST 2110-10 requires, and what Dante, RAVENNA and the
    // AES67 Linux daemon all write; Dante Controller reads the field with
    // Integer.parseInt and takes nothing else (DanteInteropSim). The parser
    // above still accepts both.
    if (session.ptpTraceable) {
        attributes.push_back("a=ts-refclk:ptp=IEEE1588-2008:traceable");
    } else if (session.ptpDomain >= 0 && !session.ptpMasterMAC.empty()) {
        std::ostringstream ptpRefclk;
        ptpRefclk << "a=ts-refclk:ptp=IEEE1588-2008:"
                  << session.ptpMasterMAC
                  << ":" << session.ptpDomain;
        attributes.push_back(ptpRefclk.str());
    }

    // Media clock
    if (!session.mediaClockType.empty()) {
        attributes.push_back("a=mediaclk:" + session.mediaClockType);
    }

    // Custom attributes
    for (const auto& [key, value] : session.customAttributes) {
        if (value.empty()) {
            attributes.push_back("a=" + key);
        } else {
            std::string attribute = "a=";
            attribute += key;
            attribute += ':';
            attribute += value;
            attributes.push_back(std::move(attribute));
        }
    }

    return attributes;
}

bool SDPParser::writeFile(const SDPSession& session, const std::string& filepath) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    file << generate(session);
    return true;
}

// ============================================================================
// Validation
// ============================================================================

bool SDPParser::validate(const SDPSession& session, std::vector<std::string>* errors) {
    auto validationErrors = session.getValidationErrors();

    if (errors) {
        *errors = validationErrors;
    }

    return validationErrors.empty();
}

// ============================================================================
// Convenience Functions
// ============================================================================

SDPSession SDPParser::createDefaultTxSession(
    const std::string& name,
    const std::string& sourceIP,
    const std::string& multicastIP,
    uint16_t port,
    uint16_t numChannels,
    uint32_t sampleRate,
    const std::string& encoding
) {
    SDPSession session;

    session.sessionName = name;
    session.sessionInfo = "AES67 Stream";
    session.sessionID = static_cast<uint64_t>(std::time(nullptr));
    session.sessionVersion = 0;

    session.originUsername = "-";
    session.originAddress = sourceIP;
    session.originAddressType = "IP4";
    session.originNetworkType = "IN";

    session.connectionAddress = multicastIP;
    session.connectionType = "IN";
    session.connectionNetwork = "IP4";
    session.ttl = 32;

    session.timeStart = 0;
    session.timeStop = 0;

    session.mediaType = "audio";
    session.port = port;
    session.transport = "RTP/AVP";
    session.payloadType = 96;

    session.encoding = encoding;
    session.sampleRate = sampleRate;
    session.numChannels = numChannels;

    // Calculate ptime and framecount
    session.ptimeUs = 1000;  // 1 ms packets
    session.framecount = sampleRate / 1000;  // Samples per 1ms

    session.sourceAddress = sourceIP;
    session.direction = "sendonly";

    session.ptpDomain = 0;
    session.mediaClockType = "direct=0";

    return session;
}

StreamInfo SDPParser::toStreamInfo(const SDPSession& session) {
    StreamInfo info;

    info.id = StreamID::generate();
    info.name = session.sessionName;
    info.description = session.sessionInfo;

    // Network
    info.source.ip = session.sourceAddress;
    info.source.port = 0;  // Not in SDP

    info.multicast.ip = session.connectionAddress;
    info.multicast.port = session.port;
    info.multicast.ttl = session.ttl;

    // Audio format
    if (session.encoding == "L16") {
        info.encoding = AudioEncoding::L16;
    } else if (session.encoding == "L24") {
        info.encoding = AudioEncoding::L24;
    } else {
        info.encoding = AudioEncoding::Unknown;
    }

    info.sampleRate = session.sampleRate;
    info.numChannels = session.numChannels;
    info.payloadType = session.payloadType;

    // Timing
    info.ptime = session.ptimeUs;  // StreamInfo::ptime is microseconds too
    info.framecount = session.framecount;

    // PTP
    info.ptp.domain = session.ptpDomain;
    info.ptp.masterMAC = session.ptpMasterMAC;
    info.ptp.enabled = (session.ptpDomain >= 0);

    return info;
}

SDPSession SDPParser::fromStreamInfo(const StreamInfo& info) {
    SDPSession session;

    session.sessionName = info.name;
    session.sessionInfo = info.description;
    session.sessionID = static_cast<uint64_t>(std::time(nullptr));

    session.originAddress = info.source.ip;
    session.connectionAddress = info.multicast.ip;
    session.port = info.multicast.port;
    session.ttl = info.multicast.ttl;

    switch (info.encoding) {
        case AudioEncoding::L16:
            session.encoding = "L16";
            break;
        case AudioEncoding::L24:
            session.encoding = "L24";
            break;
        default:
            session.encoding = "L24";
    }

    session.sampleRate = info.sampleRate;
    session.numChannels = info.numChannels;
    session.payloadType = info.payloadType;

    session.ptimeUs = info.ptime;  // both microseconds
    session.framecount = info.framecount;

    session.sourceAddress = info.source.ip;

    session.ptpDomain = info.ptp.domain;
    session.ptpMasterMAC = info.ptp.masterMAC;

    return session;
}

// ============================================================================
// Utility Functions
// ============================================================================

std::vector<std::string> SDPParser::splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;

    while (std::getline(stream, line)) {
        // Remove \r if present (Windows line endings)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    return lines;
}

std::vector<std::string> SDPParser::splitString(const std::string& str, char delimiter) {
    std::vector<std::string> parts;
    std::istringstream stream(str);
    std::string part;

    while (std::getline(stream, part, delimiter)) {
        parts.push_back(trim(part));
    }

    return parts;
}

std::string SDPParser::trim(const std::string& str) {
    const char* whitespace = " \t\n\r\f\v";
    size_t start = str.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    size_t end = str.find_last_not_of(whitespace);
    return str.substr(start, end - start + 1);
}

bool SDPParser::startsWith(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() &&
           str.compare(0, prefix.size(), prefix) == 0;
}

bool SDPParser::isValidIPv4(const std::string& ip) {
    std::regex ipv4Regex(R"(^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$)");
    std::smatch match;

    if (!std::regex_match(ip, match, ipv4Regex)) {
        return false;
    }

    for (int i = 1; i <= 4; i++) {
        int octet = std::stoi(match[i]);
        if (octet < 0 || octet > 255) {
            return false;
        }
    }

    return true;
}

bool SDPParser::isValidPort(uint16_t port) {
    return port > 0;
}

bool SDPParser::isValidSampleRate(uint32_t sampleRate) {
    static const std::vector<uint32_t> validRates = {
        44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000
    };
    return std::find(validRates.begin(), validRates.end(), sampleRate) != validRates.end();
}

bool SDPParser::isValidEncoding(const std::string& encoding) {
    return encoding == "L16" || encoding == "L24" || encoding == "AM824";
}

} // namespace AES67
