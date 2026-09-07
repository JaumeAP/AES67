//
// AES67Device.cpp
// AES67 macOS Driver - Build #9
// Core Audio device implementation
//

#include "AES67Device.h"
#include "AES67IOHandler.h"
#include "Driver/SDPParser.h"
#include "Driver/DebugLog.h"
#include "Shared/CustomProperties.h"
#include "NetworkEngine/DeviceChannelSettings.h"
#include "NetworkEngine/AmplifierUnitSettings.h"
#include "NetworkEngine/NMOSSettings.h"
#include "NetworkEngine/PTP/PTPMasterSettings.h"
#include "NetworkEngine/NetworkInterfaceDetection.h"
#include <CoreAudio/AudioServerPlugIn.h>
#include <algorithm>
#include <utility>

namespace AES67 {

namespace {

/// This machine's name, for the registry to show. An empty answer is fine:
/// the label carries the useful half and IS-04 allows it.
std::string localHostname() {
    char name[256] = {0};
    if (::gethostname(name, sizeof(name) - 1) != 0) return {};
    return std::string(name);
}

/// "host:port", the form the settings file takes for a registry that mDNS
/// cannot reach. Anything else is refused rather than half-parsed.
std::optional<NMOSRegistry> parseRegistryOverride(const std::string& text) {
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return std::nullopt;
    }
    const std::string portText = text.substr(colon + 1);
    errno = 0;
    char* end = nullptr;
    const unsigned long port = std::strtoul(portText.c_str(), &end, 10);
    if (end == portText.c_str() || *end != '\0' || errno == ERANGE) return std::nullopt;
    if (port == 0 || port > 65535) return std::nullopt;

    NMOSRegistry registry;
    registry.host = text.substr(0, colon);
    registry.port = static_cast<uint16_t>(port);
    return registry;
}

} // namespace


// Helper to create initialized ring buffer array
namespace {
    template<size_t... Is>
    auto MakeRingBufferArray(size_t bufferSize, std::index_sequence<Is...>) {
        return std::array<SPSCRingBuffer<float>, sizeof...(Is)>{
            ((void)Is, SPSCRingBuffer<float>(bufferSize))...
        };
    }

    auto MakeRingBufferArray(size_t bufferSize) {
        return MakeRingBufferArray(bufferSize, std::make_index_sequence<AES67Device::kNumChannels>{});
    }
}

AES67Device::AES67Device(const std::shared_ptr<aspl::Context>& context)
    : aspl::Device(context, aspl::DeviceParameters{
        .Name = "AES67 Device",
        .Manufacturer = "AES67 Driver",
        .DeviceUID = "com.aes67.driver.device",
        .ModelUID = "com.aes67.driver.model",
        .CanBeDefault = true,
        .CanBeDefaultForSystemSounds = false
    })
    // Initialize ring buffers sized for maximum supported sample rate (384kHz)
    // This ensures buffers are always large enough regardless of sample rate changes
    // Power-of-2 sizing: 384kHz @ 3ms = 1152 samples → 2048 (next power of 2)
    , inputBuffers_(MakeRingBufferArray(
          CalculateRingBufferSize(384000.0)))  // Max sample rate
    , outputBuffers_(MakeRingBufferArray(
          CalculateRingBufferSize(384000.0)))  // Max sample rate
{
    AES67_LOG("AES67Device constructor: Starting initialization");

    // How many channels are USABLE — not how many are advertised. The
    // device always presents all kNumChannels (128) to Core Audio, both
    // here and in InitializeStreams(); this setting caps how many of them
    // StreamChannelMapper will hand out to streams. Declaring all and using
    // the selected subset avoids reconfiguring the device's stream format,
    // which would only take effect across a Core Audio restart anyway.
    {
        DeviceChannelSettingsManager channelSettingsManager;
        const DeviceChannelSettings channelSettings = channelSettingsManager.load();
        usableRxChannelCount_ = channelSettings.rx.totalChannelCount();
        usableTxChannelCount_ = channelSettings.tx.totalChannelCount();
        AES67_LOGF("AES67Device: RX channel setting = %u + %s = %u usable (%zu always advertised)",
                   channelSettings.rx.channelCount,
                   channelSettings.rx.auxChannelEnabled ? "8 (aux group)" : "0 (no aux)",
                   usableRxChannelCount_,
                   kNumChannels);
        AES67_LOGF("AES67Device: TX channel setting = %u + %s = %u usable (%zu always advertised)",
                   channelSettings.tx.channelCount,
                   channelSettings.tx.auxChannelEnabled ? "8 (aux group)" : "0 (no aux)",
                   usableTxChannelCount_,
                   kNumChannels);
    }

    const Float64 initialSampleRate = currentSampleRate_.load();
    const size_t ringBufferSize = CalculateRingBufferSize(384000.0);
    AES67_LOGF("AES67Device: Initial sample rate = %.0f Hz", initialSampleRate);
    AES67_LOGF("AES67Device: Ring buffer size = %zu samples (sized for max 384kHz)",
               ringBufferSize);
    AES67_LOGF("AES67Device: Buffer latency @ %.0f Hz = %.2f ms",
               initialSampleRate,
               (ringBufferSize * 1000.0) / initialSampleRate);

    // NOTE: Cannot call InitializeStreams() here because shared_from_this()
    // won't work until the shared_ptr is fully constructed
    // InitializeStreams() will be called from Initialize() method

    // The gateway: safe to register now even though streamManager_ doesn't
    // exist until Initialize() runs — the getter lambda below is only
    // invoked later, when a client actually queries the property, and it
    // guards against a null streamManager_ itself.
    RegisterCustomProperty(kPTPDiagnosticsPropertySelector,
        [this]() { return GetPTPDiagnosticsProperty(); });

    // Same for SAP discovery. Registered here for the same reason: the
    // getter is only invoked once a client actually queries, by which time
    // sapListener_ exists (and guards against null anyway).
    RegisterCustomProperty(kDiscoveredSessionsPropertySelector,
        [this]() { return GetDiscoveredSessionsProperty(); });

    // Passive PTP peer observer gateway - lists the Dolby elements present
    // on the network by PTP role. Getter guards against a null observer.
    RegisterCustomProperty(kPtpPeersPropertySelector,
        [this]() { return GetPtpPeersProperty(); });

    // RTCP receiver-report gateway - the second amplifier-detection vector.
    RegisterCustomProperty(kRtcpReceiversPropertySelector,
        [this]() { return GetRtcpReceiversProperty(); });

    AES67_LOG("AES67Device constructor: Basic initialization complete");
}

