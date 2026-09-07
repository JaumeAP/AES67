#include "Ravenna/DnsSd.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace AES67::Ravenna {
namespace {

void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// A record header: name, type, class with the cache-flush bit, TTL, and the
/// length of what follows.
void putRecordHeader(std::vector<uint8_t>& out, const std::string& name, uint16_t type,
                     uint32_t ttl, uint16_t dataLength, bool cacheFlush) {
    appendBytes(out, encodeName(name));
    put16(out, type);
    put16(out, static_cast<uint16_t>(kClassIN | (cacheFlush ? kCacheFlush : 0)));
    put32(out, ttl);
    put16(out, dataLength);
}

std::string instanceFqdn(const SessionAdvertisement& session) {
    return session.instanceName + "." + session.serviceType;
}

/// PTR is a shared record: several sessions answer the same name, so it never
/// carries the cache-flush bit. The rest describe one instance and do.
std::vector<uint8_t> buildRecords(const SessionAdvertisement& session, bool includeSubtype,
                                  uint32_t serviceTtl, uint32_t hostTtl) {
    std::vector<uint8_t> records;
    const std::string instance = instanceFqdn(session);
    const std::vector<uint8_t> instanceName = encodeName(instance);

    putRecordHeader(records, session.serviceType, kTypePTR, serviceTtl,
                    static_cast<uint16_t>(instanceName.size()), false);
    appendBytes(records, instanceName);

    if (includeSubtype && !session.subtype.empty()) {
        putRecordHeader(records, session.subtype, kTypePTR, serviceTtl,
                        static_cast<uint16_t>(instanceName.size()), false);
        appendBytes(records, instanceName);
    }

    const std::vector<uint8_t> hostName = encodeName(session.hostName);
    putRecordHeader(records, instance, kTypeSRV, hostTtl,
                    static_cast<uint16_t>(6 + hostName.size()), true);
    put16(records, 0);  // priority
    put16(records, 0);  // weight
    put16(records, session.port);
    appendBytes(records, hostName);

    // TXT: RFC 6763 sec 6.1 says an empty TXT is one zero-length string, not
    // an empty record. A resolver that sees a zero-length TXT treats the
    // service as absent.
    std::vector<uint8_t> txt;
    if (session.txtEntries.empty()) {
        txt.push_back(0);
    } else {
        for (const std::string& entry : session.txtEntries) {
            const size_t length = std::min<size_t>(entry.size(), 255);
            txt.push_back(static_cast<uint8_t>(length));
            txt.insert(txt.end(), entry.begin(), entry.begin() + static_cast<long>(length));
        }
    }
    putRecordHeader(records, instance, kTypeTXT, serviceTtl,
                    static_cast<uint16_t>(txt.size()), true);
    appendBytes(records, txt);

    putRecordHeader(records, session.hostName, kTypeA, hostTtl, 4, true);
    put32(records, session.addressV4);

    return records;
}

std::vector<uint8_t> buildPacket(const std::vector<uint8_t>& records, uint16_t answerCount) {
    std::vector<uint8_t> packet;
    put16(packet, 0);       // transaction id: zero in mDNS responses
    put16(packet, 0x8400);  // response, authoritative
    put16(packet, 0);       // questions
    put16(packet, answerCount);
    put16(packet, 0);       // authority
    put16(packet, 0);       // additional
    appendBytes(packet, records);
    return packet;
}

uint16_t answerCountFor(const SessionAdvertisement& session, bool includeSubtype) {
    // PTR, SRV, TXT, A, and the subtype PTR when there is one.
    return static_cast<uint16_t>(includeSubtype && !session.subtype.empty() ? 5 : 4);
}

}  // namespace

std::vector<uint8_t> encodeName(const std::string& name) {
    std::vector<uint8_t> encoded;
    size_t start = 0;

    while (start < name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();

        const size_t length = std::min<size_t>(dot - start, 63);
        if (length > 0) {
            encoded.push_back(static_cast<uint8_t>(length));
            encoded.insert(encoded.end(), name.begin() + static_cast<long>(start),
                           name.begin() + static_cast<long>(start + length));
        }
        start = dot + 1;
    }

    encoded.push_back(0);
    return encoded;
}

std::vector<uint8_t> buildAnnouncement(const SessionAdvertisement& session,
                                       bool includeSubtype) {
    return buildPacket(buildRecords(session, includeSubtype, kServiceRecordTtl,
                                    kHostRecordTtl),
                       answerCountFor(session, includeSubtype));
}

std::vector<uint8_t> buildGoodbye(const SessionAdvertisement& session, bool includeSubtype) {
    return buildPacket(buildRecords(session, includeSubtype, 0, 0),
                       answerCountFor(session, includeSubtype));
}

std::vector<std::string> parseQueryNames(const uint8_t* packet, size_t length) {
    std::vector<std::string> names;
    if (packet == nullptr || length < 12) return names;

    const uint16_t flags = static_cast<uint16_t>((packet[2] << 8) | packet[3]);
    if ((flags & 0x8000) != 0) return names;  // a response, not a query

    const uint16_t questions = static_cast<uint16_t>((packet[4] << 8) | packet[5]);
    size_t offset = 12;

    for (uint16_t i = 0; i < questions; ++i) {
        std::string name;
        while (offset < length) {
            const uint8_t label = packet[offset];
            if (label == 0) {
                ++offset;
                break;
            }
            // A compression pointer in a question is legal and rare. Following
            // it means walking the packet backwards with a loop guard; this
            // reads packets off an open network, so it stops instead.
            if ((label & 0xC0) != 0) return names;
            if (offset + 1 + label > length) return names;

            if (!name.empty()) name += '.';
            for (size_t c = 0; c < label; ++c) {
                name += static_cast<char>(
                    std::tolower(static_cast<unsigned char>(packet[offset + 1 + c])));
            }
            offset += 1 + label;
        }
        if (offset + 4 > length) return names;
        offset += 4;  // qtype and qclass
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

}  // namespace AES67::Ravenna
