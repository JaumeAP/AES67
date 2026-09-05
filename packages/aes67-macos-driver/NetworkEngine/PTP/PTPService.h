//
// PTPService.h
// AES67 macOS Driver
//
// Server and client for the PTP daemon's status socket. The server runs
// inside the daemon, which owns the PTP sockets for the host; the client runs
// wherever the measurement is needed -- the AudioServerPlugIn inside
// coreaudiod, the Manager app, or a test.
//
// Both halves are plain Unix-domain sockets with no dependency on the PTP
// engine, so they are testable in one process.
//

#ifndef PTP_SERVICE_H
#define PTP_SERVICE_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Shared/PTPServiceProtocol.h"

namespace AES67 {

// Publishes a PTPServiceStatus to every connected reader.
class PTPServiceServer {
public:
    explicit PTPServiceServer(std::string socketPath = kPTPServiceSocketPath);
    ~PTPServiceServer();

    PTPServiceServer(const PTPServiceServer&) = delete;
    PTPServiceServer& operator=(const PTPServiceServer&) = delete;

    // Creates the socket, replacing a stale one left by a previous run.
    bool start();
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // Send to everyone connected. `sequence` and `publishedAtMs` are filled
    // in here, so a caller only sets the measurement fields.
    void publish(PTPServiceStatus status);

    int clientCount() const;
    const std::string& socketPath() const { return socketPath_; }

    // Hard ceiling on connected readers. The socket is chmod 0666 -- it has
    // to be, the reader is coreaudiod and not root -- so anyone on the
    // machine can connect, and nothing here ever drops a reader that simply
    // stays quiet: a client is only removed when a write to it fails. An
    // uncapped list is therefore a local descriptor exhaustion away from
    // silencing the daemon. Oldest connection goes first, the same backstop
    // RTCPReceiverTable and PTPPeerTable already apply to their own
    // attacker-fed tables.
    static constexpr size_t kMaxClients{32};

private:
    void acceptLoop();

    std::string socketPath_;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    mutable std::mutex clientsMutex_;
    std::vector<int> clients_;
    std::atomic<uint32_t> sequence_{0};
};

// Reads the daemon's status, reconnecting on its own. Every accessor answers
// from the last status that arrived, and reports nothing at all once that
// status is older than kPTPServiceStaleMs: a daemon that died must not leave
// a stale offset behind looking like a lock.
class PTPServiceClient {
public:
    explicit PTPServiceClient(std::string socketPath = kPTPServiceSocketPath);
    ~PTPServiceClient();

    PTPServiceClient(const PTPServiceClient&) = delete;
    PTPServiceClient& operator=(const PTPServiceClient&) = delete;

    // Starts the reader thread. Returns false only if it was already
    // started; a daemon that is not there yet is not an error, the client
    // keeps trying.
    bool start();
    void stop();

    // True when a status arrived recently enough to be believed.
    bool hasFreshStatus() const;
    bool isLocked() const;
    int64_t getOffsetNs() const;
    int64_t getPathDelayNs() const;
    double getFrequencyDriftPpb() const;
    uint8_t getClockClass() const;
    std::string getGrandmasterID() const;

    // Copy of the last status, whether fresh or not, with `valid` false when
    // nothing has ever arrived.
    bool lastStatus(PTPServiceStatus* out) const;

    int getConnectCount() const {
        return connectCount_.load(std::memory_order_relaxed);
    }
    int getRejectedCount() const {
        return rejectedCount_.load(std::memory_order_relaxed);
    }

private:
    void readLoop();
    bool connectOnce();

    std::string socketPath_;
    std::atomic<bool> running_{false};
    std::thread readThread_;
    int fd_ = -1;

    mutable std::mutex statusMutex_;
    PTPServiceStatus status_{};
    bool haveStatus_ = false;
    std::chrono::steady_clock::time_point receivedAt_{};

    std::atomic<int> connectCount_{0};
    std::atomic<int> rejectedCount_{0};
};

}  // namespace AES67

#endif  // PTP_SERVICE_H