void AES67Device::Initialize() {
    AES67_LOG("AES67Device::Initialize() called");

    // Initialize streams
    AES67_LOG("AES67Device: Calling InitializeStreams()");
    InitializeStreams();

    // Create RT-safe interface (compile-time boundary for IO handler)
    // Must be created before InitializeIOHandler() so it can be passed in.
    AES67_LOG("AES67Device: Creating RTSafeStreamInterface");
    rtInterface_ = std::make_unique<RTSafeStreamInterface>(
        inputBuffers_,
        outputBuffers_,
        inputUnderruns_,
        outputUnderruns_,
        ioRunning_
    );
    AES67_LOG("AES67Device: RTSafeStreamInterface created successfully");

    // Initialize IO handler (uses RT-safe interface)
    AES67_LOG("AES67Device: Calling InitializeIOHandler()");
    InitializeIOHandler();

    // Initialize Stream Manager (manages all AES67 network streams)
    AES67_LOG("AES67Device: Creating StreamManager");
    streamManager_ = std::make_unique<StreamManager>(inputBuffers_, outputBuffers_);
    AES67_LOG("AES67Device: StreamManager created successfully");

    // Apply the channel-count setting read in the constructor. Must happen
    // before loadSavedStreams() below, so restored streams are validated
    // against the same ceiling new ones will be.
    streamManager_->setUsableChannelCount(usableRxChannelCount_);
    streamManager_->setUsableTxChannelCount(usableTxChannelCount_);
    AES67_LOGF("AES67Device: StreamManager usable channels set to RX=%u TX=%u",
               usableRxChannelCount_, usableTxChannelCount_);

    // Compatibility profile, likewise before loadSavedStreams(): restored
    // streams face the same profile limits new ones will. Defaults to AES67
    // (no extra restrictions) when nothing has been selected.
    {
        CompatibilityProfileManager profileManager;
        const CompatibilityProfileKind profileKind = profileManager.load();
        streamManager_->setCompatibilityProfile(profileKind);

        // Which unit in a chained Dolby Atmos Connect installation this
        // driver is feeding, translated into the flow-port offset that's
        // the only way the choice shows up on the wire — see
        // StreamManager::setTxFlowPortOffset() and
        // NetworkEngine/AmplifierUnitSettings.h. Clamped to the active
        // profile's own maxUnits so a selection left over from a
        // different profile can't shift ports under one that only ever
        // has a single unit.
        AmplifierUnitSettingsManager unitSettingsManager;
        const AmplifierUnitSettings unitSettings = unitSettingsManager.load();
        const auto profile = CompatibilityProfile::forKind(profileKind);
        const uint32_t unitIndex = std::min(unitSettings.unitIndex, profile.maxUnits);

        // The preceding units' source-port flows — the real sum of their
        // sizes, since a chain may mix 16-, 24- and 32-channel units (each
        // 2/3/4 flows). Unknown positions fall back to this driver's own
        // output width, so an unset chain behaves as the old uniform case.
        const uint32_t flowOffset = AmplifierUnitSettings::flowOffsetForUnit(
            unitSettings.chainUnitChannels, usableTxChannelCount_, unitIndex);
        streamManager_->setTxFlowPortOffset(flowOffset);

        // Playout delay lives in the same settings file.
        const PlayoutDelaySettings delay = unitSettingsManager.loadPlayoutDelay();
        streamManager_->setPlayoutDelaySamples(delay.samples);
        if (delay.samples > 0) {
            AES67_LOGF("AES67Device: Playout delay %u samples", delay.samples);
        }
        AES67_LOGF("AES67Device: Amplifier unit %u of max %u -> TX flow port offset %u",
                   unitIndex, profile.maxUnits, flowOffset);
    }

    // SAP discovery. Listens for other devices' session announcements so
    // ManagerApp can offer them and receive streams can auto-follow a moved
    // source (below); our own sources are announced separately by
    // SAPAnnouncer. Failing to start is not fatal — discovery is a convenience,
    // and a driver that carries audio without it is far better than one
    // that refuses to load because a multicast join failed.
    sapListener_ = std::make_unique<SAPListener>();
    if (sapListener_) {
        // Auto sink-follow (RAVENNA auto_sinks_update): when a discovered
        // source re-announces with changed transport, re-point any receive
        // stream bound to it. Parsing happens here, off the audio path; the
        // match/re-subscribe is StreamManager's job.
        sapListener_->registerAnnouncementCallback(
            [this](const SAPAnnouncement& a) {
                if (a.isDeletion || a.sessionDescription.empty()) return;
                if (!streamManager_) return;
                auto parsed = SDPParser::parseString(a.sessionDescription);
                if (!parsed) return;
                // The announcement has to come from the host it claims to
                // describe. Without this, any machine on the network can
                // re-point a live receiver at its own multicast group by
                // announcing a session that borrows the name and origin of a
                // real one (2026-09-04 audit). It does not survive a spoofed
                // source IP, but it removes the case that needs nothing but a
                // socket.
                if (parsed->originAddress.empty() ||
                    parsed->originAddress != a.sourceAddress) {
                    AES67_LOGF("AES67Device: SAP announcement from %s claims origin '%s' "
                               "— ignored for sink-follow",
                               a.sourceAddress.c_str(), parsed->originAddress.c_str());
                    return;
                }
                streamManager_->updateReceiveStreamsFromAnnouncement(*parsed);
            });
    }
    if (sapListener_->initialize() && sapListener_->start()) {
        AES67_LOG("AES67Device: SAP discovery listening on 224.2.127.254:9875");
    } else {
        AES67_LOG("AES67Device: SAP discovery unavailable — continuing without it");
        sapListener_.reset();
    }

    // mDNS/DNS-SD browsing, alongside SAP rather than instead of it: the
    // two see different halves of a real network. A sender that registers
    // `_rtsp._tcp` but never announces over SAP was invisible to this
    // driver until now (2026-08-31) — that is how Merging's RAVENNA
    // driver publishes sessions, and it is what an AES67 device on a
    // switch with SAP filtered still offers. Same posture as SAP: a
    // failure to start costs discovery, never audio.
    mdnsBrowser_ = std::make_unique<MDNSBrowser>(MDNSBrowser::kServiceTypeRTSP);
    if (mdnsBrowser_->start()) {
        AES67_LOG("AES67Device: mDNS discovery browsing _rtsp._tcp");
    } else {
        AES67_LOG("AES67Device: mDNS discovery unavailable — continuing without it");
        mdnsBrowser_.reset();
    }

    // SAP announcement of our OWN transmit streams, so remote AES67/Dante
    // receivers can discover and subscribe to what this driver sends.
    // Listening (above) lets us find others; announcing lets others find us.
    // Like discovery, a failure here is not fatal - audio still flows, only
    // auto-discovery of our sources is lost.
    //
    // Announced from the address of the interface the audio leaves by: the
    // SAP header's originating source, and the SDP's o= address, which is
    // half of the identity a receiver keys the flow on (Dante Controller's
    // RtpFlowIdentity is that address plus the session id). Both went out
    // empty -- 0.0.0.0 in the header, "o=- <id> 1 IN IP4 " in the body --
    // because createTxStream() sets no origin and the announcer was
    // initialised without an interface (DanteInteropSim, 2026-09-07).
    const std::string sapInterface = NetworkInterfaceDetection::detectPTPInterface();
    const std::string sapAddress = sapInterface.empty()
        ? std::string{}
        : NetworkInterfaceDetection::getInterfaceIPAddress(sapInterface);
    sapAnnouncer_ = std::make_unique<SAPAnnouncer>();
    if (sapAnnouncer_->initialize(sapAddress) &&
        sapAnnouncer_->start([this, sapAddress]() {
            std::vector<std::string> sdps;
            if (!streamManager_) return sdps;
            for (SDPSession session : streamManager_->getTransmitSessions()) {
                if (session.originAddress.empty()) session.originAddress = sapAddress;
                std::string sdp = SDPParser::generate(session);
                if (!sdp.empty()) sdps.push_back(std::move(sdp));
            }
            return sdps;
        })) {
        AES67_LOG("AES67Device: SAP announcing transmit streams on :9875");
    } else {
        AES67_LOG("AES67Device: SAP announcement unavailable - continuing without it");
        sapAnnouncer_.reset();
    }

    // RTSP DESCRIBE endpoint for our own transmit streams. SAP is a
    // broadcast a receiver has to be listening for at the right moment;
    // DESCRIBE is a question it can ask whenever it likes, and it is how
    // RAVENNA-class gear expects to fetch a session description
    // (2026-08-31). Same non-fatal posture as every other discovery
    // surface here.
    //
    // Port 554 is IANA's and needs root, which a user-space driver does
    // not have: kUnprivilegedRTSPPort is what we actually bind, and it is
    // what the mDNS registration below advertises, so a client that
    // discovers us reaches the right port without assuming the default.
    rtspServer_ = std::make_unique<RTSPServer>(kUnprivilegedRTSPPort);
    const bool rtspStarted = rtspServer_->start([this]() {
        std::vector<RTSPPublishedStream> published;
        if (!streamManager_) return published;
        for (const auto& session : streamManager_->getTransmitSessions()) {
            std::string sdp = SDPParser::generate(session);
            if (sdp.empty()) continue;
            // One path per session name, plus "/" for the first stream so
            // a client that asks for the root gets something useful
            // rather than a 404.
            RTSPPublishedStream entry;
            // The path is compared against what a client sends, and the
            // server percent-decodes that before comparing -- so publish
            // the decoded form. A session name with a space ("Studio Mic
            // 1") reaches us as "%20" and matches here.
            entry.path = "/by-name/" + session.sessionName;
            entry.sdp = sdp;
            published.push_back(entry);
            if (published.size() == 1) {
                RTSPPublishedStream root;
                root.path = "/";
                root.sdp = std::move(sdp);
                published.push_back(std::move(root));
            }
        }
        return published;
    });
    if (rtspStarted) {
        AES67_LOGF("AES67Device: RTSP DESCRIBE serving on :%u", rtspServer_->boundPort());
    } else {
        AES67_LOG("AES67Device: RTSP server unavailable - continuing without it");
        rtspServer_.reset();
    }

    // NMOS. Off unless the installation asked for it: registering puts
    // this machine in whatever reads the plant's registry, which is a
    // decision somebody makes rather than something a driver starts doing
    // on its own. A registry that cannot be found, or that refuses, costs
    // the audio path nothing.
    {
        NMOSSettingsManager nmosSettingsManager;
        NMOSSettings nmosSettings = nmosSettingsManager.load();
        if (nmosSettings.enabled) {
            // First run under `enabled` has no id yet: generate and persist
            // one before announcing, so the registry sees the same node
            // after a restart.
            if (nmosSettings.nodeId.empty()) {
                nmosSettingsManager.save(nmosSettings);
            }

            NMOSNodeInfo node;
            node.id = nmosSettings.nodeId;
            nmosNodeId_ = nmosSettings.nodeId;
            node.hostname = localHostname();
            node.label = nmosSettings.label.empty()
                             ? ("AES67 macOS Driver on " + node.hostname)
                             : nmosSettings.label;

            nmosClient_ = std::make_unique<NMOSRegistrationClient>(node);

            std::optional<NMOSRegistry> registry;
            if (!nmosSettings.registryOverride.empty()) {
                registry = parseRegistryOverride(nmosSettings.registryOverride);
                if (!registry.has_value()) {
                    AES67_LOGF("AES67Device: NMOS registry override '%s' is not host:port",
                               nmosSettings.registryOverride.c_str());
                }
            } else {
                registry = NMOSRegistrationClient::discoverRegistry();
            }

            if (registry.has_value() && nmosClient_->registerWith(*registry)) {
                nmosClient_->startHeartbeats();
                AES67_LOGF("AES67Device: registered with the NMOS registry at %s:%u as %s",
                           registry->host.c_str(), registry->port, node.id.c_str());

                // The streams are described from a thread of its own, and
                // the callbacks below only ask for it: they run with
                // StreamManager's mutex held and describing the streams
                // needs that same mutex.
                {
                    std::lock_guard<std::mutex> lock(nmosSyncMutex_);
                    nmosSyncRunning_ = true;
                }
                nmosSyncThread_ = std::thread([this] {
                    for (;;) {
                        {
                            std::unique_lock<std::mutex> lock(nmosSyncMutex_);
                            nmosSyncSignal_.wait(lock, [this] {
                                return nmosSyncRequested_ || !nmosSyncRunning_;
                            });
                            if (!nmosSyncRunning_) return;
                            nmosSyncRequested_ = false;
                        }
                        syncNMOSResources();
                    }
                });

                // IS-05. Bound to an ephemeral port: this is a user-space
                // driver and the port it gets is what it advertises.
                connectionServer_ = std::make_unique<ConnectionAPIServer>(0);
                const bool connectionStarted = connectionServer_->start(
                    [this] { return connectionSenders(); },
                    [this] { return connectionReceivers(); },
                    [this](const std::string& id, const ConnectionPatch& patch) {
                        return applyConnectionPatch(id, patch);
                    });
                if (connectionStarted) {
                    AES67_LOGF("AES67Device: IS-05 connection API on :%u",
                               connectionServer_->boundPort());
                } else {
                    AES67_LOG("AES67Device: IS-05 connection API unavailable - "
                              "registering without a control");
                    connectionServer_.reset();
                }

                streamManager_->setStreamAddedCallback(
                    [this](const StreamInfo&) { requestNMOSSync(); });
                streamManager_->setStreamRemovedCallback(
                    [this](const StreamInfo&) { requestNMOSSync(); });
                requestNMOSSync();
            } else {
                AES67_LOG("AES67Device: no NMOS registry registered with - continuing without it");
                nmosClient_.reset();
            }
        }
    }

    // Passive PTP peer observer: watches PTP traffic to list which Dolby
    // elements are on the network (see PTPPeerObserver / GetPtpPeersProperty).
    // Independent of our own PTP role and, like SAP discovery, non-fatal if it
    // can't start. Uses the default interface (empty) for now.
    ptpPeerObserver_ = std::make_unique<PTPPeerObserver>();
    if (ptpPeerObserver_->start("")) {
        AES67_LOG("AES67Device: PTP peer observer watching 319/320");
    } else {
        AES67_LOG("AES67Device: PTP peer observer unavailable - continuing without it");
        ptpPeerObserver_.reset();
    }

    // RTCP receiver monitor: listens on the RTCP port (RTP destination + 1) of
    // each transmit stream to count the receivers of what this driver sends —
    // the second amplifier-detection vector, for gear that emits RTCP. Passive
    // and non-fatal, like the others.
    rtcpMonitor_ = std::make_unique<RTCPMonitor>();
    if (rtcpMonitor_->start([this]() {
            std::vector<RTCPMonitor::Endpoint> eps;
            if (!streamManager_) return eps;
            for (const auto& session : streamManager_->getTransmitSessions()) {
                if (session.connectionAddress.empty()) continue;
                RTCPMonitor::Endpoint e;
                e.multicastIp = session.connectionAddress;
                e.rtcpPort = static_cast<uint16_t>(session.port + 1); // RTP dest + 1
                eps.push_back(e);
            }
            return eps;
        })) {
        AES67_LOG("AES67Device: RTCP receiver monitor started");
    } else {
        AES67_LOG("AES67Device: RTCP receiver monitor unavailable - continuing without it");
        rtcpMonitor_.reset();
    }

    // PTP. Off unless the installation has asked for it — see
    // StreamManager::setPTPEnabled() for why that default is what it is.
    {
        PTPMasterSettingsManager ptpSettingsManager;
        const PTPMasterSettings ptpSettings = ptpSettingsManager.load();
        streamManager_->setRequirePTPLock(ptpSettings.requireLock);
        streamManager_->setPTPEnabled(ptpSettings.ptpEnabled);
        AES67_LOGF("AES67Device: PTP %s (require lock: %s)",
                   ptpSettings.ptpEnabled ? "enabled" : "disabled",
                   ptpSettings.requireLock ? "yes" : "no");
    }

    // Set device sample rate in StreamManager
    streamManager_->setDeviceSampleRate(currentSampleRate_.load());
    AES67_LOGF("AES67Device: StreamManager sample rate set to %.0f Hz", currentSampleRate_.load());

    // Load saved stream configurations from disk
    AES67_LOG("AES67Device: Attempting to load saved stream configurations");
    bool loadedSavedStreams = streamManager_->loadSavedStreams();

    // If no saved streams were loaded, create test streams for initial testing
    if (!loadedSavedStreams) {
        // Create test RX stream (Network → Core Audio) on channels 0-7
        AES67_LOG("AES67Device: No saved streams found, adding test RX stream (239.1.1.1:5004, 8ch @ 48kHz)");
        SDPSession testSDP;
        testSDP.sessionName = "Test AES67 Stream";
        testSDP.sessionInfo = "Hard-coded test stream for driver development";
        testSDP.connectionAddress = "239.1.1.1";  // AES67 multicast range
        testSDP.port = 5004;
        testSDP.numChannels = 8;
        testSDP.sampleRate = 48000.0;
        testSDP.encoding = "L24";
        testSDP.payloadType = 97;
        testSDP.ptimeUs = 1000;  // 1 ms packets (48 samples @ 48kHz)
        testSDP.framecount = 48;
        testSDP.ptpDomain = 0;
        testSDP.sessionID = 123456;
        testSDP.sessionVersion = 1;
        testSDP.sourceAddress = "0.0.0.0";

        StreamID testStreamID = streamManager_->addStream(testSDP);
        if (!testStreamID.isNull()) {
            AES67_LOG("AES67Device: Test RX stream added successfully");
            AES67_LOGF("AES67Device: Test RX stream ID: %s", testStreamID.toString().c_str());
        } else {
            AES67_LOG("AES67Device: WARNING - Failed to add test RX stream");
        }

        // Create test TX stream (Core Audio → Network) on channels 8-15
        // Uses a different multicast group (239.1.1.2) to avoid confusion with RX
        AES67_LOG("AES67Device: Adding test TX stream (239.1.1.2:5004, 8ch @ 48kHz, channels 8-15)");
        ChannelMapping txMapping;
        txMapping.streamChannelCount = 8;
        txMapping.deviceChannelStart = 8;   // Channels 8-15 (non-overlapping with RX 0-7)
        txMapping.deviceChannelCount = 8;

        StreamID txStreamID = streamManager_->createTxStream(
            "Test AES67 TX Stream",
            "239.1.1.2",   // Different multicast group from RX (239.1.1.1)
            5004,
            8,
            txMapping
        );
        if (!txStreamID.isNull()) {
            AES67_LOG("AES67Device: Test TX stream added successfully");
            AES67_LOGF("AES67Device: Test TX stream ID: %s", txStreamID.toString().c_str());
        } else {
            AES67_LOG("AES67Device: WARNING - Failed to add test TX stream");
        }
    }

    AES67_LOG("AES67Device::Initialize() complete");
}

