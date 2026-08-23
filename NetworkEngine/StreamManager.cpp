//
// StreamManager.cpp
// AES67 macOS Driver - Build #9
// Unified management of all AES67 streams with validation
//

#include "StreamManager.h"
#include "NetworkUtils.h"
#include "../Driver/DebugLog.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <ctime>

namespace AES67 {

StreamManager::StreamManager(DeviceChannelBuffers& inputChannels, DeviceChannelBuffers& outputChannels)
    : inputChannels_(inputChannels)
    , outputChannels_(outputChannels)
    , configManager_(std::make_unique<StreamConfigManager>())
{
}

PTPDiagnostics StreamManager::getPTPDiagnostics(int domain) {
    // ptpManager_ (the member) is never assigned anywhere in this class —
    // StreamManager doesn't actually own a PTP clock instance. Going
    // through the singleton directly here rather than fixing that: whether
    // a PTPClock is actually running for `domain` (and therefore whether
    // this returns real data or just PTPDiagnostics{}'s disconnected
    // defaults) depends on something calling
    // PTPClockManager::getInstance().getClockForDomain(domain) somewhere —
    // which nothing in the real driver path does today either. That's a
    // separate decision (starting PTP sockets/threads at driver startup is
    // a real behavior change, not just wiring a read-only query) — this
    // function reports whatever's actually there, honestly, rather than
    // silently starting something new.
    return PTPClockManager::getInstance().getDiagnostics(domain);
}

StreamManager::~StreamManager() {
    removeAllStreams();
}

//
// Stream Management - RX
//

StreamID StreamManager::addStream(const SDPSession& sdp) {
    // Auto-create channel mapping
    auto optMapping = mapper_.createDefaultMapping(sdp);
    if (!optMapping) {
        AES67_LOGF("StreamManager::addStream: failed to create default mapping for '%s' (%u channels)",
                   sdp.sessionName.c_str(), sdp.numChannels);
        return StreamID::null();
    }

    return addStream(sdp, *optMapping);
}

StreamID StreamManager::addStream(const SDPSession& sdp, const ChannelMapping& mapping) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Validate stream can be added
    std::string error;
    if (!canAddStream(sdp, /*isTransmit=*/false, &error)) {
        AES67_LOGF("StreamManager::addStream: validation failed for '%s': %s",
                   sdp.sessionName.c_str(), error.c_str());
        return StreamID::null();
    }

    // Generate unique stream ID
    StreamID id = StreamID::generate();

    // Check if stream already exists (shouldn't happen with UUIDs but be safe)
    if (streams_.find(id) != streams_.end()) {
        AES67_LOGF("StreamManager::addStream: duplicate stream ID for '%s'",
                   sdp.sessionName.c_str());
        return StreamID::null();
    }

    // Create complete mapping with stream ID
    ChannelMapping completeMapping = mapping;
    completeMapping.streamID = id;
    completeMapping.streamName = sdp.sessionName;
    completeMapping.streamChannelCount = sdp.numChannels;
    completeMapping.deviceChannelCount = sdp.numChannels;

    // Add mapping to mapper
    if (!mapper_.addMapping(completeMapping)) {
        AES67_LOGF("StreamManager::addStream: mapper rejected mapping for '%s' (devCh=%zu, count=%u)",
                   sdp.sessionName.c_str(), mapping.deviceChannelStart, sdp.numChannels);
        return StreamID::null();
    }

    // Create managed stream
    ManagedStream managed;
    managed.sdp = sdp;
    managed.mapping = completeMapping;
    managed.isTransmit = false;

    // Create RTP receiver
    managed.receiver = createReceiver(sdp, completeMapping);
    if (!managed.receiver) {
        AES67_LOGF("StreamManager::addStream: failed to create RTP receiver for '%s'",
                   sdp.sessionName.c_str());
        mapper_.removeMapping(id);
        return StreamID::null();
    }

    // Only start receiver if IO is active (a Core Audio client has called StartIO).
    // Otherwise the stream is created dormant and will be started by setIOActive(true).
    if (ioActive_.load()) {
        if (!managed.receiver->start()) {
            AES67_LOGF("StreamManager::addStream: failed to start RTP receiver for '%s' (%s:%u)",
                       sdp.sessionName.c_str(), sdp.connectionAddress.c_str(), sdp.port);
            mapper_.removeMapping(id);
            return StreamID::null();
        }
    }

