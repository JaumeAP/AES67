//
// StreamChannelMapper.cpp
// AES67 macOS Driver - Build #2
// CRITICAL: Stream-to-Channel mapping with validation and persistence
//

#include "StreamChannelMapper.h"
#include "NetworkEngine/JsonFields.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace AES67 {

// ============================================================================
// ChannelMapping Implementation
// ============================================================================

bool ChannelMapping::isValid() const {
    return getValidationError().empty();  // Empty error = valid
}

std::string ChannelMapping::getValidationError() const {
    if (streamID.isNull()) {
        return "Stream ID is null";
    }

    if (streamChannelCount == 0) {
        return "Stream channel count must be non-zero";
    }

    if (deviceChannelCount == 0) {
        return "Device channel count must be non-zero";
    }

    if (deviceChannelStart >= StreamChannelMapper::kMaxDeviceChannels) {
        return "Device channel start out of range (0-127)";
    }

    if (deviceChannelStart + deviceChannelCount > StreamChannelMapper::kMaxDeviceChannels) {
        return "Device channel range exceeds maximum (128 channels)";
    }

    std::vector<bool> taken(StreamChannelMapper::kMaxDeviceChannels, false);
    for (const ChannelRoute& route : routes) {
        if (route.streamChannel >= streamChannelCount) {
            return "A route names stream channel " + std::to_string(route.streamChannel) +
                   ", and the stream has " + std::to_string(streamChannelCount);
        }
        if (route.deviceChannel >= StreamChannelMapper::kMaxDeviceChannels) {
            return "A route names device channel " + std::to_string(route.deviceChannel) +
                   ", and the device has " +
                   std::to_string(StreamChannelMapper::kMaxDeviceChannels);
        }
        // Two sources on one output is not a mix, it is a fault.
        if (taken[route.deviceChannel]) {
            return "Two routes land on device channel " +
                   std::to_string(route.deviceChannel);
        }
        taken[route.deviceChannel] = true;
    }

    return "";  // Valid
}

bool mappingFitsDevice(const ChannelMapping& mapping, uint16_t streamChannelCount) {
    if (mapping.routes.empty()) {
        // The block it was given. Both widths, because deviceChannels() emits
        // deviceChannelStart .. +deviceChannelCount and the two need not
        // agree with the stream's: a mapping declaring sixteen device
        // channels for a two-channel stream still lands sixteen of them, and
        // checking only the stream's width let it run off the end of
        // StreamChannelMapper's 128-entry owner table.
        const uint32_t width = std::max<uint32_t>(streamChannelCount, mapping.deviceChannelCount);
        return mapping.deviceChannelStart + width <= StreamChannelMapper::kMaxDeviceChannels;
    }

    for (const ChannelRoute& route : mapping.routes) {
        if (route.deviceChannel >= StreamChannelMapper::kMaxDeviceChannels) return false;
        if (route.streamChannel >= streamChannelCount) return false;
    }
    return true;
}

std::vector<int> ChannelMapping::deviceChannels() const {
    std::vector<int> channels;
    if (routes.empty()) {
        channels.reserve(deviceChannelCount);
        for (uint16_t i = 0; i < deviceChannelCount; i++) {
            channels.push_back(deviceChannelStart + i);
        }
        return channels;
    }

    channels.reserve(routes.size());
    for (const ChannelRoute& route : routes) channels.push_back(route.deviceChannel);
    return channels;
}

bool ChannelMapping::containsDeviceChannel(int deviceCh) const {
    if (routes.empty()) {
        // Sequential mapping
        return deviceCh >= deviceChannelStart &&
               deviceCh < (deviceChannelStart + deviceChannelCount);
    }
    return std::any_of(routes.begin(), routes.end(), [deviceCh](const ChannelRoute& route) {
        return route.deviceChannel == deviceCh;
    });
}

// ============================================================================
// StreamChannelMapper Implementation
// ============================================================================

StreamChannelMapper::StreamChannelMapper() {
    // Initialize all device channels as unassigned
    std::fill(deviceChannelOwners_.begin(), deviceChannelOwners_.end(), StreamID::null());
}

StreamChannelMapper::~StreamChannelMapper() = default;

bool StreamChannelMapper::addMapping(const ChannelMapping& mapping) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate mapping
    std::string error;
    if (!validateMapping(mapping, &error)) {
        return false;
    }

    // Check for overlaps
    if (isOverlapWithStream(mapping, StreamID::null())) {
        return false;
    }

    // Add mapping
    mappings_[mapping.streamID] = mapping;
    updateDeviceChannelOwners(mapping);

    return true;
}

bool StreamChannelMapper::removeMapping(const StreamID& streamID) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = mappings_.find(streamID);
    if (it == mappings_.end()) {
        return false;
    }

    clearDeviceChannelOwners(streamID);
    mappings_.erase(it);

    return true;
}