std::string AES67Device::nmosIdFor(const std::string& prefix, const std::string& name) const {
    if (nmosNodeId_.empty()) return {};
    return NMOSRegistrationClient::deriveId(nmosNodeId_, prefix + ":" + name);
}

std::vector<ConnectionSender> AES67Device::connectionSenders() {
    std::vector<ConnectionSender> senders;
    if (!streamManager_) return senders;

    for (const SDPSession& sdp : streamManager_->getTransmitSessions()) {
        ConnectionSender sender;
        sender.id = nmosIdFor("sender", sdp.sessionName);
        sender.label = sdp.sessionName;
        sender.multicastAddress = sdp.connectionAddress;
        sender.port = sdp.port;
        sender.sourceAddress = sdp.originAddress;
        // The same description the RTSP server hands out, so a controller
        // that fetches it here and a receiver that asks for it there get
        // the same stream.
        sender.sdp = SDPParser::generate(sdp);
        senders.push_back(std::move(sender));
    }
    return senders;
}

std::vector<ConnectionReceiver> AES67Device::connectionReceivers() {
    std::vector<ConnectionReceiver> receivers;
    if (!streamManager_) return receivers;

    for (const SDPSession& sdp : streamManager_->getReceiveSessions()) {
        ConnectionReceiver receiver;
        receiver.id = nmosIdFor("receiver", sdp.sessionName);
        receiver.label = sdp.sessionName;
        receiver.multicastAddress = sdp.connectionAddress;
        receiver.port = sdp.port;
        receivers.push_back(std::move(receiver));
    }
    return receivers;
}

