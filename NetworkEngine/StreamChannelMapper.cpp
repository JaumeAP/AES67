//
// StreamChannelMapper.cpp
// AES67 macOS Driver - Build #2
// CRITICAL: Stream-to-Channel mapping with validation and persistence
//

#include "StreamChannelMapper.h"
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

    if (!channelMap.empty() && channelMap.size() != streamChannelCount) {
        return "Custom channel map size doesn't match stream channel count";
    }

    return "";  // Valid
}

bool ChannelMapping::containsDeviceChannel(int deviceCh) const {
    if (channelMap.empty()) {
        // Sequential mapping
        return deviceCh >= deviceChannelStart &&
               deviceCh < (deviceChannelStart + deviceChannelCount);
    } else {
        // Custom mapping
        return std::find(channelMap.begin(), channelMap.end(), deviceCh) != channelMap.end();
    }
}

// ============================================================================
// StreamChannelMapper Implementation
// ============================================================================

StreamChannelMapper::StreamChannelMapper() {
    // Initialize all device channels as unassigned
    for (auto& owner : deviceChannelOwners_) {
        owner = StreamID::null();
    }
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
    for (auto& owner : deviceChannelOwners_) {
        owner = StreamID::null();
    }
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
    // channelMap left empty for sequential mapping

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

// Minimal field extractors matching the fixed layout produced by toJSON().
// Not a general JSON parser -- relies on the "key": value shape emitted above.

std::string extractStringField(const std::string& block, const std::string& key) {
    std::string pattern = "\"" + key + "\":";
    size_t pos = block.find(pattern);
    if (pos == std::string::npos) return {};

    size_t firstQuote = block.find('"', pos + pattern.size());
    if (firstQuote == std::string::npos) return {};
    size_t secondQuote = block.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos) return {};

    return block.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

std::optional<uint16_t> extractUintField(const std::string& block, const std::string& key) {
    std::string pattern = "\"" + key + "\":";
    size_t pos = block.find(pattern);
    if (pos == std::string::npos) return std::nullopt;
    pos += pattern.size();

    while (pos < block.size() && std::isspace(static_cast<unsigned char>(block[pos]))) pos++;

    size_t end = pos;
    while (end < block.size() && std::isdigit(static_cast<unsigned char>(block[end]))) end++;
    if (end == pos) return std::nullopt;

    return static_cast<uint16_t>(std::stoul(block.substr(pos, end - pos)));
}

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
        mapping.streamID = StreamID(extractStringField(block, "streamID"));
        mapping.streamName = extractStringField(block, "streamName");
        mapping.streamChannelCount = extractUintField(block, "streamChannelCount").value_or(0);
        mapping.streamChannelOffset = extractUintField(block, "streamChannelOffset").value_or(0);
        mapping.deviceChannelStart = extractUintField(block, "deviceChannelStart").value_or(0);
        mapping.deviceChannelCount = extractUintField(block, "deviceChannelCount").value_or(0);

        addMapping(mapping);
    }

    return true;
}

// ============================================================================
// Private Helper Functions
// ============================================================================

void StreamChannelMapper::updateDeviceChannelOwners(const ChannelMapping& mapping) {
    // Note: Caller must hold lock

    if (mapping.channelMap.empty()) {
        // Sequential mapping
        for (uint16_t i = 0; i < mapping.deviceChannelCount; i++) {
            int deviceCh = mapping.deviceChannelStart + i;
            deviceChannelOwners_[deviceCh] = mapping.streamID;
        }
    } else {
        // Custom mapping
        for (int deviceCh : mapping.channelMap) {
            if (deviceCh >= 0 && deviceCh < static_cast<int>(kMaxDeviceChannels)) {
                deviceChannelOwners_[deviceCh] = mapping.streamID;
            }
        }
    }
}

void StreamChannelMapper::clearDeviceChannelOwners(const StreamID& streamID) {
    // Note: Caller must hold lock

    for (auto& owner : deviceChannelOwners_) {
        if (owner == streamID) {
            owner = StreamID::null();
        }
    }
}

bool StreamChannelMapper::isRangeValid(uint16_t start, uint16_t count) const {
    return start < kMaxDeviceChannels &&
           (start + count) <= kMaxDeviceChannels;
}

bool StreamChannelMapper::isOverlapWithStream(const ChannelMapping& mapping, const StreamID& excludeStream) const {
    // Note: Caller must hold lock

    for (uint16_t i = 0; i < mapping.deviceChannelCount; i++) {
        int deviceCh = mapping.deviceChannelStart + i;
        const StreamID& owner = deviceChannelOwners_[deviceCh];

        if (!owner.isNull() && owner != excludeStream) {
            return true;  // Overlap detected
        }
    }

    return false;
}

} // namespace AES67