bool StreamChannelMapper::updateMapping(const ChannelMapping& mapping) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate mapping
    std::string error;
    if (!validateMapping(mapping, &error)) {
        return false;
    }

    // Check for overlaps (excluding this stream)
    if (isOverlapWithStream(mapping, mapping.streamID)) {
        return false;
    }

    // Remove old mapping
    clearDeviceChannelOwners(mapping.streamID);

    // Add new mapping
    mappings_[mapping.streamID] = mapping;
    updateDeviceChannelOwners(mapping);

    return true;
}

std::optional<ChannelMapping> StreamChannelMapper::getMapping(const StreamID& streamID) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = mappings_.find(streamID);
    if (it == mappings_.end()) {
        return std::nullopt;
    }

    return it->second;
}

std::vector<ChannelMapping> StreamChannelMapper::getAllMappings() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ChannelMapping> result;
    result.reserve(mappings_.size());

    for (const auto& [id, mapping] : mappings_) {
        result.push_back(mapping);
    }

    return result;
}

void StreamChannelMapper::clearAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    mappings_.clear();
    std::fill(deviceChannelOwners_.begin(), deviceChannelOwners_.end(), StreamID::null());
}

std::optional<ChannelMapping> StreamChannelMapper::createDefaultMapping(const SDPSession& sdp) {
    return createDefaultMapping(
        StreamID::generate(),
        sdp.sessionName,
        sdp.numChannels
    );
}

std::optional<ChannelMapping> StreamChannelMapper::createDefaultMapping(
    const StreamID& streamID,
    const std::string& streamName,
    uint16_t numChannels
) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Find first available contiguous block
    auto blockStart = findContiguousBlock(numChannels);
    if (!blockStart) {
        return std::nullopt;  // No space available
    }

    ChannelMapping mapping;
    mapping.streamID = streamID;
    mapping.streamName = streamName;
    mapping.streamChannelCount = numChannels;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = *blockStart;
    mapping.deviceChannelCount = numChannels;
    // routes left empty for sequential mapping

    return mapping;
}

bool StreamChannelMapper::validateMapping(const ChannelMapping& mapping, std::string* errorOut) const {
    std::string error = mapping.getValidationError();

    if (!error.empty()) {
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }

    return true;
}

bool StreamChannelMapper::hasOverlap(const ChannelMapping& mapping) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return isOverlapWithStream(mapping, mapping.streamID);
}

std::vector<StreamID> StreamChannelMapper::getOverlappingStreams(const ChannelMapping& mapping) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<StreamID> overlaps;

    for (uint16_t i = 0; i < mapping.deviceChannelCount; i++) {
        int deviceCh = mapping.deviceChannelStart + i;
        const StreamID& owner = deviceChannelOwners_[deviceCh];

        if (!owner.isNull() && owner != mapping.streamID) {
            if (std::find(overlaps.begin(), overlaps.end(), owner) == overlaps.end()) {
                overlaps.push_back(owner);
            }
        }
    }

    return overlaps;
}

std::optional<StreamID> StreamChannelMapper::getStreamForDeviceChannel(int deviceCh) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (deviceCh < 0 || deviceCh >= static_cast<int>(kMaxDeviceChannels)) {
        return std::nullopt;
    }

    const StreamID& owner = deviceChannelOwners_[deviceCh];
    if (owner.isNull()) {
        return std::nullopt;
    }

    return owner;
}

std::vector<int> StreamChannelMapper::getUnassignedDeviceChannels() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<int> unassigned;
    for (size_t i = 0; i < kMaxDeviceChannels; i++) {
        if (deviceChannelOwners_[i].isNull()) {
            unassigned.push_back(static_cast<int>(i));
        }
    }

    return unassigned;
}

size_t StreamChannelMapper::getAvailableChannelCount() const {
    return getUnassignedDeviceChannels().size();
}

size_t StreamChannelMapper::getUsedChannelCount() const {
    return kMaxDeviceChannels - getAvailableChannelCount();
}

bool StreamChannelMapper::isChannelAssigned(int deviceCh) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (deviceCh < 0 || deviceCh >= static_cast<int>(kMaxDeviceChannels)) {
        return false;
    }

    return !deviceChannelOwners_[deviceCh].isNull();
}

void StreamChannelMapper::setUsableChannelCount(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    usableChannelCount_ = std::min(count, kMaxDeviceChannels);
}

size_t StreamChannelMapper::getUsableChannelCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return usableChannelCount_;
}

std::optional<int> StreamChannelMapper::findContiguousBlock(size_t numChannels) const {
    // Note: Caller must hold lock

    int consecutiveCount = 0;
    int blockStart = -1;

    // Stops at usableChannelCount_, not kMaxDeviceChannels: channels above
    // the user's selected count stay advertised to Core Audio but are never
    // handed out to a stream.
    for (size_t i = 0; i < usableChannelCount_; i++) {
        if (deviceChannelOwners_[i].isNull()) {
            if (blockStart == -1) {
                blockStart = static_cast<int>(i);
            }
            consecutiveCount++;

            if (consecutiveCount >= static_cast<int>(numChannels)) {
                return blockStart;
            }
        } else {
            blockStart = -1;
            consecutiveCount = 0;
        }
    }

    return std::nullopt;  // No block found
}