bool AES67Device::applyConnectionPatch(const std::string& receiverId,
                                       const ConnectionPatch& patch) {
    if (!streamManager_) return false;

    // Which of our receive streams this id names. The ids are derived from
    // the session name, so this is the same walk the listing does.
    SDPSession target;
    bool found = false;
    for (const SDPSession& sdp : streamManager_->getReceiveSessions()) {
        if (nmosIdFor("receiver", sdp.sessionName) == receiverId) {
            target = sdp;
            found = true;
            break;
        }
    }
    if (!found) return false;

    // A patch with no activation is staged and not applied. This driver
    // keeps no staged state, so saying yes to it would be a promise it
    // cannot keep on the next GET.
    if (!patch.activateImmediate) return false;

    // master_enable false is a controller disconnecting the receiver.
    if (patch.masterEnable.has_value() && !*patch.masterEnable) {
        for (const StreamInfo& info : streamManager_->getActiveStreams()) {
            if (info.name == target.sessionName) {
                return streamManager_->removeStream(info.id);
            }
        }
        return true;   // already not running: the controller got what it asked for
    }

    // What the receiver should be taking now. A transport file carries the
    // whole description, which is the only way to be patched onto a stream
    // this driver has never heard announced; without one, the transport
    // params move the address and the format stays as it was.
    SDPSession wanted = target;
    if (patch.transportFile.has_value()) {
        const auto parsed = SDPParser::parseString(*patch.transportFile);
        if (!parsed) return false;
        wanted = *parsed;
        // The sink keeps its own identity: what changed is where it
        // listens, not which receiver it is. The origin address is part of
        // that identity — sink-follow matches on it, and a controller's
        // transport file legitimately carries the sender's own "o=".
        wanted.sessionName = target.sessionName;
        wanted.originAddress = target.originAddress;
    }
    if (patch.multicastAddress.has_value() && !patch.multicastAddress->empty()) {
        wanted.connectionAddress = *patch.multicastAddress;
    }
    if (patch.port.has_value()) {
        wanted.port = *patch.port;
    }

    // Re-pointing goes through the same path SAP's sink-follow uses, which
    // preserves the device-channel mapping and refuses a channel-count
    // change for the reason recorded there.
    return streamManager_->updateReceiveStreamsFromAnnouncement(wanted) > 0;
}