    // Build stream info
    managed.info.id = id;
    managed.info.name = sdp.sessionName;
    managed.info.description = sdp.sessionInfo;

    // Network addresses
    managed.info.source.ip = sdp.sourceAddress;
    managed.info.source.port = sdp.port;
    managed.info.multicast.ip = sdp.connectionAddress;
    managed.info.multicast.port = sdp.port;
    managed.info.multicast.ttl = sdp.ttl;

    // Audio format
    if (sdp.encoding == "L16") {
        managed.info.encoding = AudioEncoding::L16;
    } else if (sdp.encoding == "L24") {
        managed.info.encoding = AudioEncoding::L24;
    } else {
        managed.info.encoding = AudioEncoding::Unknown;
    }

    managed.info.sampleRate = sdp.sampleRate;
    managed.info.numChannels = sdp.numChannels;
    managed.info.payloadType = sdp.payloadType;

    // Timing
    managed.info.ptime = sdp.ptimeUs;  // StreamInfo::ptime is microseconds
    managed.info.framecount = sdp.framecount;

    // PTP
    managed.info.ptp.domain = sdp.ptpDomain;

    // State
    managed.info.isActive = true;
    managed.info.isConnected = false;
    managed.info.startTime = std::chrono::steady_clock::now();

    // Store stream
    streams_[id] = std::move(managed);
    rxChannelsInUse_.fetch_add(sdp.numChannels, std::memory_order_relaxed);

    // Notify callback
    notifyStreamAdded(streams_[id].info);

    // Auto-save configuration
    autoSaveIfEnabled();

    return id;
}

StreamID StreamManager::importSDPFile(const std::string& filepath) {
    // Read SDP file
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return StreamID::null();
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string sdpContent = buffer.str();

    // Parse SDP
    auto sdpSession = SDPParser::parseString(sdpContent);
    if (!sdpSession) {
        return StreamID::null();
    }

    // Add stream with auto-mapping
    return addStream(*sdpSession);
}

bool StreamManager::removeStream(const StreamID& id) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return false;
    }

    // Save info for callback before deletion
    StreamInfo info = it->second.info;

    // Stop receiver/transmitter
    if (it->second.receiver) {
        it->second.receiver->stop();
    }
    if (it->second.transmitter) {
        it->second.transmitter->stop();
    }

    // Remove from mapper
    mapper_.removeMapping(id);

    // Keep the running totals in step with what's actually still open.
    (it->second.isTransmit ? txChannelsInUse_ : rxChannelsInUse_)
        .fetch_sub(it->second.sdp.numChannels, std::memory_order_relaxed);

    // Remove from map
    streams_.erase(it);

    // Notify callback
    notifyStreamRemoved(info);

    // Auto-save configuration
    autoSaveIfEnabled();

    return true;
}

void StreamManager::removeAllStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Stop all streams
    for (auto& pair : streams_) {
        if (pair.second.receiver) {
            pair.second.receiver->stop();
        }
        if (pair.second.transmitter) {
            pair.second.transmitter->stop();
        }

        notifyStreamRemoved(pair.second.info);
    }

    streams_.clear();
    mapper_.clearAll();
    rxChannelsInUse_.store(0, std::memory_order_relaxed);
    txChannelsInUse_.store(0, std::memory_order_relaxed);
}

//
// Stream Management - TX
//

