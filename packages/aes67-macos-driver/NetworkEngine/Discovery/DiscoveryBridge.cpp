//
// DiscoveryBridge.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/DiscoveryBridge.h"

#include "NetworkEngine/Discovery/RTSPSessionDiscovery.h"
#include "NetworkEngine/Discovery/SAPListener.h"
#include "NetworkEngine/Discovery/SessionDirectory.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

/// A JSON string, escaped the way RFC 8259 asks. Hand-written, like every
/// other JSON in this tree: what goes through here is an SDP and a handful of
/// addresses, and a dependency for that would be the larger decision.
std::string jsonString(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escape[7];
                    std::snprintf(escape, sizeof(escape), "\\u%04x", c);
                    out += escape;
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

char* duplicate(const std::string& text) {
    char* copy = static_cast<char*>(std::malloc(text.size() + 1));
    if (!copy) return nullptr;
    std::memcpy(copy, text.c_str(), text.size() + 1);
    return copy;
}

} // namespace

/// What the handle owns: the one directory both discoverers file into, and
/// whichever of them the caller asked for.
struct AES67DiscoveryHandle {
    AES67::SessionDirectory directory;
    std::unique_ptr<AES67::SAPListener> sap;
    std::unique_ptr<AES67::RTSPSessionDiscovery> rtsp;
};

extern "C" {

AES67DiscoveryHandle* aes67_discovery_start(const char* interface_ip,
                                            int enable_sap, int enable_rtsp) {
    auto handle = std::make_unique<AES67DiscoveryHandle>();
    const std::string interfaceIP = interface_ip ? interface_ip : "";
    int started = 0;

    if (enable_sap) {
        handle->sap = std::make_unique<AES67::SAPListener>();
        handle->sap->registerAnnouncementCallback(
            [raw = handle.get()](const AES67::SAPAnnouncement& announcement) {
                if (announcement.isDeletion) {
                    raw->directory.forget(AES67::SessionDirectory::identityOf(
                        announcement.sessionDescription, announcement.multicastAddress,
                        announcement.port));
                    return;
                }
                if (announcement.sessionDescription.empty()) return;

                AES67::DiscoveredSessionEntry entry;
                entry.sessionName = announcement.sessionName;
                entry.sourceAddress = announcement.sourceAddress;
                entry.multicastAddress = announcement.multicastAddress;
                entry.port = announcement.port;
                entry.ptpDomain = announcement.ptpDomain;
                entry.sessionDescription = announcement.sessionDescription;
                entry.sources = {AES67::DiscoverySource::SAP};
                raw->directory.offer(entry);
            });
        if (handle->sap->initialize(interfaceIP) && handle->sap->start()) {
            ++started;
        } else {
            handle->sap.reset();
        }
    }

    if (enable_rtsp) {
        handle->rtsp = std::make_unique<AES67::RTSPSessionDiscovery>(handle->directory);
        if (handle->rtsp->start()) {
            ++started;
        } else {
            handle->rtsp.reset();
        }
    }

    if (started == 0) return nullptr;
    return handle.release();
}

char* aes67_discovery_sessions_json(AES67DiscoveryHandle* handle) {
    if (!handle) return duplicate("[]");

    std::string json = "[";
    bool first = true;
    for (const AES67::DiscoveredSessionEntry& session : handle->directory.sessions()) {
        if (!first) json += ",";
        first = false;

        json += "{";
        json += "\"sessionName\":" + jsonString(session.sessionName) + ",";
        json += "\"sourceAddress\":" + jsonString(session.sourceAddress) + ",";
        json += "\"multicastAddress\":" + jsonString(session.multicastAddress) + ",";
        json += "\"port\":" + std::to_string(session.port) + ",";
        json += "\"ptpDomain\":" + std::to_string(session.ptpDomain) + ",";
        json += "\"sdp\":" + jsonString(session.sessionDescription) + ",";
        json += "\"sources\":[";
        bool firstSource = true;
        for (AES67::DiscoverySource source : session.sources) {
            if (!firstSource) json += ",";
            firstSource = false;
            json += jsonString(AES67::discoverySourceName(source));
        }
        json += "]}";
    }
    json += "]";
    return duplicate(json);
}

void aes67_discovery_free_string(char* text) {
    std::free(text);
}

void aes67_discovery_stop(AES67DiscoveryHandle* handle) {
    if (!handle) return;
    if (handle->rtsp) handle->rtsp->stop();
    if (handle->sap) handle->sap->stop();
    delete handle;
}

} // extern "C"
