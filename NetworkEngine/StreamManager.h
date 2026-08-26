/// @file StreamManager.h
/// @brief Central coordinator for all AES67 RX and TX streams.

#pragma once

#include "Shared/Types.h"
#include "Shared/RingBuffer.hpp"
#include "Driver/SDPParser.h"
#include "NetworkEngine/CompatibilityProfile.h"
#include "NetworkEngine/StreamChannelMapper.h"
#include "NetworkEngine/StreamConfig.h"
#include "RTP/RTPReceiver.h"
#include "RTP/RTPTransmitter.h"
#include "PTP/PTPClock.h"
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>

namespace AES67 {

/// Central coordinator for all AES67 streams (receivers, transmitters, channel mapping).
///
/// @warning NOT REAL-TIME SAFE. All public methods acquire a mutex. Never call from
/// the Core Audio IO thread or any RT-constrained thread. Safe to call from
/// initialization, UI/manager app, or control threads.
class StreamManager {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;
    using StreamCallback = std::function<void(const StreamInfo&)>;

    /// @param inputChannels  Ring buffers written by RTP receivers (RX path).
    /// @param outputChannels Ring buffers read by RTP transmitters (TX path).
    StreamManager(DeviceChannelBuffers& inputChannels, DeviceChannelBuffers& outputChannels);
    ~StreamManager();

    // Prevent copy/move
    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    /// Full PTP diagnostic snapshot for a domain (default 0 — every stream
    /// this driver serves defaults to domain 0 unless its SDP says
    /// otherwise). What AES67Device's custom property
    /// (Shared/CustomProperties.h) serves to ManagerApp.
    PTPDiagnostics getPTPDiagnostics(int domain = 0);

    /// Caps how many device channels RX streams may be assigned. Feeds
    /// StreamChannelMapper::setUsableChannelCount() (restricts the index
    /// range addStream()'s auto-assignment searches) AND the aggregate RX
    /// total canAddStream() checks (covers explicit-mapping RX too, which
    /// bypasses auto-assignment). AES67Device passes the user's persisted
    /// DeviceChannelSettings.rx here at startup.
    void setUsableChannelCount(size_t count) {
        mapper_.setUsableChannelCount(count);
        usableRxChannelCount_.store(static_cast<uint32_t>(count), std::memory_order_relaxed);
    }

    /// Caps how many device channels TX streams may be assigned, in total
    /// across every createTxStream()/createTxStreamFlows() call while this
    /// setting is active. TX streams always specify their own device
    /// channels explicitly (no auto-assignment to restrict the index range
    /// of), so this is purely the aggregate check — same mechanism as
    /// CompatibilityProfile::maxTotalChannels, just user-driven instead of
    /// profile-driven. AES67Device passes DeviceChannelSettings.tx here.
    void setUsableTxChannelCount(size_t count) {
        usableTxChannelCount_.store(static_cast<uint32_t>(count), std::memory_order_relaxed);
    }

    /// How many 8-channel flows this driver's TX flows should be shifted
    /// by on the wire — the amplifier-unit selection, expressed the only
    /// way it actually shows up in the protocol. Under the Dolby Atmos
    /// Connect scheme (CompatibilityProfile::useFixedMulticastWith
    /// PerFlowSourcePort) consecutive units take consecutive source-port
    /// blocks: unit 1 sends from 6518+, unit 2 (32 channels later) from
    /// 6522+, and so on. Feeding "unit 2" therefore means starting the
    /// source-port walk 4 flows in, which is exactly what this offset
    /// does. AES67Device computes it from the persisted
    /// AmplifierUnitSettings and the selected TX channel count.
    ///
    /// Ignored entirely under any profile that doesn't use per-flow
    /// source ports — those distinguish flows by multicast address, where
    /// a port offset would mean nothing.
    void setTxFlowPortOffset(uint32_t flows) {
        txFlowPortOffset_.store(flows, std::memory_order_relaxed);
    }

