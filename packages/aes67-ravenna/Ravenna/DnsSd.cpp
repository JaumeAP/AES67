#include "Ravenna/DnsSd.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

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

namespace {

/// Reads a name at `offset`, following compression pointers. Returns false on
/// anything malformed, which includes a pointer that goes forwards or a chain
/// longer than the packet could justify: this parses packets off an open
/// network and a loop is two bytes to write.
bool readName(const uint8_t* packet, size_t length, size_t offset, std::string& name,
              size_t& afterName) {
    name.clear();
    bool jumped = false;
    afterName = offset;
    int jumps = 0;

    while (offset < length) {
        const uint8_t label = packet[offset];
        if ((label & 0xC0) == 0xC0) {
            if (offset + 1 >= length) return false;
            const size_t target =
                (static_cast<size_t>(label & 0x3F) << 8) | packet[offset + 1];
            // Only backwards, and only so many times: forwards or in circles
            // is how a hand-written parser spins for ever.
            if (target >= offset) return false;
            if (++jumps > 16) return false;
            if (!jumped) afterName = offset + 2;
            jumped = true;
            offset = target;
            continue;
        }
        if ((label & 0xC0) != 0) return false;
        if (label == 0) {
            if (!jumped) afterName = offset + 1;
            return true;
        }
        if (offset + 1 + label > length) return false;

        if (!name.empty()) name += '.';
        for (size_t c = 0; c < label; ++c) {
            name += static_cast<char>(
                std::tolower(static_cast<unsigned char>(packet[offset + 1 + c])));
        }
        offset += 1 + label;
    }
    return false;
}

uint16_t readBE16(const uint8_t* at) {
    return static_cast<uint16_t>((at[0] << 8) | at[1]);
}

}  // namespace

std::string DiscoveredService::txt(const std::string& key, const std::string& fallback) const {
    const std::string prefix = key + "=";
    for (const std::string& entry : txtEntries) {
        if (entry.rfind(prefix, 0) == 0) return entry.substr(prefix.size());
    }
    return fallback;
}

std::vector<uint8_t> buildQuery(const std::string& serviceType) {
    std::vector<uint8_t> packet;
    // A query: no id, no flags, one question and nothing else.
    const uint8_t header[12] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    packet.insert(packet.end(), std::begin(header), std::end(header));

    const std::vector<uint8_t> name = encodeName(serviceType);
    packet.insert(packet.end(), name.begin(), name.end());
    packet.push_back(0);
    packet.push_back(static_cast<uint8_t>(kTypePTR));
    packet.push_back(0);
    packet.push_back(static_cast<uint8_t>(kClassIN));
    return packet;
}

std::vector<DiscoveredService> parseServiceResponse(const uint8_t* packet, size_t length,
                                                    const std::string& serviceType) {
    std::vector<DiscoveredService> found;
    if (packet == nullptr || length < 12) return found;

    const uint16_t flags = readBE16(packet + 2);
    if ((flags & 0x8000) == 0) return found;  // a query, not a response

    const uint16_t questions = readBE16(packet + 4);
    const size_t records = static_cast<size_t>(readBE16(packet + 6)) +
                           readBE16(packet + 8) + readBE16(packet + 10);

    size_t offset = 12;
    for (uint16_t i = 0; i < questions; ++i) {
        std::string question;
        size_t after = 0;
        if (!readName(packet, length, offset, question, after)) return found;
        offset = after + 4;  // qtype and qclass
        if (offset > length) return found;
    }

    // A record is only this service's if a PTR for the type asked about
    // named its instance: an SRV on its own belongs to whatever else the
    // responder happens to advertise, and taking it would report a node as a
    // registry. So the records are gathered by owner first and matched to the
    // PTRs afterwards, because a responder may put them in any order.
    std::string wanted = serviceType;
    for (char& letter : wanted) letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));

    std::vector<std::string> instances;
    std::vector<std::pair<std::string, DiscoveredService>> byOwner;
    std::vector<std::pair<std::string, uint32_t>> addresses;

    const auto owned = [&byOwner](const std::string& owner) -> DiscoveredService& {
        for (auto& [name, service] : byOwner) {
            if (name == owner) return service;
        }
        byOwner.emplace_back(owner, DiscoveredService{});
        byOwner.back().second.instanceName = owner;
        return byOwner.back().second;
    };

    for (size_t i = 0; i < records; ++i) {
        std::string owner;
        size_t after = 0;
        if (!readName(packet, length, offset, owner, after)) break;
        if (after + 10 > length) break;

        const uint16_t type = readBE16(packet + after);
        const uint16_t dataLength = readBE16(packet + after + 8);
        const size_t data = after + 10;
        if (data + dataLength > length) break;

        if (type == kTypePTR && owner == wanted) {
            std::string instance;
            size_t ignored = 0;
            if (readName(packet, length, data, instance, ignored) && !instance.empty()) {
                instances.push_back(instance);
            }
        } else if (type == kTypeSRV && dataLength >= 6) {
            std::string host;
            size_t ignored = 0;
            if (readName(packet, length, data + 6, host, ignored)) {
                DiscoveredService& service = owned(owner);
                service.port = readBE16(packet + data + 4);
                service.hostName = host;
            }
        } else if (type == kTypeTXT) {
            DiscoveredService& service = owned(owner);
            size_t at = data;
            while (at < data + dataLength) {
                const uint8_t entryLength = packet[at];
                if (at + 1 + entryLength > data + dataLength) break;
                if (entryLength > 0) {
                    service.txtEntries.emplace_back(
                        reinterpret_cast<const char*>(packet + at + 1), entryLength);
                }
                at += 1 + entryLength;
            }
        } else if (type == kTypeA && dataLength == 4) {
            const uint32_t address = (static_cast<uint32_t>(packet[data]) << 24) |
                                     (static_cast<uint32_t>(packet[data + 1]) << 16) |
                                     (static_cast<uint32_t>(packet[data + 2]) << 8) |
                                     packet[data + 3];
            addresses.emplace_back(owner, address);
        }

        offset = data + dataLength;
    }

    // Every instance a PTR named, with whatever else arrived for it. A PTR on
    // its own is normal rather than broken: a responder is free to send the
    // SRV, the TXT and the A in later packets, and a browser that dropped the
    // name would have nothing to attach them to when they came.
    for (const std::string& instance : instances) {
        DiscoveredService service;
        service.instanceName = instance;
        for (const auto& [owner, gathered] : byOwner) {
            if (owner != instance) continue;
            service = gathered;
            break;
        }
        found.push_back(service);
    }

    for (DiscoveredService& service : found) {
        for (const auto& [host, address] : addresses) {
            if (host == service.hostName) {
                service.addressV4 = address;
                break;
            }
        }
    }
    return found;
}

}  // namespace AES67::Ravenna
