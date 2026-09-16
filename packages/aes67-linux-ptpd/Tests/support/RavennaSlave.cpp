//
// RavennaSlave.cpp
// AES67 PTP daemon
//
// The RAVENNA module's own PTP receive path, compiled and called.
//
// This used to mirror process_PTP_packet: the same tests on the same offsets,
// written out in C++ beside a comment naming the lines they came from. The
// mirror was faithful, and a comment is what guaranteed it -- an upstream
// change would have left it quietly wrong rather than failing anything.
//
// So the module's own file is compiled instead (external/aes67-linux-daemon/3rdparty/ravenna-alsa-lkm/
// driver/PTP.c, with Tests/support/lkm standing in for the handful of kernel
// names it uses) and this is the shim around it: a synthetic Ethernet, IP and
// UDP frame around each PTP payload, because that is what the module reads
// out of a netfilter hook, and its own status read back afterwards.
//

#include "support/RavennaSlave.h"

extern "C" {
#include "PTP.h"
#include "EtherTubeNetfilter.h"
}

#include <cstring>
#include <vector>

namespace {

/// The counter clock PTP.c asks for, in nanoseconds, the unit the kernel's
/// own get_clock_time uses. Still unless a test moves it, so a run is
/// repeatable.
uint64_t g_counterTime = 0;

/// What the module's frame offsets assume in front of the PTP payload:
/// 14 bytes of Ethernet, 20 of IPv4, 8 of UDP.
constexpr size_t kEthernet = 14;
constexpr size_t kIPv4 = 20;
constexpr size_t kUDP = 8;
constexpr size_t kHeaders = kEthernet + kIPv4 + kUDP;

void writeBE16(uint8_t* at, uint16_t value) {
    at[0] = static_cast<uint8_t>(value >> 8);
    at[1] = static_cast<uint8_t>(value & 0xFF);
}

/// A PTP payload wrapped the way it arrives at the module.
std::vector<uint8_t> frameAround(const uint8_t* payload, size_t length, uint16_t destinationPort) {
    std::vector<uint8_t> frame(kHeaders + length, 0);
    writeBE16(&frame[12], 0x0800);                       // EtherType IPv4
    frame[kEthernet] = 0x45;                             // IPv4, 20-byte header
    writeBE16(&frame[kEthernet + 2], static_cast<uint16_t>(kIPv4 + kUDP + length));
    frame[kEthernet + 9] = 17;                           // UDP
    writeBE16(&frame[kEthernet + kIPv4], 319);           // source port
    writeBE16(&frame[kEthernet + kIPv4 + 2], destinationPort);
    writeBE16(&frame[kEthernet + kIPv4 + 4], static_cast<uint16_t>(kUDP + length));
    if (length > 0) std::memcpy(&frame[kHeaders], payload, length);
    return frame;
}

} // namespace

extern "C" {

// What PTP.c needs from the rest of the module. None of it decides anything
// about a PTP packet: it is the clock, the checksums and the transmit path.
unsigned long long MTAL_LK_GetCounterTime(void) { return g_counterTime; }
uint16_t MTAL_ComputeChecksum(void*, uint16_t) { return 0; }
uint16_t MTAL_ComputeUDPChecksum(void*, uint16_t, uint16_t*, uint16_t*) { return 0; }
uint64_t CW_ll_modulo(uint64_t dividend, uint64_t divisor) {
    return divisor ? dividend % divisor : 0;
}
void get_clock_time(uint64_t* clock_time) { if (clock_time) *clock_time = g_counterTime; }
void set_base_period(uint64_t) {}

// The netfilter half the module talks to. A slave under test sends nothing
// and its link is up: what is being exercised is what it does with a packet.
int EnablePTPTimeStamping(TEtherTubeNetfilter*, int, uint16_t) { return 1; }
int GetMACAddress(TEtherTubeNetfilter*, unsigned char* address, uint32_t length) {
    if (address != nullptr) std::memset(address, 0, length);
    return 1;
}
int IsLinkUp(TEtherTubeNetfilter*) { return 1; }
int SendRawPacket(TEtherTubeNetfilter*, void*, uint32_t) { return 1; }

} // extern "C"

namespace AES67::LinuxPtpd::Tests {

SlaveVerdict feed(SlaveState& state, const uint8_t* data, size_t length,
                  uint16_t destinationPort) {
    SlaveVerdict verdict;

    // One clock per state, made on first use and carried in the state itself.
    if (state.impl == nullptr) {
        auto* clock = new TClock_PTP{};
        // The module's own init, not a hand-filled struct: it allocates the
        // lock the packet path takes and sets the state the rest of it
        // assumes. A zeroed struct segfaults on the first Sync.
        static TEtherTubeNetfilter netfilter{};
        static clock_ptp_ops callbacks{};
        callbacks.user = nullptr;
        callbacks.GetIPAddress = [](void*) -> uint32_t { return 0x7F000001; };
        callbacks.AudioFrameTIC = [](void*) {};
        init_ptp(clock, &netfilter, &callbacks);
        // What a running module has and a test has not: the audio frame timer.
        // Without it process_PTP_packet refuses every packet at its first line.
        clock->m_bAudioFrameTICTimerStarted = true;
        state.impl = clock;
    }
    auto* clock = static_cast<TClock_PTP*>(state.impl);
    clock->m_ui8PTPClockDomain = state.configuredDomain;
    clock->m_PTPConfig.ui8Domain = state.configuredDomain;

    const uint64_t masterBefore = clock->m_ui64PTPMaster_ClockIdentity;
    const uint16_t lockCounterBefore = clock->m_usPTPLockCounter;
    const std::vector<uint8_t> frame = frameAround(data, length, destinationPort);

    const EDispatchResult result = process_PTP_packet(
        clock, reinterpret_cast<TUDPPacketBase*>(const_cast<uint8_t*>(frame.data())),
        static_cast<uint32_t>(frame.size()));

    verdict.used = (result != DR_PACKET_NOT_USED);
    verdict.elected = clock->m_ui64PTPMaster_ClockIdentity != 0 &&
                      clock->m_ui64PTPMaster_ClockIdentity != masterBefore;
    // ResetPTPLock puts the counter back up to PTP_LOCK_HYSTERESIS; nothing
    // else in the packet path ever raises it.
    verdict.lockReset = clock->m_usPTPLockCounter > lockCounterBefore;

    state.masterClockIdentity = clock->m_ui64PTPMaster_ClockIdentity;
    state.grandmasterIdentity = clock->m_ui64PTPMaster_GMID;
    state.lastSyncSequenceId = clock->m_wLastSyncSequenceId;
    state.syncArrivalTime = clock->m_ui64T2;
    state.locked = clock->m_usPTPLockCounter == 0;
    return verdict;
}

void setCounterTime(uint64_t timeNs) { g_counterTime = timeNs; }

/// The two-step flag of a Sync, PTP byte 6 bit 1 (IEEE 1588 SS 13.3.2.6). A
/// one-step Sync carries its own departure time and no Follow_Up, and this
/// module refuses it -- which is a property of the message, readable without
/// a clock.
bool syncIsTwoStep(const uint8_t* data) {
    return data != nullptr && (data[6] & 0x02) != 0;
}

SlaveState::~SlaveState() { destroy(*this); }

void destroy(SlaveState& state) {
    if (state.impl != nullptr) destroy_ptp(static_cast<TClock_PTP*>(state.impl));
    delete static_cast<TClock_PTP*>(state.impl);
    state.impl = nullptr;
}

} // namespace AES67::LinuxPtpd::Tests