void AES67Device::requestNMOSSync() {
    {
        std::lock_guard<std::mutex> lock(nmosSyncMutex_);
        if (!nmosSyncRunning_) return;
        nmosSyncRequested_ = true;
    }
    nmosSyncSignal_.notify_one();
}

void AES67Device::syncNMOSResources() {
    if (!nmosClient_ || !streamManager_) return;

    std::vector<NMOSSenderResource> senders;
    for (const SDPSession& sdp : streamManager_->getTransmitSessions()) {
        NMOSSenderResource sender;
        sender.name = sdp.sessionName;
        sender.description = sdp.sessionInfo;
        sender.multicastAddress = sdp.connectionAddress;
        sender.port = sdp.port;
        sender.sourceAddress = sdp.originAddress;
        sender.sampleRate = static_cast<uint32_t>(sdp.sampleRate);
        sender.channels = sdp.numChannels;
        sender.encoding = sdp.encoding.empty() ? "L24" : sdp.encoding;
        senders.push_back(std::move(sender));
    }

    std::vector<NMOSReceiverResource> receivers;
    for (const SDPSession& sdp : streamManager_->getReceiveSessions()) {
        NMOSReceiverResource receiver;
        receiver.name = sdp.sessionName;
        receiver.description = sdp.sessionInfo;
        receiver.subscribedMulticastAddress = sdp.connectionAddress;
        // A receive stream that exists is a receiver that is taking
        // something: this driver does not keep idle receivers around.
        receiver.active = true;
        receivers.push_back(std::move(receiver));
    }

    // The control href names the port the connection server actually got.
    // Empty when it could not start, which leaves the device advertising
    // no control rather than one nobody can reach.
    std::string controlHref;
    if (connectionServer_) controlHref = connectionServer_->controlHref(localHostname());

    if (!nmosClient_->syncResources(senders, receivers, controlHref)) {
        AES67_LOG("AES67Device: the NMOS registry did not take every resource");
    }
}

AES67Device::~AES67Device() {
    // Stop announcing first: its sender thread calls back into streamManager_,
    // which must still be alive (it is - declared before the announcer, so
    // destroyed after it - but stop promptly regardless).
    if (sapAnnouncer_) sapAnnouncer_->stop();
    if (ptpPeerObserver_) ptpPeerObserver_->stop();
    if (rtcpMonitor_) rtcpMonitor_->stop();
    // Same reason as the observers above: the browser's thread can call
    // back, so it stops before anything it might reach is torn down.
    if (mdnsBrowser_) mdnsBrowser_->stop();
    if (rtspServer_) rtspServer_->stop();
    // Telling the registry beats leaving it to time us out: a controller
    // showing a node that is gone is worse than one showing nothing.
    {
        std::lock_guard<std::mutex> lock(nmosSyncMutex_);
        nmosSyncRunning_ = false;
    }
    nmosSyncSignal_.notify_all();
    if (nmosSyncThread_.joinable()) nmosSyncThread_.join();
    if (connectionServer_) connectionServer_->stop();
    if (nmosClient_) {
        nmosClient_->stop();
        nmosClient_->unregister();
    }
    // Deactivate streams directly rather than calling StopIO() (which requires
    // framework context). This is safe in the destructor.
    if (inputStream_) {
        inputStream_->SetIsActive(false);
    }
    if (outputStream_) {
        outputStream_->SetIsActive(false);
    }
    ioRunning_.store(false);
}