    /// Safety cushion, in samples, that a receiver accumulates before it
    /// starts handing audio to Core Audio. Higher survives worse network
    /// jitter; every sample of it is latency.
    ///
    /// 0 (default) keeps this receiver's own long-standing cushion of 6
    /// packets. The AES67 Linux daemon calls this playout_delay and sets
    /// it per sink; Dolby's DMA manual calls it Safety Buffer and
    /// documents 0-50 samples for exactly the same symptom, brief dropouts
    /// from network trouble. Two independent sources describing the same
    /// control is why it's expressed in samples here rather than packets,
    /// even though packets is what the receiver counts.
    ///
    /// Applied when a stream is created, so changing it affects streams
    /// added afterwards.
    void setPlayoutDelaySamples(uint32_t samples) {
        playoutDelaySamples_.store(samples, std::memory_order_relaxed);
    }
    uint32_t getPlayoutDelaySamples() const {
        return playoutDelaySamples_.load(std::memory_order_relaxed);
    }

    /// Whether this driver runs a PTP clock at all.
    ///
    /// When enabled, adding a stream starts (or joins) a PTPClock for that
    /// stream's PTP domain, so the clock disciplines against the network's
    /// grandmaster and getPTPDiagnostics() reports something real instead
    /// of PTPDiagnostics{}'s disconnected defaults. Before this existed,
    /// nothing in the driver path ever called
    /// PTPClockManager::getClockForDomain(), so the whole PTP subsystem —
    /// slave, master, BMCA, arbitrator — was compiled and never run.
    ///
    /// Off by default, deliberately. Starting PTP opens multicast sockets
    /// and threads on a path that has been verified against real hardware
    /// without them; this is the one change here that can't be checked by
    /// building and running the tests. Turn it on knowingly.
    void setPTPEnabled(bool enabled);
    bool isPTPEnabled() const { return ptpEnabled_.load(std::memory_order_relaxed); }

    /// Whether to refuse audio until the PTP clock has locked, the way the
    /// AES67 Linux daemon does. Off by default even when PTP is enabled:
    /// on a system that currently carries audio without any PTP at all,
    /// switching this on can only ever take audio away. Meaningless unless
    /// setPTPEnabled(true).
    void setRequirePTPLock(bool require) {
        requirePTPLock_.store(require, std::memory_order_relaxed);
    }
    bool getRequirePTPLock() const { return requirePTPLock_.load(std::memory_order_relaxed); }

    /// Whether to allow streams locked to different PTP grandmasters to
    /// coexist. Two streams disciplined by different grandmasters drift
    /// against each other — slowly, and audibly long before anything looks
    /// broken — so by default the second one is refused with the mismatch
    /// named. The AES67 Linux daemon exposes the same escape hatch per
    /// sink as `ignore_refclk_gmid`; this is driver-wide.
    ///
    /// Only streams that actually declare a grandmaster (SDP a=ts-refclk)
    /// are compared. A stream that declares none is never refused on this
    /// ground — silence isn't disagreement.
    void setIgnoreRefClockMismatch(bool ignore) {
        ignoreRefClockMismatch_.store(ignore, std::memory_order_relaxed);
    }
    bool getIgnoreRefClockMismatch() const {
        return ignoreRefClockMismatch_.load(std::memory_order_relaxed);
    }

    /// The PTP grandmaster every current stream is locked to, or empty if
    /// no stream has declared one. What canAddStream() compares against.
    std::string getActiveGrandmaster() const;

    /// Which flavour of AoIP gear this driver is being pointed at. Every
    /// stream added from here on is validated against the profile's limits
    /// (canAddStream). Defaults to AES67, which imposes only AES67's own
    /// mandatory configuration. See CompatibilityProfile.h.
    void setCompatibilityProfile(CompatibilityProfileKind kind);
    CompatibilityProfileKind getCompatibilityProfileKind() const;

    //
    // Stream Management - RX
    //

    /// Add an RX stream with automatic channel assignment. Returns the new StreamID.
    StreamID addStream(const SDPSession& sdp);

    /// Add an RX stream with explicit channel mapping.
    StreamID addStream(const SDPSession& sdp, const ChannelMapping& mapping);

    /// Import an RX stream from an SDP file on disk.
    StreamID importSDPFile(const std::string& filepath);

    // Remove stream
    bool removeStream(const StreamID& id);

    // Remove all streams
    void removeAllStreams();

    //
    // Stream Management - TX
    //

    /// Create a TX stream that reads from device output channels and sends RTP.
    /// sourcePort binds the transmitter's local UDP port explicitly instead
    /// of leaving it kernel-assigned — 0 (default) for every profile but
    /// DMA. See CompatibilityProfile::useFixedMulticastWithPerFlowSourcePort.
    StreamID createTxStream(
        const std::string& name,
        const std::string& multicastIP,
        uint16_t port,
        uint16_t numChannels,
        const ChannelMapping& mapping,
        uint16_t sourcePort = 0,
        int dscp = -1
    );