StreamID StreamManager::createTxStream(
    const std::string& name,
    const std::string& multicastIP,
    uint16_t port,
    uint16_t numChannels,
    const ChannelMapping& mapping,
    uint16_t sourcePort
) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Build SDP session for transmit stream
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.connectionAddress = multicastIP;
    sdp.port = port;
    sdp.numChannels = numChannels;
    sdp.sampleRate = currentDeviceSampleRate_.load();
    sdp.encoding = "L24"; // Use L24 for best quality
    sdp.payloadType = 97; // Dynamic payload type
    sdp.sessionID = static_cast<uint64_t>(std::time(nullptr));
    sdp.sessionVersion = 1;

    // Validate
    std::string error;
    if (!canAddStream(sdp, /*isTransmit=*/true, &error)) {
        AES67_LOGF("StreamManager::createTxStream: validation failed for '%s': %s",
                   name.c_str(), error.c_str());
        return StreamID::null();
    }

    // Generate stream ID
    StreamID id = StreamID::generate();

    // Create complete mapping with stream ID
    ChannelMapping completeMapping = mapping;
    completeMapping.streamID = id;
    completeMapping.streamName = name;
    completeMapping.streamChannelCount = numChannels;
    completeMapping.deviceChannelCount = numChannels;

    // Add mapping
    if (!mapper_.addMapping(completeMapping)) {
        AES67_LOGF("StreamManager::createTxStream: mapper rejected mapping for '%s' (devCh=%zu, count=%u)",
                   name.c_str(), mapping.deviceChannelStart, numChannels);
        return StreamID::null();
    }

    // Create managed stream
    ManagedStream managed;
    managed.sdp = sdp;
    managed.mapping = completeMapping;
    managed.isTransmit = true;

    // Create RTP transmitter
    managed.transmitter = createTransmitter(sdp, completeMapping, /*networkInterface=*/"", sourcePort);
    if (!managed.transmitter) {
        AES67_LOGF("StreamManager::createTxStream: failed to create RTP transmitter for '%s'",
                   name.c_str());
        mapper_.removeMapping(id);
        return StreamID::null();
    }

    // Only start transmitter if IO is active (a Core Audio client has called StartIO).
    // Otherwise the stream is created dormant and will be started by setIOActive(true).
    if (ioActive_.load()) {
        if (!managed.transmitter->start()) {
            AES67_LOGF("StreamManager::createTxStream: failed to start RTP transmitter for '%s' (%s:%u)",
                       name.c_str(), multicastIP.c_str(), port);
            mapper_.removeMapping(id);
            return StreamID::null();
        }
    }

    // Build stream info
    managed.info.id = id;
    managed.info.name = name;
    managed.info.multicast.ip = multicastIP;
    managed.info.multicast.port = port;
    managed.info.encoding = AudioEncoding::L24;
    managed.info.sampleRate = sdp.sampleRate;
    managed.info.numChannels = numChannels;
    managed.info.payloadType = sdp.payloadType;
    managed.info.isActive = true;
    managed.info.startTime = std::chrono::steady_clock::now();

    // Store stream
    streams_[id] = std::move(managed);
    txChannelsInUse_.fetch_add(numChannels, std::memory_order_relaxed);

    // Notify callback
    notifyStreamAdded(streams_[id].info);

    // Auto-save configuration
    autoSaveIfEnabled();

    return id;
}

namespace {

/// Advances a dotted-quad's last octet by `offset`. Returns false if the
/// result would exceed 255 (or the input isn't a dotted quad) — callers
/// must not silently wrap into a different /24.
bool advanceLastOctet(const std::string& ip, unsigned offset, std::string& out) {
    const size_t lastDot = ip.find_last_of('.');
    if (lastDot == std::string::npos || lastDot + 1 >= ip.size()) return false;

    const std::string prefix = ip.substr(0, lastDot + 1);
    const std::string lastPart = ip.substr(lastDot + 1);
    if (lastPart.empty() ||
        lastPart.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }

    const unsigned long base = std::stoul(lastPart);
    const unsigned long advanced = base + offset;
    if (advanced > 255) return false;

    out = prefix + std::to_string(advanced);
    return true;
}

} // namespace