void AES67Device::InitializeStreams() {
    AES67_LOG("InitializeStreams: Creating input stream (Network → Core Audio)");
    // Create input stream (Network → Core Audio)
    aspl::StreamParameters inputParams;
    inputParams.Direction = aspl::Direction::Input;
    inputParams.StartingChannel = 1;
    inputParams.Format.mSampleRate = currentSampleRate_.load();
    inputParams.Format.mFormatID = kAudioFormatLinearPCM;
    inputParams.Format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    inputParams.Format.mBitsPerChannel = 32;
    inputParams.Format.mChannelsPerFrame = kNumChannels;
    inputParams.Format.mBytesPerFrame = kNumChannels * sizeof(float);
    inputParams.Format.mFramesPerPacket = 1;
    inputParams.Format.mBytesPerPacket = inputParams.Format.mBytesPerFrame;

    AES67_LOGF("InitializeStreams: Input stream - %u channels @ %.0f Hz",
               kNumChannels, currentSampleRate_.load());

    inputStream_ = std::make_shared<aspl::Stream>(
        GetContext(),
        std::static_pointer_cast<aspl::Device>(shared_from_this()),
        inputParams
    );
    AES67_LOG("InitializeStreams: Input stream created, adding to device");
    AddStreamAsync(inputStream_);
    AES67_LOG("InitializeStreams: Input stream added successfully");

    // Create output stream (Core Audio → Network)
    AES67_LOG("InitializeStreams: Creating output stream (Core Audio → Network)");
    aspl::StreamParameters outputParams;
    outputParams.Direction = aspl::Direction::Output;
    outputParams.StartingChannel = 1;
    outputParams.Format.mSampleRate = currentSampleRate_.load();
    outputParams.Format.mFormatID = kAudioFormatLinearPCM;
    outputParams.Format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    outputParams.Format.mBitsPerChannel = 32;
    outputParams.Format.mChannelsPerFrame = kNumChannels;
    outputParams.Format.mBytesPerFrame = kNumChannels * sizeof(float);
    outputParams.Format.mFramesPerPacket = 1;
    outputParams.Format.mBytesPerPacket = outputParams.Format.mBytesPerFrame;

    AES67_LOGF("InitializeStreams: Output stream - %u channels @ %.0f Hz",
               kNumChannels, currentSampleRate_.load());

    outputStream_ = std::make_shared<aspl::Stream>(
        GetContext(),
        std::static_pointer_cast<aspl::Device>(shared_from_this()),
        outputParams
    );
    AES67_LOG("InitializeStreams: Output stream created, adding to device");
    AddStreamAsync(outputStream_);
    AES67_LOG("InitializeStreams: Output stream added successfully");

    AES67_LOG("InitializeStreams: Complete");
}

void AES67Device::InitializeIOHandler() {
    AES67_LOG("InitializeIOHandler: Creating AES67IOHandler with RTSafeStreamInterface");
    ioHandler_ = std::make_shared<AES67IOHandler>(
        *rtInterface_,
        kNumChannels,           // Cache channel count for RT-safe access
        sizeof(Float32)         // Cache bytes per sample for RT-safe access
    );
    AES67_LOG("InitializeIOHandler: IOHandler created successfully");

    // Register IO handler with device
    AES67_LOG("InitializeIOHandler: Registering IOHandler with device");
    SetIOHandler(ioHandler_);
    AES67_LOG("InitializeIOHandler: Complete");
}

Float64 AES67Device::GetSampleRate() const {
    return currentSampleRate_.load();
}

OSStatus AES67Device::SetSampleRate(Float64 sampleRate) {
    // Validate sample rate
    bool isValid = false;
    for (auto validRate : kSupportedSampleRates) {
        if (std::abs(sampleRate - validRate) < 0.1) {
            isValid = true;
            break;
        }
    }

    if (!isValid) {
        return kAudioHardwareUnsupportedOperationError;
    }

    // Check if IO is running - sample rate cannot be changed during IO
    if (ioRunning_.load()) {
        AES67_LOG("SetSampleRate: ERROR - Cannot change sample rate while IO is running");
        return kAudioHardwareBadObjectError;
    }

    // Log the sample rate change
    AES67_LOGF("SetSampleRate: Changing from %.0f Hz to %.0f Hz",
               currentSampleRate_.load(), sampleRate);

    const size_t ringBufferSize = inputBuffers_[0].capacity();
    AES67_LOGF("SetSampleRate: Ring buffer size = %zu samples (%.2f ms @ %.0f Hz)",
               ringBufferSize,
               (ringBufferSize * 1000.0) / sampleRate,
               sampleRate);

    // Check if buffer size would need to change (for diagnostic purposes)
    const size_t idealBufferSize = CalculateRingBufferSize(sampleRate);
    if (idealBufferSize != ringBufferSize) {
        AES67_LOGF("SetSampleRate: NOTE - Ideal buffer size for %.0f Hz would be %zu samples",
                   sampleRate, idealBufferSize);
        AES67_LOG("SetSampleRate: Using fixed buffer sized for maximum sample rate (384kHz)");
    }

    // Update current sample rate
    currentSampleRate_.store(sampleRate);

    // Update StreamManager's sample rate
    if (streamManager_) {
        streamManager_->setDeviceSampleRate(sampleRate);
        AES67_LOG("SetSampleRate: StreamManager sample rate updated");
    }

    // Update stream formats
    if (inputStream_) {
        auto format = inputStream_->GetPhysicalFormat();
        format.mSampleRate = sampleRate;
        inputStream_->SetPhysicalFormatAsync(format);
    }
    if (outputStream_) {
        auto format = outputStream_->GetPhysicalFormat();
        format.mSampleRate = sampleRate;
        outputStream_->SetPhysicalFormatAsync(format);
    }

    AES67_LOG("SetSampleRate: Complete");

    return kAudioHardwareNoError;
}

std::vector<AudioValueRange> AES67Device::GetAvailableSampleRates() const {
    std::vector<AudioValueRange> ranges;
    ranges.reserve(kSupportedSampleRates.size());
for (auto rate : kSupportedSampleRates) {
        ranges.push_back({rate, rate});
    }
    return ranges;
}

