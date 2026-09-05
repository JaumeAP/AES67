//
// PTPServiceProtocol.h
// AES67 macOS Driver
//
// The wire contract between the privileged PTP daemon and everything that
// wants its measurements.
//
// Why a separate process: not privilege. On macOS 26.6.2 an unprivileged
// process binds UDP 319 and 320 without trouble -- measured, along with 80
// and 443, as uid 501 -- so the long-standing claim in this repository that
// those ports need root is simply wrong on current macOS. What the daemon
// buys is separation: one PTP engine per host instead of one per process,
// alive across plugin reloads and coreaudiod restarts, readable by the
// plugin and the Manager app at once, and testable on its own. The plugin
// keeps its in-process path for when no daemon is running.
//
// The status is a fixed-layout struct rather than a serialisation format on
// purpose: both ends are built from this header in one repository, the
// message is a few dozen bytes, and a version field is enough to refuse a
// mismatch outright.
//

#ifndef PTP_SERVICE_PROTOCOL_H
#define PTP_SERVICE_PROTOCOL_H

#include <cstdint>

namespace AES67 {

// Where the daemon listens. The socket is created 0666 so a reader running as
// another user (coreaudiod runs as _coreaudiod) can connect; nothing is ever
// read FROM a client, so a hostile connection can only waste a descriptor.
constexpr const char* kPTPServiceSocketPath = "/var/run/aes67ptpd.sock";

constexpr uint32_t kPTPServiceMagic = 0x41503750;   // "AP7P"
constexpr uint32_t kPTPServiceVersion = 1;

// Published on every change and at least every kPTPServiceHeartbeatMs, so a
// reader can tell "locked and quiet" from "daemon gone" by age alone.
constexpr int kPTPServiceHeartbeatMs = 250;

// A reader treats anything older than this as no measurement at all.
constexpr int kPTPServiceStaleMs = 2000;

#pragma pack(push, 1)
struct PTPServiceStatus {
    uint32_t magic = kPTPServiceMagic;
    uint32_t version = kPTPServiceVersion;
    uint32_t length = sizeof(PTPServiceStatus);
    uint32_t sequence = 0;          // increments per publication

    uint8_t locked = 0;
    uint8_t clockClass = 255;
    uint8_t clockAccuracy = 0xFE;
    uint8_t domain = 0;

    int64_t offsetNs = 0;           // slave clock minus master clock
    int64_t pathDelayNs = 0;
    double frequencyDriftPpb = 0.0;

    // Monotonic milliseconds on the daemon's clock when this was published,
    // for ordering only; staleness is judged by the reader's own arrival
    // time, since the two processes do not share an epoch.
    uint64_t publishedAtMs = 0;

    uint8_t grandmasterIdentity[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    uint32_t syncCount = 0;
    uint32_t delayCount = 0;
};
#pragma pack(pop)

static_assert(sizeof(PTPServiceStatus) == 68,
              "PTPServiceStatus is a wire struct: changing its size is a "
              "protocol change and needs kPTPServiceVersion bumped");

}  // namespace AES67

#endif  // PTP_SERVICE_PROTOCOL_H