std::vector<StreamID> StreamManager::createTxStreamFlows(
    const std::string& baseName,
    const std::string& baseMulticastIP,
    uint16_t port,
    uint16_t numChannels,
    const ChannelMapping& mapping
) {
    // Note: deliberately NOT holding streamsMutex_ here — createTxStream()
    // and removeStream() below each take it themselves.

    std::vector<StreamID> created;
    if (numChannels == 0) return created;

    // See CompatibilityProfile::useFixedMulticastWithPerFlowSourcePort's
    // doc comment: true for the Dolby DMA profile's real Atmos Connect
    // wire scheme (one multicast address, fixed destination port, source
    // port stepped per flow); false is every other profile's AES67/Dante
    // convention (multicast address stepped per flow, fixed port).
    const auto profile = CompatibilityProfile::forKind(profileKind_.load(std::memory_order_relaxed));
    const bool dolbyScheme = profile.useFixedMulticastWithPerFlowSourcePort;

    // Which unit in the chain we're feeding, as a flow-port offset. Only
    // the Dolby scheme distinguishes units by source port, so this is
    // deliberately not applied to the address-stepping scheme — there it
    // would silently mean nothing.
    const unsigned unitOffset = dolbyScheme
        ? txFlowPortOffset_.load(std::memory_order_relaxed)
        : 0u;

    constexpr uint16_t kPerFlow = StreamChannelMapper::kMaxChannelsPerFlow;
    const unsigned flowCount = (numChannels + kPerFlow - 1) / kPerFlow;

    for (unsigned flow = 0; flow < flowCount; ++flow) {
        std::string flowIP = baseMulticastIP;
        uint16_t flowSourcePort = 0;

        if (dolbyScheme) {
            // Fixed address, fixed destination port (both == baseMulticastIP/
            // port for every flow, set above/below); only the source port
            // steps, matching the DMA's own documented defaults (6517 fixed
            // destination, 6518/6519/6520/... source) when the caller passes
            // 6517 as `port`. unitOffset shifts the whole walk to the
            // selected amplifier unit's own channel group — see
            // setTxFlowPortOffset().
            const unsigned candidate = static_cast<unsigned>(port) + 1 + unitOffset + flow;
            if (candidate > 0xFFFF) {
                AES67_LOGF("StreamManager::createTxStreamFlows: '%s' needs %u flows but "
                           "source port %u + 1 + %u (unit offset) + %u overruns 65535 "
                           "— rolling back",
                           baseName.c_str(), flowCount, port, unitOffset, flow);
                for (const auto& id : created) removeStream(id);
                return {};
            }
            flowSourcePort = static_cast<uint16_t>(candidate);
        } else if (!advanceLastOctet(baseMulticastIP, flow, flowIP)) {
            AES67_LOGF("StreamManager::createTxStreamFlows: '%s' needs %u flows but "
                       "%s + %u overruns the subnet — rolling back",
                       baseName.c_str(), flowCount, baseMulticastIP.c_str(), flow);
            for (const auto& id : created) removeStream(id);
            return {};
        }

        // Last flow carries the remainder, which may be fewer than 8.
        const uint16_t remaining = numChannels - static_cast<uint16_t>(flow * kPerFlow);
        const uint16_t flowChannels = std::min<uint16_t>(remaining, kPerFlow);

        ChannelMapping flowMapping = mapping;
        flowMapping.deviceChannelStart =
            static_cast<uint16_t>(mapping.deviceChannelStart + flow * kPerFlow);
        flowMapping.streamChannelCount = flowChannels;
        flowMapping.deviceChannelCount = flowChannels;
        flowMapping.channelMap.clear(); // sequential within the flow; a custom
                                        // map for the whole group wouldn't
                                        // carry over meaningfully per-flow

        const std::string flowName = (flowCount == 1)
            ? baseName
            : baseName + " (flow " + std::to_string(flow + 1) +
              "/" + std::to_string(flowCount) + ")";

        const StreamID id = createTxStream(flowName, flowIP, port, flowChannels, flowMapping, flowSourcePort);
        if (id.isNull()) {
            AES67_LOGF("StreamManager::createTxStreamFlows: flow %u/%u of '%s' failed "
                       "— rolling back %zu already created",
                       flow + 1, flowCount, baseName.c_str(), created.size());
            for (const auto& previous : created) removeStream(previous);
            return {};
        }
        created.push_back(id);
    }

    AES67_LOGF("StreamManager::createTxStreamFlows: '%s' created as %u flow(s) of up to %u channels",
               baseName.c_str(), flowCount, kPerFlow);
    return created;
}

bool StreamManager::exportSDPFile(const StreamID& id, const std::string& filepath) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return false;
    }

    // Generate SDP content
    std::string sdpContent = SDPParser::generate(it->second.sdp);
    if (sdpContent.empty()) {
        return false;
    }

    // Write to file
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    file << sdpContent;
    return true;
}

//
// Channel Mapping
//