    /// Creates a TX stream of any width as one or more AES67 flows, each
    /// carrying at most StreamChannelMapper::kMaxChannelsPerFlow (8)
    /// channels — the actual limit in the standard, and what Dante
    /// Controller does when you tick more than 8 channels.
    ///
    /// Addressing depends on the active profile's
    /// CompatibilityProfile::useFixedMulticastWithPerFlowSourcePort:
    ///  - false (default — AES67/Dante convention): flow N gets
    ///    `baseMulticastIP` with its last octet advanced by N (so
    ///    239.69.1.10 with 24 channels becomes .10, .11, .12), all flows
    ///    share `port` as both destination port and (kernel-assigned)
    ///    source port.
    ///  - true (the Dolby DMA profile's real wire scheme): every flow
    ///    shares `baseMulticastIP` and `port` as a fixed RTP destination
    ///    port; flow N instead binds source port `port + 1 + N` (so
    ///    port 6517 with 24 channels becomes source ports 6518, 6519,
    ///    6520 — matches the DMA's own documented defaults exactly when
    ///    the caller passes 6517).
    /// Either way, each flow gets the next 8 device channels starting from
    /// `mapping.deviceChannelStart`, and each is a normal stream created
    /// through createTxStream(), so nothing downstream needs to know these
    /// were grouped.
    ///
    /// Returns every created StreamID, or an empty vector on failure —
    /// partial flows are rolled back rather than left half-configured. A
    /// base IP whose last octet would exceed 255, or a source port that
    /// would exceed 65535, fails outright.
    std::vector<StreamID> createTxStreamFlows(
        const std::string& baseName,
        const std::string& baseMulticastIP,
        uint16_t port,
        uint16_t numChannels,
        const ChannelMapping& mapping,
        int dscp = -1
    );

    // Export stream to SDP file
    bool exportSDPFile(const StreamID& id, const std::string& filepath);

    //
    // Channel Mapping
    //

    // Update channel mapping for a stream
    bool updateMapping(const StreamID& id, const ChannelMapping& newMapping);

    // Get mapping for a stream
    std::optional<ChannelMapping> getMapping(const StreamID& id) const;

    // Get all mappings
    std::vector<ChannelMapping> getAllMappings() const;

    //
    // Query
    //

    // Get all active streams
    std::vector<StreamInfo> getActiveStreams() const;

    // Full SDP of every TX stream - the sources this driver offers, for SAP
    // announcement so other AES67/Dante gear can discover and subscribe.
    std::vector<SDPSession> getTransmitSessions() const;

    // Auto sink-follow (RAVENNA auto_sinks_update): given a freshly parsed SAP
    // announcement, re-point any RECEIVE stream bound to that source onto the
    // source's new transport (multicast address, port, sample rate, encoding,
    // ptime, payload type) when it has changed, preserving the sink's device-
    // channel mapping. Streams are matched by session name plus, when both
    // sides know it, the unicast source address. A change in channel count is
    // deliberately NOT followed (it would force a device-channel re-map that
    // could collide with neighbouring streams) — skipped and logged instead.
    // Returns how many receive streams were re-subscribed. No-op unless
    // setAutoSinkFollow(true) (the default).
    size_t updateReceiveStreamsFromAnnouncement(const SDPSession& announced);

    // Outcome of matching a SAP announcement against one stored receive
    // stream — the pure decision behind auto sink-follow, exposed static so
    // it can be unit-tested without a live StreamManager (no sockets):
    //   NotBound            - announcement isn't this sink's source
    //   Unchanged           - same source, identical transport: nothing to do
    //   ChannelCountChanged - source moved but changed channel count; not
    //                         auto-followed (would force a device re-map)
    //   Follow              - source moved; re-subscribe to the new transport
    enum class SinkFollowDecision { NotBound, Unchanged, ChannelCountChanged, Follow };
    static SinkFollowDecision evaluateSinkFollow(const SDPSession& stored,
                                                 const SDPSession& announced) {
        // Defined inline so it can be unit-tested without linking the rest of
        // StreamManager (receivers, sockets, PTP).
        if (announced.sessionName.empty() ||
            stored.sessionName != announced.sessionName) {
            return SinkFollowDecision::NotBound;
        }
        if (!stored.sourceAddress.empty() && !announced.sourceAddress.empty() &&
            stored.sourceAddress != announced.sourceAddress) {
            return SinkFollowDecision::NotBound;
        }
        const bool transportChanged =
            stored.connectionAddress != announced.connectionAddress ||
            stored.port != announced.port ||
            stored.payloadType != announced.payloadType ||
            stored.encoding != announced.encoding ||
            stored.sampleRate != announced.sampleRate ||
            stored.ptimeUs != announced.ptimeUs ||
            stored.framecount != announced.framecount;
        if (!transportChanged) return SinkFollowDecision::Unchanged;
        if (stored.numChannels != announced.numChannels) {
            return SinkFollowDecision::ChannelCountChanged;
        }
        return SinkFollowDecision::Follow;
    }

