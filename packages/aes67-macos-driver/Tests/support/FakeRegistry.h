//
// FakeRegistry.h
// AES67 macOS Driver tests
//
// An NMOS registry that is a socket and a script rather than a plant.
//
// Two suites need it: the one that checks what the driver PUTs to a registry,
// and the one that checks how often and to which registry it keeps saying it.
// It was written inside the first of them, where the second could not reach
// it.
//
#pragma once

#include "support/LoopbackSocket.h"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace AES67::Testing {

/// A registry that answers whatever the test tells it to and keeps every
/// request it was sent.
class FakeRegistry {
public:
    explicit FakeRegistry(std::string answer) : answer_(std::move(answer)) {}
    /// Answers the requests in order, and `answer` once the list runs out.
    /// A node that meets a 200 has to delete itself and register again, so
    /// checking that needs two different replies to two identical POSTs.
    FakeRegistry(std::vector<std::string> inOrder, std::string answer)
        : answer_(std::move(answer)), scripted_(std::move(inOrder)) {}
    ~FakeRegistry() { stop(); }

    bool start() {
        if (!listener_.open()) return false;

        running_.store(true);
        thread_ = std::thread([this] {
            while (running_.load()) {
                const int client = ::accept(listener_.fd(), nullptr, nullptr);
                if (client < 0) return;
                std::string request;
                char chunk[2048];
                // One read is enough: the client sends head and body in a
                // single write and then waits.
                const ssize_t n = ::recv(client, chunk, sizeof(chunk), 0);
                if (n > 0) request.append(chunk, static_cast<size_t>(n));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    requests_.push_back(request);
                    arrivals_.push_back(std::chrono::steady_clock::now());
                }
                std::string reply = answer_;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (served_ < scripted_.size()) reply = scripted_[served_];
                    ++served_;
                }
                ::send(client, reply.data(), reply.size(), 0);
                ::close(client);
            }
        });
        return true;
    }

    void stop() {
        running_.store(false);
        listener_.close();
        if (thread_.joinable()) thread_.join();
    }

    uint16_t port() const { return listener_.port(); }

    std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    /// When each of those arrived. A registry that is only asked what it was
    /// sent cannot say anything about how often, and how often is the whole
    /// question for a heartbeat.
    std::vector<std::chrono::steady_clock::time_point> arrivals() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return arrivals_;
    }

private:
    std::string answer_;
    std::vector<std::string> scripted_;
    size_t served_{0};
    TestSupport::LoopbackListener listener_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
    std::vector<std::chrono::steady_clock::time_point> arrivals_;
};

/// One canned HTTP response.
std::string answer(const char* status, const std::string& body = {});


}  // namespace AES67::Testing