bool StreamChannelMapper::save(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    file << toJSON();
    return true;
}

bool StreamChannelMapper::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    return fromJSON(buffer.str());
}

std::string StreamChannelMapper::toJSON() const {
    // Simple JSON generation (would use a library in production)
    std::ostringstream json;
    json << "{\n  \"mappings\": [\n";

    bool first = true;
    for (const auto& [id, mapping] : mappings_) {
        if (!first) json << ",\n";
        first = false;

        json << "    {\n"
             << "      \"streamID\": \"" << mapping.streamID.toString() << "\",\n"
             << "      \"streamName\": \"" << mapping.streamName << "\",\n"
             << "      \"streamChannelCount\": " << mapping.streamChannelCount << ",\n"
             << "      \"streamChannelOffset\": " << mapping.streamChannelOffset << ",\n"
             << "      \"deviceChannelStart\": " << mapping.deviceChannelStart << ",\n"
             << "      \"deviceChannelCount\": " << mapping.deviceChannelCount << "\n"
             << "    }";
    }

    json << "\n  ]\n}";
    return json.str();
}

namespace {




} // namespace

bool StreamChannelMapper::fromJSON(const std::string& json) {
    // Minimal parser for the fixed layout emitted by toJSON() (no external
    // JSON library dependency). Each mapping entry is a "{...}" block
    // containing a "streamID" field; the outer object itself is skipped.
    clearAll();

    size_t searchPos = 0;
    while (true) {
        size_t entryStart = json.find('{', searchPos);
        if (entryStart == std::string::npos) break;
        size_t entryEnd = json.find('}', entryStart);
        if (entryEnd == std::string::npos) break;

        std::string block = json.substr(entryStart, entryEnd - entryStart + 1);
        searchPos = entryEnd + 1;

        if (block.find("\"streamID\"") == std::string::npos) {
            continue;  // outer "{ \"mappings\": [ ... ] }" wrapper, not an entry
        }

        ChannelMapping mapping;
        mapping.streamID = StreamID(extractStringField(block, "streamID").value_or(std::string{}));
        mapping.streamName = extractStringField(block, "streamName").value_or(std::string{});
        mapping.streamChannelCount = extractUInt16Field(block, "streamChannelCount").value_or(0);
        mapping.streamChannelOffset = extractUInt16Field(block, "streamChannelOffset").value_or(0);
        mapping.deviceChannelStart = extractUInt16Field(block, "deviceChannelStart").value_or(0);
        mapping.deviceChannelCount = extractUInt16Field(block, "deviceChannelCount").value_or(0);

        addMapping(mapping);
    }

    return true;
}

// ============================================================================
// Private Helper Functions
// ============================================================================

void StreamChannelMapper::updateDeviceChannelOwners(const ChannelMapping& mapping) {
    // Note: Caller must hold lock

    for (int deviceCh : mapping.deviceChannels()) {
        if (deviceCh >= 0 && deviceCh < static_cast<int>(kMaxDeviceChannels)) {
            deviceChannelOwners_[deviceCh] = mapping.streamID;
        }
    }
}

void StreamChannelMapper::clearDeviceChannelOwners(const StreamID& streamID) {
    // Note: Caller must hold lock

    std::replace(deviceChannelOwners_.begin(), deviceChannelOwners_.end(),
                 streamID, StreamID::null());
}

bool StreamChannelMapper::isRangeValid(uint16_t start, uint16_t count) const {
    return start < kMaxDeviceChannels &&
           (start + count) <= kMaxDeviceChannels;
}

bool StreamChannelMapper::isOverlapWithStream(const ChannelMapping& mapping, const StreamID& excludeStream) const {
    // Note: Caller must hold lock

    // deviceChannels(), not the sequential block: ownership is recorded
    // against exactly this set by updateDeviceChannelOwners(), and for a
    // routed mapping the two are different. Walking the block let a mapping
    // whose routes point at channels another stream owns pass the check -- the
    // block it was auto-assigned was free -- and then claim those channels
    // anyway, which put two RTPReceiver threads on one SPSCRingBuffer whose
    // contract is a single producer, and left the channels the first stream
    // still writes reported as unassigned.
    for (int deviceCh : mapping.deviceChannels()) {
        if (deviceCh < 0 || deviceCh >= static_cast<int>(kMaxDeviceChannels)) continue;
        const StreamID& owner = deviceChannelOwners_[deviceCh];

        if (!owner.isNull() && owner != excludeStream) {
            return true;  // Overlap detected
        }
    }

    return false;
}

} // namespace AES67