    // Effective DSCP for a transmit stream: the stream's own override when
    // set (>= 0), else the active profile's recommendedDscp. Inline + static
    // so it is unit-testable without a live StreamManager. -1 out means leave
    // the socket unmarked.
    static int resolveEffectiveDscp(int perSourceDscp, int profileDscp) {
        return perSourceDscp >= 0 ? perSourceDscp : profileDscp;
    }

    // Enable/disable auto sink-follow. On by default; a source that never
    // changes its SDP is unaffected either way.
    void setAutoSinkFollow(bool enabled);

    // Get stream info
    std::optional<StreamInfo> getStreamInfo(const StreamID& id) const;

    // Check if stream exists
    bool hasStream(const StreamID& id) const;

    // Get stream count
    size_t getStreamCount() const;

    //
    // Validation
    //

    // Check if stream can be added. isTransmit distinguishes an RX stream
    // (addStream) from a TX one (createTxStream) — the active profile's
    // direction/maxTotalChannels are checked per-direction. Defaults to
    // false (RX) for callers that predate this distinction.
    bool canAddStream(const SDPSession& sdp, bool isTransmit = false, std::string* errorOut = nullptr) const;

    // Get detailed error message for why stream can't be added
    std::string getAddStreamError(const SDPSession& sdp, bool isTransmit = false) const;

    //
    // Device State
    //

    /// Notify that Core Audio IO has started or stopped.
    /// When active, starts all dormant receivers/transmitters; when inactive, stops them.
    void setIOActive(bool active);

    // Set current device sample rate (validates against streams)
    bool setDeviceSampleRate(double sampleRate);

    // Get current device sample rate
    double getDeviceSampleRate() const { return currentDeviceSampleRate_; }

    // Get available channel count
    size_t getAvailableChannelCount() const;

    //
    // Configuration Persistence
    //

    /// Load saved stream configurations from /tmp/AES67Driver/streams.json.
    bool loadSavedStreams();

    /// Persist all current streams to disk.
    bool saveAllStreams();

    // Enable/disable auto-save (automatically save after add/remove/update)
    void setAutoSave(bool enabled) { autoSaveEnabled_ = enabled; }

    // Get auto-save state
    bool isAutoSaveEnabled() const { return autoSaveEnabled_; }

    //
    // Callbacks
    //

    // Register callback for stream added
    void setStreamAddedCallback(StreamCallback callback) {
        streamAddedCallback_ = callback;
    }

    // Register callback for stream removed
    void setStreamRemovedCallback(StreamCallback callback) {
        streamRemovedCallback_ = callback;
    }

    // Register callback for stream status changed
    void setStreamStatusCallback(StreamCallback callback) {
        streamStatusCallback_ = callback;
    }

private:
    // Internal stream state
    struct ManagedStream {
        SDPSession sdp;
        ChannelMapping mapping;
        std::unique_ptr<RTPReceiver> receiver;
        std::unique_ptr<RTPTransmitter> transmitter;
        StreamInfo info;
        bool isTransmit{false};
    };

    // Validation helpers
    bool validateSampleRate(const SDPSession& sdp, std::string* errorOut) const;
    bool validateChannelAvailability(uint16_t numChannels, std::string* errorOut) const;
    bool validateNetworkConfig(const SDPSession& sdp, std::string* errorOut) const;

    // Stream creation helpers
    std::unique_ptr<RTPReceiver> createReceiver(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        size_t jitterBufferDepth = 0,
        const std::string& networkInterface = ""
    );  // playout delay comes from the driver-wide setting, read inside

    std::unique_ptr<RTPTransmitter> createTransmitter(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        const std::string& networkInterface = "",
        uint16_t sourcePort = 0
    );  // dscp comes from the active profile, read inside