UInt32 AES67Device::GetBufferSize() const {
    return currentBufferSize_.load();
}

OSStatus AES67Device::SetBufferSize(UInt32 bufferSize) {
    // Validate buffer size
    bool isValid = false;
    for (auto validSize : kSupportedBufferSizes) {
        if (bufferSize == validSize) {
            isValid = true;
            break;
        }
    }

    if (!isValid) {
        return kAudioHardwareUnsupportedOperationError;
    }

    // Update current buffer size
    currentBufferSize_.store(bufferSize);

    return kAudioHardwareNoError;
}

std::vector<UInt32> AES67Device::GetAvailableBufferSizes() const {
    return std::vector<UInt32>(kSupportedBufferSizes.begin(), kSupportedBufferSizes.end());
}

std::string AES67Device::GetDeviceName() const {
    return "AES67 Device";
}

std::string AES67Device::GetDeviceManufacturer() const {
    return "AES67 Driver";
}

std::string AES67Device::GetDeviceUID() const {
    return "com.aes67.driver.device";
}

OSStatus AES67Device::StartIOImpl(UInt32 clientID, UInt32 startCount) {
    // startCount == 0 means first client starting IO (device transitions to running)
    if (startCount == 0) {
        // Activate streams
        if (inputStream_) {
            inputStream_->SetIsActive(true);
        }
        if (outputStream_) {
            outputStream_->SetIsActive(true);
        }

        ioRunning_.store(true);

        // Start RTP network threads now that a client needs audio
        if (streamManager_) {
            streamManager_->setIOActive(true);
        }
    }

    return aspl::Device::StartIOImpl(clientID, startCount);
}

OSStatus AES67Device::StopIOImpl(UInt32 clientID, UInt32 startCount) {
    // startCount == 0 means last client stopped IO (device transitions to not running)
    if (startCount == 0) {
        // Deactivate streams
        if (inputStream_) {
            inputStream_->SetIsActive(false);
        }
        if (outputStream_) {
            outputStream_->SetIsActive(false);
        }

        // Stop RTP network threads — no client needs audio anymore
        if (streamManager_) {
            streamManager_->setIOActive(false);
        }

        ioRunning_.store(false);
    }

    return aspl::Device::StopIOImpl(clientID, startCount);
}

void AES67Device::ResetStatistics() {
    inputUnderruns_.store(0);
    outputUnderruns_.store(0);
}

namespace {

void SetCFString(CFMutableDictionaryRef dict, const char* key, const std::string& value) {
    CFStringRef keyRef = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFStringRef valueRef = CFStringCreateWithCString(kCFAllocatorDefault, value.c_str(), kCFStringEncodingUTF8);
    if (keyRef && valueRef) CFDictionarySetValue(dict, keyRef, valueRef);
    if (keyRef) CFRelease(keyRef);
    if (valueRef) CFRelease(valueRef);
}

void SetCFInt64(CFMutableDictionaryRef dict, const char* key, int64_t value) {
    CFStringRef keyRef = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFNumberRef valueRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
    if (keyRef && valueRef) CFDictionarySetValue(dict, keyRef, valueRef);
    if (keyRef) CFRelease(keyRef);
    if (valueRef) CFRelease(valueRef);
}

void SetCFBool(CFMutableDictionaryRef dict, const char* key, bool value) {
    CFStringRef keyRef = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (keyRef) CFDictionarySetValue(dict, keyRef, value ? kCFBooleanTrue : kCFBooleanFalse);
    if (keyRef) CFRelease(keyRef);
}

} // namespace

CFPropertyListRef AES67Device::GetPTPDiagnosticsProperty() const {
    // Queried off the real-time thread (custom properties are non-RT,
    // "Invoked by HAL on non-realtime thread" per aspl::Object) — safe to
    // go through StreamManager/PTPClockManager's own mutexes here.
    PTPDiagnostics diag;
    if (streamManager_) {
        diag = streamManager_->getPTPDiagnostics();
    }
    // else: streamManager_ not created yet (queried before Initialize()) —
    // diag stays at PTPDiagnostics{}'s honest defaults (disconnected).

    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!dict) return nullptr;

    SetCFBool(dict, kPTPDiagKeyIsConnected, diag.isConnected);
    SetCFBool(dict, kPTPDiagKeyIsLocked, diag.isLocked);
    SetCFString(dict, kPTPDiagKeyMasterClockID, diag.masterClockID);
    SetCFInt64(dict, kPTPDiagKeyClockClass, diag.clockClass);
    SetCFInt64(dict, kPTPDiagKeyClockAccuracy, diag.clockAccuracy);
    SetCFInt64(dict, kPTPDiagKeyOffsetNs, diag.offsetNs);
    SetCFInt64(dict, kPTPDiagKeyCurrentDomain, diag.currentDomain);
    SetCFString(dict, kPTPDiagKeyRole, diag.role == PTPDiagnostics::Role::Master ? "master" : "slave");
    SetCFBool(dict, kPTPDiagKeyEverWasMaster, diag.everWasMaster);
    SetCFBool(dict, kPTPDiagKeyHasCompetitor, diag.hasCompetitor);
    SetCFInt64(dict, kPTPDiagKeyCompetitorPriority1, diag.competitorPriority1);
    SetCFInt64(dict, kPTPDiagKeyCompetitorPriority2, diag.competitorPriority2);
    SetCFInt64(dict, kPTPDiagKeySyncMessagesReceived, diag.syncMessagesReceived);
    SetCFInt64(dict, kPTPDiagKeyAnnounceMessagesReceived, diag.announceMessagesReceived);

    return dict; // +1 from CFDictionaryCreateMutable — caller CFReleases, per RegisterCustomProperty's contract
}

CFPropertyListRef AES67Device::GetDiscoveredSessionsProperty() const {
    // Non-RT, like the diagnostics property — SAPListener takes its own
    // mutex, and getDiscoveredStreams() sweeps expired sessions as it
    // reads, so what comes back is what's actually still being announced.
    CFMutableArrayRef array = CFArrayCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!array) return nullptr;

    if (!sapListener_) {
        return array; // Discovery unavailable — an empty list, not a failure
    }

    for (const auto& session : sapListener_->getDiscoveredStreams()) {
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!dict) continue;

        SetCFString(dict, kSessionKeyName, session.sessionName);
        SetCFString(dict, kSessionKeySourceAddress, session.sourceAddress);
        SetCFString(dict, kSessionKeyMulticastAddress, session.multicastAddress);
        SetCFInt64(dict, kSessionKeyPort, session.port);
        SetCFInt64(dict, kSessionKeyPtpDomain, session.ptpDomain);
        // The full SDP, so ManagerApp can add the stream without having to
        // re-derive anything the announcer already told us.
        SetCFString(dict, kSessionKeySDP, session.sessionDescription);

        CFArrayAppendValue(array, dict);
        CFRelease(dict); // array retains it
    }

    return array; // +1 — caller CFReleases, per RegisterCustomProperty's contract
}

