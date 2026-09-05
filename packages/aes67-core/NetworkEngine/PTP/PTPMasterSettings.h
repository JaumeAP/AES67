//
// PTPMasterSettings.h
// AES67 macOS Driver
// Persisted choice of PTP clock source — the "internal, or lock to this
// other audio device" setting ManagerApp's UI writes and the driver reads
// at startup. Same search-path / directory convention as StreamConfig.h's
// StreamConfigManager, kept in its own small file since this is a
// driver-wide setting, not a per-stream one.
//
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace AES67 {

struct PTPMasterSettings {
    bool masterCapable{false}; // false: PTPDInterface stays slave-only, unchanged from before this feature existed

    /// Whether the driver runs a PTP clock at all — see
    /// StreamManager::setPTPEnabled(). False keeps the behaviour every
    /// build before this had: the PTP subsystem compiled and never
    /// started. Opt-in because starting it opens sockets and threads on a
    /// path verified against hardware without them.
    bool ptpEnabled{false};

    /// Whether to refuse audio until the clock has locked. Only meaningful
    /// alongside ptpEnabled. False by default for the blunt reason that on
    /// a system carrying audio today, turning it on can only take audio
    /// away.
    bool requireLock{false};

    // "internal" or "localAudioDevice" — mirrors PTPClockSourceKind
    // (PTPArbitrator.h) without this header needing to include CoreAudio.
    std::string clockSourceKind{"internal"};

    // Device UID (kAudioDevicePropertyDeviceUID), not AudioDeviceID: UIDs
    // are stable across reboots/replugs, AudioDeviceIDs aren't. Empty
    // unless clockSourceKind == "localAudioDevice".
    std::string lockToDeviceUID;

    // --- The dataset this clock announces, and the rate it announces at ---
    //
    // Every one of these is a number IEEE 1588 puts in the Announce or in
    // the header, and every PTP implementation worth using lets the
    // installation set it: the RAVENNA driver has $.network.PTP.Prio1,
    // .Prio2, .Class, .Accuracy, .Announce, .Sync and .DelayMechanism, and
    // until now this driver had them compiled in.
    //
    // The defaults are exactly what the code used before they were
    // settable, so a file written by an older build, or no file at all,
    // behaves as it did.

    /// BMCA's first tiebreak. Lower wins; 128 is the 1588 default.
    int priority1{128};
    /// BMCA's tiebreak after the clock quality. Lower wins.
    int priority2{128};
    /// clockClass. 248 is "default, not traceable to a primary reference",
    /// which is what an internal oscillator is.
    int clockClass{248};
    /// clockAccuracy. 0xFE is "unknown", the honest answer unless the
    /// clock is disciplined by something that knows better.
    int clockAccuracy{0xFE};

    /// How often this port sends Sync when it is the master, and Announce.
    /// 125 ms and 1 s are the media profile's values and what PTPMaster
    /// used before this was settable.
    int syncIntervalMs{125};
    int announceIntervalMs{1000};

    /// How often this port asks for the path delay when it is a slave.
    int delayReqIntervalMs{1000};

    /// "e2e" (delay request-response, what AES67 uses) or "p2p" (peer
    /// delay). Anything else is read as "e2e".
    std::string delayMechanism{"e2e"};

    /// DSCP to mark this port's PTP with, or -1 for unmarked, which is
    /// what the stack sends and what this driver sent before it could be
    /// set.
    int dscp{-1};
};

class PTPMasterSettingsManager {
public:
    PTPMasterSettingsManager();
    ~PTPMasterSettingsManager();

    /// Returns settings from disk, or PTPMasterSettings{} (masterCapable
    /// false — exactly the old slave-only behavior) if no settings file
    /// exists yet or it's unreadable.
    PTPMasterSettings load();

    bool save(const PTPMasterSettings& settings);

    std::string getConfigPath() const;

private:
    std::vector<std::string> getConfigSearchPaths();
    std::string findExistingConfig();
    bool ensureConfigDirectoryExists();

    std::string configPath_;
    static constexpr const char* kDefaultConfigFile = "ptp_master.json";
};

} // namespace AES67
