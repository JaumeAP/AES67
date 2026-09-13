#include "Ravenna/RegistrationClient.h"

#include "NetworkEngine/Discovery/HTTPClient.h"

#include <chrono>
#include <cstdio>

namespace AES67::Ravenna {
namespace {

/// How long a browse waits for registries to answer. RFC 6762 asks a
/// responder to answer within 120 ms; a second is generous and is paid once,
/// when the daemon starts or when the registry it was using went away.
constexpr int kBrowseMs = 1000;

/// How long to wait before browsing again when nothing answered. A link with
/// no registry on it is the ordinary case for this device, and asking every
/// second for ever would be a packet a second nobody wants.
constexpr int kBrowseAgainSeconds = 10;

std::string resourceBody(const std::string& type, const JsonValue& data) {
    JsonObject envelope;
    envelope["type"] = JsonValue(type);
    envelope["data"] = data;
    return JsonValue(envelope).serialise();
}

}  // namespace

RegistrationClient::~RegistrationClient() { stop(); }

void RegistrationClient::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void RegistrationClient::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

std::string RegistrationClient::registeredWith() const {
    const std::lock_guard<std::mutex> held(registeredLock_);
    return registeredWith_;
}

bool RegistrationClient::registerEverything(const NmosRegistry& registry) {
    HTTPClient client(registry.host, registry.port);
    const std::string resource = registry.registrationPath() + "/resource";

    bool first = true;
    for (const auto& [type, data] : node_.resourcesInRegistrationOrder()) {
        HTTPResponse response =
            client.post(resource, resourceBody(type, data), "application/json");

        // IS-04 sec 4.1: 201 is a new registration and 200 means the registry
        // already had this id. A node that takes the 200 and carries on is
        // left with whatever the registry remembered from before it
        // restarted, so its own registration is deleted and made again.
        if (first && response.error.empty() && response.status == 200) {
            (void)client.del(resource + "/nodes/" + nodeId_);
            response = client.post(resource, resourceBody(type, data), "application/json");
        }
        first = false;

        if (!response.error.empty() || (response.status != 200 && response.status != 201)) {
            (void)std::fprintf(stderr, "[nmos] %s refused by %s:%u (%d %s)\n", type.c_str(),
                         registry.host.c_str(), static_cast<unsigned>(registry.port),
                         response.status, response.error.c_str());
            return false;
        }
    }
    return true;
}

bool RegistrationClient::heartbeat(const NmosRegistry& registry) {
    HTTPClient client(registry.host, registry.port);
    // The health resource takes a POST with no body, and the registry answers
    // 200 while it still has this node.
    const HTTPResponse response =
        client.post(registry.registrationPath() + "/health/nodes/" + nodeId_, "",
                    "application/json");
    return response.error.empty() && response.status == 200;
}

void RegistrationClient::run() {
    std::vector<NmosRegistry> registries;
    size_t current = 0;
    bool registered = false;

    const auto forget = [this]() {
        const std::lock_guard<std::mutex> held(registeredLock_);
        registeredWith_.clear();
    };

    while (running_.load()) {
        if (!registered) {
            if (current >= registries.size()) {
                registries = browseForRegistries(interfaceName_, addressV4_, kBrowseMs);
                current = 0;
            }
            if (registries.empty()) {
                // Nothing on the link. Peer-to-peer discovery still works, so
                // this is a quiet wait rather than a failure.
                for (int slept = 0; slept < kBrowseAgainSeconds && running_.load(); ++slept) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                continue;
            }

            const NmosRegistry& registry = registries[current];
            if (registerEverything(registry)) {
                registered = true;
                const std::lock_guard<std::mutex> held(registeredLock_);
                registeredWith_ = registry.host + ":" + std::to_string(registry.port);
                (void)std::fprintf(stderr, "[nmos] registered with %s\n", registeredWith_.c_str());
            } else {
                // The next one down the priority list, and a fresh browse once
                // the list is used up: that is what a second advertised
                // registry is for.
                ++current;
            }
            continue;
        }

        for (int slept = 0; slept < kHeartbeatSeconds && running_.load(); ++slept) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!running_.load()) break;

        if (!heartbeat(registries[current])) {
            (void)std::fprintf(stderr, "[nmos] %s stopped answering; looking for another registry\n",
                         registries[current].host.c_str());
            registered = false;
            forget();
            // The one that failed is not tried again until the next browse.
            ++current;
        }
    }

    forget();
}

}  // namespace AES67::Ravenna