    // Callback invocation
    void notifyStreamAdded(const StreamInfo& info);
    void notifyStreamRemoved(const StreamInfo& info);
    void notifyStreamStatusChanged(const StreamInfo& info);

    /// Starts (or joins) a PTP clock for this stream's domain when PTP is
    /// enabled. No-op when it isn't, or for a stream that declares no PTP.
    void ensurePTPClockForDomain(int domain);

    /// Records the grandmaster the first stream declares; later streams
    /// are checked against it by canAddStream(). A stream declaring none
    /// leaves it untouched.
    void adoptGrandmaster(const std::string& grandmaster);

    /// Clears it once no streams remain. Called with streamsMutex_ held.
    void recomputeActiveGrandmaster();

    // Configuration helpers
    void autoSaveIfEnabled();
    bool saveAllStreamsInternal();  // Internal version without locking

    // Data members
    DeviceChannelBuffers& inputChannels_;   // RTP receivers write here (Network → Core Audio)
    DeviceChannelBuffers& outputChannels_;  // RTP transmitters read here (Core Audio → Network)
    StreamChannelMapper mapper_;

    // Active compatibility profile. Atomic, not mutex-guarded: canAddStream()
    // reads it both with streamsMutex_ held (via addStream) and without it
    // (via getAddStreamError), so it can't take the lock itself.
    std::atomic<CompatibilityProfileKind> profileKind_{CompatibilityProfileKind::AES67};

    // Running totals for CompatibilityProfile::maxTotalChannels — atomic
    // for the same reason profileKind_ is: canAddStream() must be callable
    // both with streamsMutex_ held (addStream/createTxStream) and without
    // it (getAddStreamError), so it can't take the lock to sum streams_
    // itself. Kept in step by addStream/createTxStream on success and by
    // removeStream/removeAllStreams on removal — see those for the actual
    // increment/decrement.
    std::atomic<uint32_t> rxChannelsInUse_{0};
    std::atomic<uint32_t> txChannelsInUse_{0};

    // User-configured per-direction channel caps (DeviceChannelSettings.rx/tx,
    // see setUsableChannelCount/setUsableTxChannelCount above). Same atomic
    // reasoning as profileKind_: checked from canAddStream() without the lock.
    // Default 128 = no restriction (every channel usable), matching the
    // driver's behavior before this setting existed.
    std::atomic<uint32_t> usableRxChannelCount_{128};
    std::atomic<uint32_t> usableTxChannelCount_{128};

    // Amplifier-unit selection as a flow-port offset — see
    // setTxFlowPortOffset(). 0 = first (or only) unit, the behavior before
    // the selector existed.
    std::atomic<uint32_t> txFlowPortOffset_{0};

    // Auto sink-follow toggle (see updateReceiveStreamsFromAnnouncement). On
    // by default. Atomic: read on the SAP listener's callback thread, set
    // from control paths.
    std::atomic<bool> autoSinkFollowEnabled_{true};

    // Grandmaster of the streams currently open, and whether to police it.
    // Its own mutex rather than streamsMutex_, for the same reason
    // profileKind_ is atomic: canAddStream() is called both with
    // streamsMutex_ held (addStream/createTxStream) and without it
    // (getAddStreamError), so it can't take that lock itself.
    std::atomic<uint32_t> playoutDelaySamples_{0};
    std::atomic<bool> ptpEnabled_{false};
    std::atomic<bool> requirePTPLock_{false};

    mutable std::mutex refClockMutex_;
    std::string activeGrandmaster_;
    std::atomic<bool> ignoreRefClockMismatch_{false};
    std::map<StreamID, ManagedStream> streams_;
    mutable std::mutex streamsMutex_;

    // Configuration management
    std::unique_ptr<StreamConfigManager> configManager_;
    bool autoSaveEnabled_{true};

    // IO lifecycle state — true when Core Audio IO is active (StartIO/StopIO)
    std::atomic<bool> ioActive_{false};

    // Device state
    std::atomic<double> currentDeviceSampleRate_{48000.0};

    // PTP clock manager reference
    std::shared_ptr<PTPClockManager> ptpManager_;

    // Callbacks
    StreamCallback streamAddedCallback_;
    StreamCallback streamRemovedCallback_;
    StreamCallback streamStatusCallback_;
};

} // namespace AES67