bool StreamManager::updateMapping(const StreamID& id, const ChannelMapping& newMapping) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return false;
    }

    // Update mapper
    ChannelMapping completeMapping = newMapping;
    completeMapping.streamID = id;
    completeMapping.streamName = it->second.mapping.streamName;
    completeMapping.streamChannelCount = it->second.sdp.numChannels;

    if (!mapper_.updateMapping(completeMapping)) {
        return false;
    }

    // Update managed stream
    it->second.mapping = completeMapping;

    // Update receiver/transmitter
    bool updated = false;
    if (it->second.receiver) {
        updated = it->second.receiver->updateMapping(completeMapping);
    } else if (it->second.transmitter) {
        updated = it->second.transmitter->updateMapping(completeMapping);
    }

    if (updated) {
        notifyStreamStatusChanged(it->second.info);

        // Auto-save configuration
        autoSaveIfEnabled();
    }

    return updated;
}

std::optional<ChannelMapping> StreamManager::getMapping(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return mapper_.getMapping(id);
}

std::vector<ChannelMapping> StreamManager::getAllMappings() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return mapper_.getAllMappings();
}

//
// Query
//

std::vector<StreamInfo> StreamManager::getActiveStreams() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    std::vector<StreamInfo> activeStreams;
    for (const auto& pair : streams_) {
        if (pair.second.info.isActive) {
            activeStreams.push_back(pair.second.info);
        }
    }

    return activeStreams;
}

std::optional<StreamInfo> StreamManager::getStreamInfo(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it != streams_.end()) {
        return it->second.info;
    }

    return std::nullopt;
}

bool StreamManager::hasStream(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return streams_.find(id) != streams_.end();
}

size_t StreamManager::getStreamCount() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return streams_.size();
}

//
// Validation
//

void StreamManager::setCompatibilityProfile(CompatibilityProfileKind kind) {
    // Atomic rather than mutex-guarded: canAddStream() reads this, and it
    // is already called with streamsMutex_ held (from addStream) as well as
    // without it (from getAddStreamError) — so it can neither take the lock
    // itself nor rely on callers having taken it.
    profileKind_.store(kind, std::memory_order_relaxed);
    AES67_LOGF("StreamManager: compatibility profile set to '%s'",
               CompatibilityProfile::forKind(kind).displayName.c_str());
    // Deliberately does NOT re-validate streams already added: tightening
    // the profile shouldn't silently tear down running audio. It applies to
    // everything added from here on.
}

CompatibilityProfileKind StreamManager::getCompatibilityProfileKind() const {
    return profileKind_.load(std::memory_order_relaxed);
}

bool StreamManager::canAddStream(const SDPSession& sdp, bool isTransmit, std::string* errorOut) const {
    if (!validateSampleRate(sdp, errorOut)) {
        return false;
    }

    if (!validateChannelAvailability(sdp.numChannels, errorOut)) {
        return false;
    }

    if (!validateNetworkConfig(sdp, errorOut)) {
        return false;
    }

    // Profile limits last: the checks above are about whether this driver
    // can carry the stream at all, this one is about whether the gear we're
    // pointed at would accept it.
    const auto profile = CompatibilityProfile::forKind(
        profileKind_.load(std::memory_order_relaxed));
    if (!profile.validate(sdp, isTransmit, errorOut)) {
        return false;
    }

    // maxTotalChannels is cumulative across every stream in this direction
    // while the profile is active — CompatibilityProfile::validate() can't
    // check it, it only ever sees one SDPSession at a time.
    if (profile.maxTotalChannels > 0) {
        const uint32_t inUse = (isTransmit ? txChannelsInUse_ : rxChannelsInUse_)
                                    .load(std::memory_order_relaxed);
        if (inUse + sdp.numChannels > profile.maxTotalChannels) {
            if (errorOut) {
                *errorOut = profile.displayName + ": " + std::to_string(sdp.numChannels) +
                           " more channels would bring total " + (isTransmit ? "TX" : "RX") +
                           " usage to " + std::to_string(inUse + sdp.numChannels) +
                           ", over its " + std::to_string(profile.maxTotalChannels) + "-channel limit";
            }
            return false;
        }
    }

    // User-configured per-direction cap (DeviceChannelSettings.rx/tx via
    // setUsableChannelCount/setUsableTxChannelCount) — same cumulative check
    // as maxTotalChannels above, just driven by the user's own selector
    // instead of the active profile. Checked in addition to, not instead of,
    // the profile limit: whichever is stricter wins.
    const uint32_t usableCount = (isTransmit ? usableTxChannelCount_ : usableRxChannelCount_)
                                      .load(std::memory_order_relaxed);
    if (usableCount > 0) {
        const uint32_t inUse = (isTransmit ? txChannelsInUse_ : rxChannelsInUse_)
                                    .load(std::memory_order_relaxed);
        if (inUse + sdp.numChannels > usableCount) {
            if (errorOut) {
                *errorOut = std::to_string(sdp.numChannels) +
                           " more channels would bring total " + (isTransmit ? "TX" : "RX") +
                           " usage to " + std::to_string(inUse + sdp.numChannels) +
                           ", over the " + std::to_string(usableCount) +
                           " channels selected for " + (isTransmit ? "output" : "input") +
                           " in ManagerApp";
            }
            return false;
        }
    }

    return true;
}