CFPropertyListRef AES67Device::GetPtpPeersProperty() const {
    // Non-RT: PTPPeerObserver::peers() takes its own lock and sweeps expired
    // peers as it reads, so the array reflects who is actually still on the
    // PTP network right now.
    CFMutableArrayRef array = CFArrayCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!array) return nullptr;

    if (!ptpPeerObserver_) {
        return array; // observer unavailable — an empty list, not a failure
    }

    for (const auto& peer : ptpPeerObserver_->peers()) {
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!dict) continue;

        // A function rather than a switch into a variable: every role is
        // named, and a value that was assigned before the switch and never
        // read was the analyser's way of saying so.
        const char* roleStr = [&]() -> const char* {
            switch (peer.role()) {
                case PTPPeerRole::Master: return "master";
                case PTPPeerRole::Slave:  return "slave";
                case PTPPeerRole::Mixed:  return "mixed";
                case PTPPeerRole::Unknown: break;
            }
            return "unknown";
        }();

        SetCFString(dict, kPeerKeyClockId, peer.clockIdString());
        SetCFString(dict, kPeerKeyOui, peer.ouiString());
        SetCFString(dict, kPeerKeyRole, roleStr);
        SetCFString(dict, kPeerKeySourceIp, peer.sourceIp);
        SetCFInt64(dict, kPeerKeyDomain, peer.domain);
        SetCFInt64(dict, kPeerKeyMessageCount, static_cast<int64_t>(peer.messageCount));

        CFArrayAppendValue(array, dict);
        CFRelease(dict); // array retains it
    }

    return array; // +1 — caller CFReleases, per RegisterCustomProperty's contract
}

CFPropertyListRef AES67Device::GetRtcpReceiversProperty() const {
    CFMutableArrayRef array = CFArrayCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!array) return nullptr;
    if (!rtcpMonitor_) {
        return array; // monitor unavailable — empty list, not a failure
    }
    for (const auto& r : rtcpMonitor_->reporters()) {
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!dict) continue;
        SetCFInt64(dict, kRcvrKeySSRC, static_cast<int64_t>(r.ssrc));
        SetCFString(dict, kRcvrKeySourceIp, r.sourceIp);
        SetCFString(dict, kRcvrKeyCname, r.cname);
        SetCFInt64(dict, kRcvrKeyPacketCount, static_cast<int64_t>(r.packetCount));
        CFArrayAppendValue(array, dict);
        CFRelease(dict);
    }
    return array;
}

OSStatus AES67Device::OnSetSampleRate(Float64 sampleRate) {
    return SetSampleRate(sampleRate);
}

OSStatus AES67Device::OnSetBufferSize(UInt32 bufferSize) {
    return SetBufferSize(bufferSize);
}

size_t AES67Device::CalculateRingBufferSize(Float64 sampleRate, double latencyMs) {
    // Calculate ring buffer size for desired latency
    // Formula: samples = (sampleRate × latencyMs) / 1000
    //
    // Examples (with 3ms latency):
    //   48kHz @ 3ms = 144 samples → 256 (rounded to power of 2)
    //   96kHz @ 3ms = 288 samples → 512
    //   192kHz @ 3ms = 576 samples → 1024
    //   384kHz @ 3ms = 1152 samples → 2048
    //
    // Minimum 3ms buffer provides adequate tolerance for:
    // - Network jitter (typical: 0.5-1ms)
    // - Processing delays (typical: 0.5-1ms)
    // - Scheduling variations (typical: 0.5-1ms)
    //
    // Power-of-2 sizing enables efficient modulo operations

    // Calculate minimum size based on latency requirement
    const size_t minSize = static_cast<size_t>(
        (sampleRate * latencyMs) / 1000.0
    );

    // Round up to next power of 2 for efficient modulo operations
    size_t size = 1;
    while (size < minSize) {
        size <<= 1;
    }

    // Enforce absolute minimum (512 samples = 10.6ms @ 48kHz, 1.3ms @ 384kHz)
    constexpr size_t kMinRingBufferSize = 512;

    // Enforce maximum to prevent excessive memory (8192 samples = 21.3ms @ 384kHz)
    // At 128 channels × 4 bytes/sample: 8192 × 128 × 4 = 4MB per buffer direction
    constexpr size_t kMaxRingBufferSize = 8192;

    size = std::max(size, kMinRingBufferSize);
    size = std::min(size, kMaxRingBufferSize);

    return size;
}

void AES67Device::ResizeRingBuffers(Float64 sampleRate) {
    // IMPORTANT: Ring buffers cannot be resized after construction because
    // SPSCRingBuffer has deleted copy/move assignment operators.
    //
    // The ring buffers are sized based on sample rate at construction time.
    // If sample rate needs to change significantly (requiring different buffer size),
    // the device must be torn down and recreated.
    //
    // Current approach: Ring buffers are sized for worst-case (highest sample rate)
    // to avoid needing to resize. The buffer size calculation uses power-of-2 sizing,
    // so adjacent sample rates often share the same buffer size:
    //   - 44.1/48 kHz → 512 samples
    //   - 88.2/96 kHz → 512 samples
    //   - 176.4/192 kHz → 1024 samples
    //   - 352.8/384 kHz → 2048 samples
    //
    // This function logs a warning if sample rate change requires buffer resize.

    const size_t newSize = CalculateRingBufferSize(sampleRate);
    const size_t currentSize = inputBuffers_[0].capacity();  // All buffers same size

    if (newSize != currentSize) {
        AES67_LOGF("ResizeRingBuffers: WARNING - Sample rate change from %.0f Hz to %.0f Hz",
                   currentSampleRate_.load(), sampleRate);
        AES67_LOGF("ResizeRingBuffers: Would require buffer resize: %zu → %zu samples",
                   currentSize, newSize);
        AES67_LOG("ResizeRingBuffers: Ring buffers CANNOT be resized after construction");
        AES67_LOG("ResizeRingBuffers: Continuing with existing buffer size - may cause underruns");
    }
}

} // namespace AES67
