#ifndef SAP_LISTENER_H
#define SAP_LISTENER_H

#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

namespace AES67 {

struct SAPAnnouncement {
    std::string sessionDescription;  // The SDP content
    std::string sourceAddress;       // IP address of the announcer
    std::string sessionName;         // Name of the session
    std::string multicastAddress;    // Multicast address for the stream
    int port;                        // Port for the stream
    int ptpDomain;                   // PTP domain

    /// When this session was last announced. A SAP announcer repeats
    /// itself indefinitely, so an entry that stops being refreshed means
    /// the sender is gone — see SAPListener::kSessionTimeout.
    std::chrono::steady_clock::time_point lastSeen{};

    /// True for a SAP deletion packet (RFC 2974 type bit set) — the
    /// announcer saying this session is finished. Never appears in
    /// getDiscoveredStreams(); the listener acts on it by removing the
    /// matching session instead of waiting out kSessionTimeout.
    bool isDeletion{false};

    /// SAP session identity, straight from the packet header and present in
    /// BOTH announcements and deletions (RFC 2974 §6): the 16-bit Message
    /// ID Hash (bytes 2-3) and the 32-bit originating source (bytes 4-7).
    /// This is the stable key the listener matches on, so a deletion — or a
    /// session with no SDP `s=` name — is still identified exactly, rather
    /// than by the optional, editable session name. 0/0 means the sender
    /// didn't supply a hash, in which case the listener falls back to
    /// (sessionName, sourceAddress).
    uint16_t msgIdHash{0};
    uint32_t originatingSource{0};

    SAPAnnouncement() : port(0), ptpDomain(0) {}
};

using SAPAnnouncementCallback = std::function<void(const SAPAnnouncement&)>;

class SAPListener {
public:
    /// How long a session may go un-announced before it's dropped from
    /// getDiscoveredStreams().
    ///
    /// RFC 2974 announcers repeat on an interval derived from the
    /// announcement bandwidth, with 30 s the usual result in practice, and
    /// the AES67 Linux daemon (see
    /// Docs/comparison_ravenna_aes67_linux_driver.md) drops a remote
    /// source after ten missed announcements. 300 s is that same rule at
    /// the usual interval: tolerant of a few lost packets, but a list that
    /// still reflects what is actually on the network. A listener can't
    /// know the announcer's own interval, so this is a fixed timeout
    /// rather than a computed one.
    static constexpr std::chrono::seconds kSessionTimeout{300};

    SAPListener();
    ~SAPListener();
    
    // Initialize the SAP listener
    bool initialize();
    
    // Start listening for SAP announcements
    bool start();
    
    // Stop listening
    void stop();
    
    // Register a callback to receive SAP announcements
    void registerAnnouncementCallback(const SAPAnnouncementCallback& callback);
    
    // Get the list of recently discovered streams
    std::vector<SAPAnnouncement> getDiscoveredStreams() const;

    /// Parse one SAP datagram (RFC 2974) into an announcement.
    ///
    /// Static and public so that the parsing can be exercised without a
    /// socket: everything this reads arrives unauthenticated from a
    /// multicast group anyone can send to, which makes it the part most
    /// worth pinning, and it had no test of its own until 2026-09-04.
    /// `sourceAddress` is the datagram's sender, which the parser only
    /// records — it does not come out of the packet's own bytes.
    ///
    /// An announcement that could not be read comes back with an empty
    /// sessionDescription, never as an exception: this runs inside
    /// coreaudiod.
    static SAPAnnouncement parseAnnouncement(const char* data, size_t length,
                                             const std::string& sourceAddress);

    
private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace AES67

#endif // SAP_LISTENER_H