std::string StreamManager::getAddStreamError(const SDPSession& sdp, bool isTransmit) const {
    std::string error;
    canAddStream(sdp, isTransmit, &error);
    return error;
}

//
// Device State
//

void StreamManager::setIOActive(bool active) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    bool wasActive = ioActive_.exchange(active);
    if (wasActive == active) {
        return; // No state change
    }

    if (active) {
        AES67_LOGF("StreamManager::setIOActive: Starting %zu stream(s)", streams_.size());
        for (auto& [id, managed] : streams_) {
            if (managed.receiver) {
                managed.receiver->start();
            }
            if (managed.transmitter) {
                managed.transmitter->start();
            }
        }
    } else {
        AES67_LOGF("StreamManager::setIOActive: Stopping %zu stream(s)", streams_.size());
        for (auto& [id, managed] : streams_) {
            if (managed.receiver) {
                managed.receiver->stop();
            }
            if (managed.transmitter) {
                managed.transmitter->stop();
            }
        }
    }
}

bool StreamManager::setDeviceSampleRate(double sampleRate) {
    if (sampleRate < 44100 || sampleRate > 384000) {
        return false;
    }

    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Check if any streams would be incompatible
    for (const auto& pair : streams_) {
        if (std::abs(pair.second.sdp.sampleRate - sampleRate) > 0.1) {
            return false;
        }
    }

    currentDeviceSampleRate_.store(sampleRate);
    return true;
}

size_t StreamManager::getAvailableChannelCount() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return mapper_.getAvailableChannelCount();
}

//
// Validation Helpers
//

bool StreamManager::validateSampleRate(const SDPSession& sdp, std::string* errorOut) const {
    const double deviceRate = currentDeviceSampleRate_.load();

    if (std::abs(sdp.sampleRate - deviceRate) > 0.1) {
        if (errorOut) {
            *errorOut = "Sample rate mismatch: stream=" + std::to_string(static_cast<int>(sdp.sampleRate)) +
                       " Hz, device=" + std::to_string(static_cast<int>(deviceRate)) + " Hz";
        }
        return false;
    }

    return true;
}

bool StreamManager::validateChannelAvailability(uint16_t numChannels, std::string* errorOut) const {
    if (numChannels == 0 || numChannels > 128) {
        if (errorOut) {
            *errorOut = "Invalid channel count: " + std::to_string(numChannels) + " (must be 1-128)";
        }
        return false;
    }

    size_t available = mapper_.getAvailableChannelCount();
    if (numChannels > available) {
        if (errorOut) {
            *errorOut = "Insufficient channels: need " + std::to_string(numChannels) +
                       ", have " + std::to_string(available);
        }
        return false;
    }

    return true;
}

