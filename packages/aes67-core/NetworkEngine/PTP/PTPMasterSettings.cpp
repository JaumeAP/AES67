#include <algorithm>
#include <cmath>
#include "PTPMasterSettings.h"
#include "NetworkEngine/JsonEscape.h"
#include "NetworkEngine/JsonFields.h"
#include "Profiles/ConfigPaths.h"
#include "../../Driver/DebugLog.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

namespace AES67 {

PTPMasterSettingsManager::PTPMasterSettingsManager() {
    std::string existing = findExistingConfig();
    if (!existing.empty()) {
        configPath_ = existing;
    } else {
        // Not the system-wide path unconditionally: coreaudiod runs as
        // _coreaudiod and cannot create it, and neither can a tool or a
        // test. The first of the same search paths this process could
        // actually write -- the system one when it is already there, so an
        // installed driver keeps its settings where it kept them. See
        // StreamConfigManager's constructor, fixed the same way for the
        // same reason.
        configPath_ = firstWritableConfigPath("AES67_PTP_MASTER_CONFIG_PATH",
                                              kDefaultConfigFile,
                                              /*systemBeforeHome=*/true);
    }
}

PTPMasterSettingsManager::~PTPMasterSettingsManager() = default;

const std::string& PTPMasterSettingsManager::getConfigPath() const { return configPath_; }

std::vector<std::string> PTPMasterSettingsManager::getConfigSearchPaths() {
    return configSearchPaths("AES67_PTP_MASTER_CONFIG_PATH", kDefaultConfigFile);
}

std::string PTPMasterSettingsManager::findExistingConfig() {
    return AES67::findExistingConfig("AES67_PTP_MASTER_CONFIG_PATH", kDefaultConfigFile);
}

bool PTPMasterSettingsManager::ensureConfigDirectoryExists() {
    return ensureParentDirectory(configPath_, "PTPMasterSettingsManager");
}

namespace {

// Tiny hand-rolled extraction for this file's two known string fields —
// full generic JSON parsing (as StreamConfig.cpp needs, for an array of
// stream objects) is overkill for one flat object with two keys.

/// An integer field, without exceptions: this parses a file the app wrote
/// and a person may have edited by hand, and std::stoi throws on anything
/// that is not a number.


} // namespace

PTPMasterSettings PTPMasterSettingsManager::load() {
    PTPMasterSettings settings; // defaults: masterCapable=false, i.e. old slave-only behavior

    std::ifstream file(configPath_);
    if (!file.is_open()) return settings;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    if (auto v = extractBoolField(json, "masterCapable")) settings.masterCapable = *v;
    if (auto v = extractBoolField(json, "ptpEnabled")) settings.ptpEnabled = *v;
    if (auto v = extractBoolField(json, "requireLock")) settings.requireLock = *v;
    if (auto v = extractStringField(json, "clockSourceKind")) settings.clockSourceKind = *v;
    if (auto v = extractStringField(json, "lockToDeviceUID")) settings.lockToDeviceUID = *v;
    if (auto v = extractIntField(json, "priority1")) settings.priority1 = *v;
    if (auto v = extractIntField(json, "priority2")) settings.priority2 = *v;
    if (auto v = extractIntField(json, "clockClass")) settings.clockClass = *v;
    if (auto v = extractIntField(json, "clockAccuracy")) settings.clockAccuracy = *v;
    // The millisecond keys first, so that the exponent wins when a file has
    // both: a file written by this driver carries both, and one written by an
    // older build or by ManagerApp carries only the milliseconds. Reading the
    // old key is what keeps a settings file that predates this from silently
    // reverting to the defaults.
    if (auto v = extractIntField(json, "syncIntervalMs")) {
        settings.logSyncInterval = msToLogInterval(*v);
    }
    if (auto v = extractIntField(json, "announceIntervalMs")) {
        settings.logAnnounceInterval = msToLogInterval(*v);
    }
    // Clamped to the range Profiles/PtpIntervals.h treats as an interval to
    // follow, -9..21, not merely to what fits in int8_t. Two reasons, and
    // the first is not cosmetic: narrowing a raw JSON int straight to int8_t
    // is implementation-defined before C++20 and wraps under C++20's own
    // rule, so "logSyncInterval": 200 became -56 rather than being refused.
    // -56 is outside -9..21, and ptpLogIntervalToNanoseconds already answers
    // 0 for that -- which is a period of zero, pinning PTPMaster's transmit
    // loop at "now" forever: a busy loop flooding the segment with Sync and
    // Follow_Up at real-time priority. Clamping only to int8_t's own range
    // stops the wraparound but not this: -56 fits in an int8_t perfectly
    // well. Clamping to the domain the conversion actually honours stops
    // both at once, for a value that wrapped and for one that did not.
    if (auto v = extractIntField(json, "logSyncInterval")) {
        settings.logSyncInterval = static_cast<int8_t>(std::clamp(*v, -9, 21));
    }
    if (auto v = extractIntField(json, "logAnnounceInterval")) {
        settings.logAnnounceInterval = static_cast<int8_t>(std::clamp(*v, -9, 21));
    }
    if (auto v = extractIntField(json, "delayReqIntervalMs")) settings.delayReqIntervalMs = *v;
    if (auto v = extractStringField(json, "delayMechanism")) settings.delayMechanism = *v;
    if (auto v = extractIntField(json, "dscp")) settings.dscp = *v;

    AES67_LOGF("PTPMasterSettingsManager: Loaded from %s (masterCapable=%s, clockSourceKind=%s)",
               configPath_.c_str(), settings.masterCapable ? "true" : "false",
               settings.clockSourceKind.c_str());
    return settings;
}

bool PTPMasterSettingsManager::save(const PTPMasterSettings& settings) {
    if (!ensureConfigDirectoryExists()) {
        AES67_LOG("PTPMasterSettingsManager: Failed to create config directory");
        return false;
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": \"1.0\",\n";
    json << "  \"masterCapable\": " << (settings.masterCapable ? "true" : "false") << ",\n";
    json << "  \"ptpEnabled\": " << (settings.ptpEnabled ? "true" : "false") << ",\n";
    json << "  \"requireLock\": " << (settings.requireLock ? "true" : "false") << ",\n";
    json << "  \"clockSourceKind\": \"" << jsonEscape(settings.clockSourceKind) << "\",\n";
    json << "  \"lockToDeviceUID\": \"" << jsonEscape(settings.lockToDeviceUID) << "\",\n";
    json << "  \"priority1\": " << settings.priority1 << ",\n";
    json << "  \"priority2\": " << settings.priority2 << ",\n";
    json << "  \"clockClass\": " << settings.clockClass << ",\n";
    json << "  \"clockAccuracy\": " << settings.clockAccuracy << ",\n";
    // Both: the exponent is what this driver reads back, and the rounded
    // milliseconds are what ManagerApp's PTP screen shows and writes. An app
    // that only knows the old keys keeps working, and loses only what
    // milliseconds could never express anyway.
    json << "  \"logSyncInterval\": " << static_cast<int>(settings.logSyncInterval) << ",\n";
    json << "  \"logAnnounceInterval\": " << static_cast<int>(settings.logAnnounceInterval) << ",\n";
    json << "  \"syncIntervalMs\": " << static_cast<long long>(std::llround(settings.syncIntervalMs()))
         << ",\n";
    json << "  \"announceIntervalMs\": "
         << static_cast<long long>(std::llround(settings.announceIntervalMs())) << ",\n";
    json << "  \"delayReqIntervalMs\": " << settings.delayReqIntervalMs << ",\n";
    json << "  \"delayMechanism\": \"" << jsonEscape(settings.delayMechanism) << "\",\n";
    json << "  \"dscp\": " << settings.dscp << "\n";
    json << "}\n";

    std::ofstream file(configPath_);
    if (!file.is_open()) {
        AES67_LOGF("PTPMasterSettingsManager: Failed to open %s for writing", configPath_.c_str());
        return false;
    }
    file << json.str();
    AES67_LOGF("PTPMasterSettingsManager: Saved to %s", configPath_.c_str());
    return true;
}

} // namespace AES67
