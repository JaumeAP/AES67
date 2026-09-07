//
// NodeAPIRouter.h
// AES67 macOS Driver
//
// The IS-04 Node API, peer-to-peer: what a controller browsing the link
// reads to learn that this device exists and what it is made of. The
// registry client already builds every resource; this serves the same
// objects, unwrapped, at the paths IS-04 names, so a controller that reads
// them here and patches the Connection API is talking about one device.
//
// Pure: a request in, a reply out. The socket is ConnectionAPIServer's.
//
#pragma once

#include "NetworkEngine/Discovery/ConnectionAPIServer.h"
#include "NetworkEngine/Discovery/NMOSRegistrationClient.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace AES67 {

using NMOSSenderLister = std::function<std::vector<NMOSSenderResource>()>;
using NMOSReceiverLister = std::function<std::vector<NMOSReceiverResource>()>;

class NodeAPIRouter {
public:
    static constexpr const char* kApiVersion = "v1.3";

    /// `node.id` must be set: every other id is derived from it.
    /// `controlHref` is the IS-05 root the device advertises; empty means
    /// the device lists no control.
    NodeAPIRouter(NMOSNodeInfo node, std::string controlHref,
                  NMOSSenderLister senders, NMOSReceiverLister receivers);

    /// Marks every resource as changed now. IS-04 orders updates by a
    /// version stamp; a controller re-reads what moved.
    void touch();

    /// The port is only known once the server has bound it, which is after
    /// this router has to exist for the server to call. Sets the node's
    /// api.endpoints and href, and the device's control.
    void setEndpoint(const std::string& apiHost, uint16_t apiPort, const std::string& controlHref);

    ConnectionAPIServer::Reply route(const std::string& method, const std::string& path) const;

    std::string deviceId() const;

private:
    void touchLocked();
    ConnectionAPIServer::Reply error(int status, const std::string& text) const;
    std::string nodeData() const;
    std::string deviceData() const;

    /// The serving thread routes while the device thread sets the endpoint
    /// once at start-up; one lock covers both.
    mutable std::mutex mutex_;
    NMOSNodeInfo node_;
    std::string controlHref_;
    NMOSSenderLister senders_;
    NMOSReceiverLister receivers_;
    int64_t versionSeconds_{0};
    int32_t versionNanos_{0};
};

}  // namespace AES67