bool StreamManager::validateNetworkConfig(const SDPSession& sdp, std::string* errorOut) const {
    if (sdp.connectionAddress.empty()) {
        if (errorOut) {
            *errorOut = "Missing multicast IP address";
        }
        return false;
    }

    // Check for valid multicast range (239.x.x.x for AES67)
    if (sdp.connectionAddress.substr(0, 4) != "239.") {
        if (errorOut) {
            *errorOut = "Invalid multicast IP: " + sdp.connectionAddress +
                       " (AES67 requires 239.x.x.x)";
        }
        return false;
    }

    if (sdp.port == 0) {
        if (errorOut) {
            *errorOut = "Invalid port: 0";
        }
        return false;
    }

    // Check multicast routing configuration
    // Get primary ethernet interface for the check
    std::string primaryIface = NetworkUtils::getPrimaryEthernetInterface();

    if (!NetworkUtils::hasMulticastRoute(primaryIface)) {
        if (errorOut) {
            std::string iface = primaryIface.empty() ? "en0" : primaryIface;
            *errorOut = "WARNING: Multicast routing not configured. Audio may not flow.\n"
                       "To fix, run: " + NetworkUtils::getMulticastRouteCommand(iface) + "\n"
                       "Stream will be added but may not receive traffic.";
        }
        // Log warning but don't block stream addition
        AES67_LOG("WARNING: Multicast routing not configured - audio may not flow");
        if (!primaryIface.empty()) {
            AES67_LOGF("Run: %s", NetworkUtils::getMulticastRouteCommand(primaryIface).c_str());
        }
        // Return true to allow stream addition with warning
    }

    return true;
}

//
// Stream Creation Helpers
//

std::unique_ptr<RTPReceiver> StreamManager::createReceiver(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    size_t jitterBufferDepth,
    const std::string& networkInterface
) {
    // Receivers write decoded network audio to INPUT buffers (Network → Core Audio)
    return std::make_unique<RTPReceiver>(sdp, mapping, inputChannels_, jitterBufferDepth, networkInterface);
}

std::unique_ptr<RTPTransmitter> StreamManager::createTransmitter(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    const std::string& networkInterface,
    uint16_t sourcePort
) {
    // Mark outgoing audio with whatever DSCP the active profile documents
    // for its gear (-1 = nothing documented, leave the socket unmarked).
    // This is the one place the profiles' recommendedDscp actually reaches
    // the wire — see CompatibilityProfile::recommendedDscp.
    const auto profile = CompatibilityProfile::forKind(profileKind_.load(std::memory_order_relaxed));

    // Transmitters read audio from OUTPUT buffers (Core Audio → Network)
    return std::make_unique<RTPTransmitter>(sdp, mapping, outputChannels_, networkInterface,
                                            sourcePort, profile.recommendedDscp);
}

//
// Callback Invocation
//

void StreamManager::notifyStreamAdded(const StreamInfo& info) {
    if (streamAddedCallback_) {
        streamAddedCallback_(info);
    }
}

void StreamManager::notifyStreamRemoved(const StreamInfo& info) {
    if (streamRemovedCallback_) {
        streamRemovedCallback_(info);
    }
}

void StreamManager::notifyStreamStatusChanged(const StreamInfo& info) {
    if (streamStatusCallback_) {
        streamStatusCallback_(info);
    }
}

//
// Configuration Persistence
//

