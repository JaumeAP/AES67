#include "Ravenna/RegistrationClient.h"

#include "Ravenna/HTTPClient.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace AES67::Ravenna {
namespace {

/// How long each turn round the loop spends waiting on the browser's socket.
/// Short, because it is also how quickly this reacts to a registry going
/// away, and the wait is where the thread sits when nothing is happening.
constexpr int kTickMs = 200;

/// How often the question is asked again. A responder announces a service
/// unprompted when it appears, so this is only for the ones that were already
/// up, and a packet every few seconds is what a browser costs a link.
constexpr int kAskAgainSeconds = 5;

/// How long to wait after every known registry has refused. A link with no
/// registry on it is the ordinary case for this device, and retrying as fast
/// as the machine can is a connection a millisecond nobody wants.
constexpr int kRetrySeconds = 5;

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

bool RegistrationClient::heartbeat(const NmosRegistry& registry, bool* unknown) {
    HTTPClient client(registry.host, registry.port);
    // The health resource takes a POST with no body at all, so it carries no
    // Content-Type either: a header describing a body that is not there is
    // what the AMWA suite warns about.
    const HTTPResponse response =
        client.perform("POST", registry.registrationPath() + "/health/nodes/" + nodeId_);
    if (unknown != nullptr) *unknown = response.error.empty() && response.status == 404;
    return response.error.empty() && response.status == 200;
}

bool RegistrationClient::takeUp(const NmosRegistry& registry) {
    bool unknown = false;
    if (heartbeat(registry, &unknown)) return true;
    // Anything other than "I have never heard of you" is a registry that is
    // not usable at all, and the next one down the list is the answer.
    if (!unknown) return false;

    if (!registerEverything(registry)) return false;
    // Registered, and told so at once: a registry counts a node as present
    // from its first heartbeat, and a plant failing over moves faster than
    // the interval.
    (void)heartbeat(registry);
    return true;
}

void RegistrationClient::run() {
    std::string error;
    if (!browser_.start(error)) {
        (void)std::fprintf(stderr, "[nmos] no registry discovery: %s\n", error.c_str());
        return;
    }

    using Clock = std::chrono::steady_clock;
    // The registries already tried and refused, by endpoint. A node walks down
    // the priority list rather than hammering the one at the top, and the set
    // is cleared once the whole list has been through: a registry that was
    // down when this asked is up again a minute later.
    std::vector<std::string> refused;
    NmosRegistry active;
    bool registered = false;
    auto nextAsk = Clock::now();
    auto nextAttempt = Clock::now();
    auto nextHeartbeat = Clock::now();

    const auto note = [this](const std::string& endpoint) {
        const std::lock_guard<std::mutex> held(registeredLock_);
        registeredWith_ = endpoint;
    };

    while (running_.load()) {
        // The socket first, so an announcement that arrives while this is
        // waiting for its next attempt is already in the list when it comes.
        browser_.service(kTickMs);

        const auto now = Clock::now();
        if (now >= nextAsk) {
            browser_.ask();
            nextAsk = now + std::chrono::seconds(kAskAgainSeconds);
        }

        if (registered) {
            if (now < nextHeartbeat) continue;
            if (heartbeat(active)) {
                nextHeartbeat = now + std::chrono::seconds(kHeartbeatSeconds);
                continue;
            }
            (void)std::fprintf(stderr, "[nmos] %s stopped answering; looking for another\n",
                               active.endpoint().c_str());
            registered = false;
            refused.push_back(active.endpoint());
            note({});
            continue;
        }

        if (now < nextAttempt) continue;

        const std::vector<NmosRegistry> registries = browser_.registries();
        const auto next = std::find_if(
            registries.begin(), registries.end(), [&refused](const NmosRegistry& registry) {
                return std::find(refused.begin(), refused.end(), registry.endpoint()) ==
                       refused.end();
            });
        if (next == registries.end()) {
            // Nothing left to try, either because nothing has answered yet or
            // because every one of them refused. Wait before starting the list
            // again, so a link whose registries are all down is not asked
            // about as fast as the machine can ask.
            refused.clear();
            nextAttempt = now + std::chrono::seconds(kRetrySeconds);
            continue;
        }

        if (takeUp(*next)) {
            active = *next;
            registered = true;
            nextHeartbeat = now + std::chrono::seconds(kHeartbeatSeconds);
            note(active.endpoint());
            (void)std::fprintf(stderr, "[nmos] registered with %s\n", active.endpoint().c_str());
        } else {
            refused.push_back(next->endpoint());
        }
    }

    note({});
    browser_.stop();
}

}  // namespace AES67::Ravenna
