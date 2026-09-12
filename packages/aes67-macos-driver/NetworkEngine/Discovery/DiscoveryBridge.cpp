//
// DiscoveryBridge.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/DiscoveryBridge.h"

#include "NetworkEngine/Discovery/RTSPSessionDiscovery.h"
#include "NetworkEngine/Discovery/SAPListener.h"
#include "NetworkEngine/Discovery/SessionDirectory.h"
#include "NetworkEngine/Discovery/MDNSBrowser.h"
#include "NetworkEngine/PTP/PTPPeerObserver.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

/// The service types this world registers on a link. Browsing all of them is
/// what makes one list of a room rather than one list per ecosystem.
///
///  - `_rtsp._tcp`          RAVENNA, and AES67 gear that serves its SDP
///  - `_nmos-node._tcp`     an NMOS node; `_nmos-register._tcp` its registry
///  - `_netaudio-*._udp`    Dante: arc is control, cmc the device itself,
///                          chan its channels, dbc its database. Reading a
///                          registration is not speaking the protocol.
///  - `_ravenna._tcp`       what some RAVENNA gear registers beside RTSP
const char* const kBrowsedServiceTypes[] = {
    "_rtsp._tcp",
    "_nmos-node._tcp",
    "_nmos-register._tcp",
    "_netaudio-arc._udp",
    "_netaudio-cmc._udp",
    "_netaudio-chan._udp",
    "_netaudio-dbc._udp",
    "_ravenna._tcp",
};

} // namespace

/// What the handle owns: the one directory both discoverers file into, and
/// whichever of them the caller asked for.
struct AES67DiscoveryHandle {
    AES67::SessionDirectory directory;
    std::unique_ptr<AES67::SAPListener> sap;
    std::unique_ptr<AES67::RTSPSessionDiscovery> rtsp;
    std::unique_ptr<AES67::PTPPeerObserver> ptp;
    std::vector<std::unique_ptr<AES67::MDNSBrowser>> services;
};

extern "C" {

AES67DiscoveryHandle* aes67_discovery_start(const char* interface_ip,
                                            const char* interface_name,
                                            int enable_sap, int enable_rtsp, int enable_ptp,
                                            int enable_services) {
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

    if (enable_ptp) {
        handle->ptp = std::make_unique<AES67::PTPPeerObserver>();
        if (handle->ptp->start(interface_name ? interface_name : "")) {
            ++started;
        } else {
            handle->ptp.reset();
        }
    }

    if (enable_services) {
        for (const char* type : kBrowsedServiceTypes) {
            auto browser = std::make_unique<AES67::MDNSBrowser>(type);
            if (browser->start()) {
                handle->services.push_back(std::move(browser));
                ++started;
            }
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

char* aes67_discovery_ptp_peers_json(AES67DiscoveryHandle* handle) {
    if (!handle || !handle->ptp) return duplicate("[]");

    const auto now = std::chrono::steady_clock::now();
    std::string json = "[";
    bool first = true;
    for (const AES67::PTPPeerObservation& peer : handle->ptp->peers()) {
        if (!first) json += ",";
        first = false;

        char clockId[32];
        std::snprintf(clockId, sizeof(clockId), "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                      peer.clockId[0], peer.clockId[1], peer.clockId[2], peer.clockId[3],
                      peer.clockId[4], peer.clockId[5], peer.clockId[6], peer.clockId[7]);
        char oui[16];
        const auto vendor = peer.oui();
        std::snprintf(oui, sizeof(oui), "%02x:%02x:%02x", vendor[0], vendor[1], vendor[2]);

        const char* role = "unknown";
        switch (peer.role()) {
            case AES67::PTPPeerRole::Master: role = "master"; break;
            case AES67::PTPPeerRole::Slave:  role = "slave";  break;
            case AES67::PTPPeerRole::Mixed:  role = "mixed";  break;
            case AES67::PTPPeerRole::Unknown: break;
        }

        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - peer.lastSeen).count();

        json += "{";
        json += "\"clockId\":" + jsonString(clockId) + ",";
        json += "\"oui\":" + jsonString(oui) + ",";
        json += "\"role\":" + jsonString(role) + ",";
        json += "\"sourceIp\":" + jsonString(peer.sourceIp) + ",";
        json += "\"domain\":" + std::to_string(peer.domain) + ",";
        json += "\"messageCount\":" + std::to_string(peer.messageCount) + ",";
        json += "\"secondsSinceLastSeen\":" + std::to_string(age);
        json += "}";
    }
    json += "]";
    return duplicate(json);
}

char* aes67_discovery_services_json(AES67DiscoveryHandle* handle) {
    if (!handle || handle->services.empty()) return duplicate("[]");

    const auto now = std::chrono::steady_clock::now();
    std::string json = "[";
    bool first = true;
    for (const auto& browser : handle->services) {
        for (const AES67::MDNSService& service : browser->discoveredServices()) {
            if (!first) json += ",";
            first = false;

            const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - service.lastSeen).count();

            json += "{";
            json += "\"name\":" + jsonString(service.name) + ",";
            json += "\"type\":" + jsonString(service.type) + ",";
            json += "\"host\":" + jsonString(service.hostTarget) + ",";
            json += "\"address\":" + jsonString(service.address) + ",";
            json += "\"port\":" + std::to_string(service.port) + ",";
            json += "\"secondsSinceLastSeen\":" + std::to_string(age);
            json += "}";
        }
    }
    json += "]";
    return duplicate(json);
}

void aes67_discovery_free_string(char* text) {
    std::free(text);
}

void aes67_discovery_stop(AES67DiscoveryHandle* handle) {
    if (!handle) return;
    for (auto& browser : handle->services) browser->stop();
    if (handle->ptp) handle->ptp->stop();
    if (handle->rtsp) handle->rtsp->stop();
    if (handle->sap) handle->sap->stop();
    delete handle;
}

} // extern "C"