bool StreamManager::loadSavedStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Load configurations from disk
    auto configs = configManager_->loadConfig();
    if (!configs) {
        AES67_LOG("StreamManager: No saved stream configurations found");
        return false;
    }

    AES67_LOGF("StreamManager: Loading %zu saved stream configurations", configs->size());

    // Add each saved stream
    int loadedCount = 0;
    int failedCount = 0;

    for (const auto& config : *configs) {
        // Skip disabled streams
        if (!config.enabled) {
            AES67_LOGF("StreamManager: Skipping disabled stream: %s", config.sdp.sessionName.c_str());
            continue;
        }

        // Validate config
        if (!config.isValid()) {
            AES67_LOGF("StreamManager: Invalid stream config: %s", config.sdp.sessionName.c_str());
            failedCount++;
            continue;
        }

        // Check if stream can be added (validate sample rate, channels, etc.)
        // loadSavedStreams() only ever restores RX streams.
        std::string error;
        if (!canAddStream(config.sdp, /*isTransmit=*/false, &error)) {
            AES67_LOGF("StreamManager: Cannot add stream '%s': %s",
                      config.sdp.sessionName.c_str(), error.c_str());
            failedCount++;
            continue;
        }

        // Generate new StreamID (use the one from saved mapping)
        StreamID id = config.mapping.streamID;

        // Check if stream already exists
        if (streams_.find(id) != streams_.end()) {
            AES67_LOGF("StreamManager: Stream already exists: %s", config.sdp.sessionName.c_str());
            failedCount++;
            continue;
        }

        // Add mapping to mapper
        if (!mapper_.addMapping(config.mapping)) {
            AES67_LOGF("StreamManager: Failed to add mapping for stream: %s",
                      config.sdp.sessionName.c_str());
            failedCount++;
            continue;
        }

        // Create managed stream
        ManagedStream managed;
        managed.sdp = config.sdp;
        managed.mapping = config.mapping;
        managed.isTransmit = (config.sdp.direction == "sendonly" || config.sdp.direction == "sendrecv");

        // Create RTP receiver or transmitter (only start if IO is active)
        if (managed.isTransmit) {
            managed.transmitter = createTransmitter(config.sdp, config.mapping, config.networkInterface);
            if (!managed.transmitter) {
                mapper_.removeMapping(id);
                failedCount++;
                continue;
            }
            if (ioActive_.load() && !managed.transmitter->start()) {
                mapper_.removeMapping(id);
                failedCount++;
                continue;
            }
        } else {
            managed.receiver = createReceiver(config.sdp, config.mapping, config.jitterBufferDepth, config.networkInterface);
            if (!managed.receiver) {
                mapper_.removeMapping(id);
                failedCount++;
                continue;
            }
            if (ioActive_.load() && !managed.receiver->start()) {
                mapper_.removeMapping(id);
                failedCount++;
                continue;
            }
        }

        // Build stream info (same as in addStream)
        managed.info.id = id;
        managed.info.name = config.sdp.sessionName;
        managed.info.description = config.sdp.sessionInfo;
        managed.info.source.ip = config.sdp.sourceAddress;
        managed.info.source.port = config.sdp.port;
        managed.info.multicast.ip = config.sdp.connectionAddress;
        managed.info.multicast.port = config.sdp.port;
        managed.info.multicast.ttl = config.sdp.ttl;

        if (config.sdp.encoding == "L16") {
            managed.info.encoding = AudioEncoding::L16;
        } else if (config.sdp.encoding == "L24") {
            managed.info.encoding = AudioEncoding::L24;
        } else {
            managed.info.encoding = AudioEncoding::Unknown;
        }

        managed.info.sampleRate = config.sdp.sampleRate;
        managed.info.numChannels = config.sdp.numChannels;
        managed.info.payloadType = config.sdp.payloadType;
        managed.info.ptime = config.sdp.ptimeUs;
        managed.info.framecount = config.sdp.framecount;
        managed.info.ptp.domain = config.sdp.ptpDomain;
        managed.info.isActive = true;
        managed.info.isConnected = false;
        managed.info.startTime = std::chrono::steady_clock::now();

        // Store stream
        streams_[id] = std::move(managed);
        // loadSavedStreams() only ever restores RX streams — see the
        // canAddStream() call above this block.
        rxChannelsInUse_.fetch_add(config.sdp.numChannels, std::memory_order_relaxed);

        // Notify callback
        notifyStreamAdded(streams_[id].info);

        loadedCount++;
        AES67_LOGF("StreamManager: Loaded stream: %s (%s)",
                  config.sdp.sessionName.c_str(),
                  id.toString().c_str());
    }

    AES67_LOGF("StreamManager: Loaded %d streams successfully, %d failed",
              loadedCount, failedCount);

    return loadedCount > 0;
}

bool StreamManager::saveAllStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return saveAllStreamsInternal();
}

bool StreamManager::saveAllStreamsInternal() {
    // NOTE: Caller must hold streamsMutex_ lock

    std::vector<PersistedStreamConfig> configs;
    configs.reserve(streams_.size());

    // Convert all streams to persisted configs
    for (const auto& pair : streams_) {
        const auto& managed = pair.second;

        PersistedStreamConfig config = StreamConfigManager::createConfig(
            managed.sdp,
            managed.mapping,
            managed.info.description
        );

        configs.push_back(config);
    }

    // Save to disk
    bool success = configManager_->saveConfig(configs);

    if (success) {
        AES67_LOGF("StreamManager: Saved %zu stream configurations to disk", configs.size());
    } else {
        AES67_LOG("StreamManager: Failed to save stream configurations");
    }

    return success;
}

void StreamManager::autoSaveIfEnabled() {
    // NOTE: We're already holding streamsMutex_ when this is called
    // from addStream/removeStream/updateMapping, so use the internal version
    if (autoSaveEnabled_) {
        saveAllStreamsInternal();
    }
}

} // namespace AES67
