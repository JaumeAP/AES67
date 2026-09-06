#pragma once

#include <ctime>
#include <limits>

#include "ptp-servo.h"
// MasterDataset, isBetterMaster, sameSource and MAX_STEPS_REMOVED: the
// dataset comparison of 1588 §9.3, on its own so that the AES67 macOS
// driver's core can include it without the board coming with it.
#include "ptp-bmca.h"

// How much this library says on the serial port: 0 silent, 1 the normal
// messages, 2 and above the detail of every message.
//
// A build flag and not a call, because a level decided at run time keeps
// every switched-off Serial.printf inside the binary -- some 1.5 KB of
// flash and 1 KB of RAM for nothing. On PlatformIO: build_flags =
// -DT41PTP_LOGGING_LEVEL=1.
#ifndef T41PTP_LOGGING_LEVEL
#define T41PTP_LOGGING_LEVEL 0
#endif

using NanoTime = int64_t;

// The seconds field of a timespec, which is NOT the same width everywhere:
// 32 bits on the Teensy and 64 on the development machine. Writing a
// NanoTime's seconds straight into it wrapped silently on the board, and a
// wrapped second puts the clock in another century.
using TvSec = decltype(timespec::tv_sec);

// Holds a seconds count inside what the target type can carry. It clamps
// rather than wraps: a value stuck at the edge is visible to the drift
// guard downstream, a wrapped one looks like a legitimate time.
//
// It takes the type as a parameter so that the host tests can exercise the
// 32-bit case, which is the board's and never the development machine's.
template <typename T>
inline T clampSeconds(NanoTime s)
{
    constexpr NanoTime hi = static_cast<NanoTime>(std::numeric_limits<T>::max());
    constexpr NanoTime lo = static_cast<NanoTime>(std::numeric_limits<T>::min());
    if (s > hi)
    {
        return std::numeric_limits<T>::max();
    }
    if (s < lo)
    {
        return std::numeric_limits<T>::min();
    }
    return static_cast<T>(s);
}

inline TvSec clampToTvSec(NanoTime s)
{
    return clampSeconds<TvSec>(s);
}

constexpr NanoTime NS_PER_S = 1000*1000*1000;

// Minimum lengths of a PTPv2 message, in bytes.
//
// parsePTPMessage was given the size of the datagram and never used it:
// `size` appeared exactly once in the file, in the signature. The
// parsers read up to buf[51], and the buffer they are handed is exactly
// as long as the datagram received, so a one-byte UDP packet to the PTP
// port made them read 51 bytes of adjacent stack -- and those bytes
// became timestamps fed to the clock servo.
//
// PTP is open multicast with no authentication by design, so any device
// on the network could send that packet.
constexpr int PTP_HEADER_LEN = 34;          // common header
constexpr int PTP_SYNC_LEN = 44;            // Sync, Delay_Req, Follow_Up
constexpr int PTP_DELAY_RESP_LEN = 54;      // Delay_Resp, Pdelay_Resp_Follow_Up
constexpr int PTP_ANNOUNCE_LEN = 64;        // Announce

// The largest seconds value that can be multiplied by a thousand
// million, have a whole second of nanoseconds added to it, and still
// fit in an int64_t.
//
// The seconds field on the wire is 48 bits, so up to
// 281474976710655, and multiplied by NS_PER_S that is 2.8e23 against
// the 9.2e18 an int64_t holds. Overflowing a signed integer is
// undefined behaviour, not a wrong value, and the value is chosen by
// whoever sends the packet.
//
// One lower than the largest multiple that fits, because the
// nanoseconds are added afterwards: at 9223372036 the product leaves
// 854775807 ns of headroom and the field carries up to 999999999, so
// the clamp handed the addition an overflow instead of preventing one.
constexpr NanoTime MAX_SAFE_SECONDS = 9223372035;

// Addition that stops at the edges of the type instead of wrapping.
//
// Every timestamp read off the wire is added to a correctionField read
// off the same wire, and both are chosen by whoever sends the packet. A
// timestamp clamped to the top of the range by MAX_SAFE_SECONDS, plus a
// correctionField of a day and a half -- which is what the 48 bits left
// of that field after the scaling can say -- overflowed the addition
// that the clamp had just made safe.
inline NanoTime addSaturating(NanoTime a, NanoTime b)
{
    if (b > 0 && a > std::numeric_limits<NanoTime>::max() - b)
    {
        return std::numeric_limits<NanoTime>::max();
    }
    if (b < 0 && a < std::numeric_limits<NanoTime>::min() - b)
    {
        return std::numeric_limits<NanoTime>::min();
    }
    return a + b;
}

// The portNumber this implementation announces in every sourcePortIdentity
// it writes, and the one it expects to see echoed back in the
// requestingPortIdentity of a response.
constexpr uint16_t PORT_NUMBER = 1;

// The range 1588 allows for a logMessageInterval carrying a real
// interval. A Delay_Resp naming anything outside it is not describing a
// rate this implementation should follow.
constexpr int8_t MIN_LOG_INTERVAL = -7;
constexpr int8_t MAX_LOG_INTERVAL = 7;

// The most this library will ever ask the timer to change its rate by,
// in nanoseconds per second: 100 ppm, which is already three times the
// worst a crystal drifts and the same figure the controller uses to
// decide a master is not worth following.
//
// Nothing bounded the correction before. The integral term accumulated
// the offset without limit -- a stuck offset of a microsecond grows it
// by a millisecond per second of run time -- and the sum went to the
// hardware as it came out, so a fault that held the offset away from
// zero ended up commanding a rate no oscillator can produce, and the
// clock could not come back.
constexpr double MAX_FREQ_ADJUST_NSPS = 100000.0;

// How many Sync intervals may pass with nothing arriving before the
// master counts as gone. Three is what 1588 uses for its receipt
// timeouts.
constexpr int SYNC_RECEIPT_TIMEOUT_INTERVALS = 3;

// How long an external reference keeps this clock after its last edge,
// in milliseconds.
//
// A clock wired to a house reference is disciplined by the pin, not by
// what the network says, so while that reference is arriving the Sync of
// the master this port follows corrects nothing. Three seconds is the
// three intervals 1588 uses for its own receipt timeouts, at the one per
// second the reference comes in at. Past it the reference is gone and
// the network takes the clock back.
constexpr unsigned long EXTERNAL_REFERENCE_TIMEOUT_MS = 3000;

// The most delay measurements the minimum filter keeps.
constexpr int MAX_DELAY_FILTER_SAMPLES = 8;

// How long a Pdelay_Resp waits for its own transmit timestamp before the
// exchange is given up, in milliseconds.
//
// The wait used to happen inside the parser, spinning for up to a
// millisecond with the socket queue held: a peer sending Pdelay_Req as
// fast as it could kept this device inside update() and made it answer
// two frames for each one. The response goes out during the parse, and
// the timestamp is picked up later, from update(), without spinning.
constexpr unsigned long PEER_FOLLOW_UP_TIMEOUT_MS = 10;

// How long a Sync waits for its own transmit timestamp before the
// Follow_Up is given up, in milliseconds.
//
// The wait used to happen inside syncMessage(), spinning for up to a
// millisecond with nothing else running: at eight Sync a
// second a hardware that stopped posting timestamps cost eight
// milliseconds of loop() every second, and every one of those
// milliseconds is packets not being drained. The Sync goes out and the
// timestamp is picked up later, from update(), the way the peer-delay
// answer already did.
//
// It also waits better: a timestamp that takes longer than the spin
// allowed -- a full frame queued ahead of the Sync is 120 us at 100 Mbit,
// and several are not unusual on a board that also sends audio -- now
// produces its Follow_Up instead of being abandoned.
constexpr unsigned long SYNC_FOLLOW_UP_TIMEOUT_MS = 10;

// How long a Delay_Req waits for its own transmit timestamp -- its T3 --
// before the exchange is given up, in milliseconds.
//
// The last of the three waits to stop spinning. It ran inside update(),
// so a hardware that stopped posting timestamps cost a millisecond of
// every pass that sent a request. T3 is now collected where the other two
// departure times are, and the same rule holds: only one message at a
// time may be owed the register.
constexpr unsigned long DELAY_REQUEST_TIMEOUT_MS = 10;

// How many Announce intervals may pass before the master that was chosen
// counts as gone and the choice is made again.
constexpr int ANNOUNCE_RECEIPT_TIMEOUT_INTERVALS = 3;


// Where this port stands, in the terms 1588 uses.
//
// A port configured as master used to send whatever else it heard, so two
// of them on one segment both drove the slaves and neither stepped aside.
// The dataset comparison decides which of the two announces and which one
// falls silent.
enum class PortState
{
    Initializing,  // begin() has not run
    Listening,     // an Announce has been heard but no master chosen yet
    Master,        // this port is the one sending Sync and Announce
    Passive,       // a better master is on the segment; this port is quiet
    Slave          // following the master that was chosen
};

class PTPBase
{
public:
    PTPBase(bool master_, bool slave_, bool p2p_);

    // The transports own resources -- l3 holds four sockets -- and are
    // used through this base, so destruction has to reach them.
    virtual ~PTPBase() = default;

    void begin();

    // Undoes begin(): closes what initSockets() opened and drops the
    // state, so a later begin() starts a fresh port.
    //
    // There was no way back before: initialised was set once and never
    // cleared, so a port taken down with Ethernet.end() could not be
    // brought up again -- begin() returned early and left the library
    // talking to sockets that were gone.
    void end();

    void update();
    void reset();
    void setKi(double val);
    void setKp(double val);

    // Follow only this clock identity, whatever else announces itself.
    //
    // PTP has no authentication, so the best master algorithm believes
    // whatever arrives: an Announce claiming priority1 of zero takes the
    // clock over. Pinning the identity is the only defence a receiver
    // has. Eight bytes; a null pointer goes back to choosing.
    void setMasterIdentity(const uint8_t *identity);
    void clearMasterIdentity();

    // Gain on the frequency-mode correction. One means the whole measured
    // rate error is taken in a single step, which is what this did before
    // the gain existed; less than one damps the approach.
    void setKf(double val);

    // The thresholds the servo decides by, in the units of what they
    // measure. They were literals inside the controller, so a board
    // whose oscillator, network or idea of "locked" differed from the
    // ones these numbers were chosen against had no way to say so.
    //
    // maxDrift: above this the measurement is refused outright -- a
    // master implying more than a crystal can do has been stepped.
    // freqModeThreshold: above this the servo corrects frequency alone.
    // coarseModeThreshold: above this offset it steps the clock.
    // lockThreshold: inside this offset a measurement counts as locked.
    void setMaxDriftNsps(double val);
    void setFreqModeThresholdNsps(double val);
    void setCoarseModeThresholdNs(NanoTime val);
    void setLockThresholdNs(NanoTime val);

    // How many of the last delay measurements the minimum filter keeps,
    // from 1 to MAX_DELAY_FILTER_SAMPLES. One is no filter at all.
    //
    // The delay used to come from a single Delay_Req exchange, so every
    // bit of queuing on the way through a switch went straight into the
    // offset. The minimum of a window is the sample that queued least,
    // which is the closest thing to the path on its own.
    void setDelayFilterLength(uint8_t val);

    // The compensation applied to every timestamp this device takes or
    // publishes, in nanoseconds, and the one the peer-delay offset uses.
    //
    // Both were literals in the source -- -200 for the hardware and a
    // bare 500 in the peer-delay branch -- so the only way to match a
    // different board or PHY was to edit the library. The defaults are
    // the values that were hardcoded.
    void setTimestampOffset(NanoTime val);
    void setPeerOffsetCorrection(NanoTime val);
    NanoTime getOffset() const;
    NanoTime getDelay() const;
    void syncMessage();
    void announceMessage();
    void ppsInterruptTriggered(NanoTime pps_ts, NanoTime local_ts);
    int getLockCount() const;

    // Whether a Sync has arrived recently enough for the lock to mean
    // anything. A master that stops sending used to leave the last lock
    // standing for ever: nothing on the slave side noticed the silence.
    bool isSyncReceiptValid() const { return syncReceiptValid; }

    // Where this port stands. See PortState.
    PortState getPortState() const { return portState; }

    // This port's clock identity: the MAC mapped to EUI-64, eight bytes.
    // Anything that has to name this clock to something outside -- an
    // inventory, a log, a registry -- needs it, and it was reachable only
    // from inside.
    const uint8_t *getClockIdentity() const { return clockID; }

    // Whether the dataset comparison may take this port off the air.
    //
    // On by default, which is what 1588 asks for: a master that hears a
    // better one stops sending and becomes Passive. Turning it off keeps
    // a configured master sending whatever else is on the segment, which
    // is what this library did before the state machine existed.
    //
    // A port configured master AND slave has no comparison to decide
    // from once this is off, so the configured roles decide instead: it
    // follows whatever master has been chosen, however poor, and
    // announces only while none has been.
    void setBmcaEnabled(bool val);
    bool getBmcaEnabled() const { return bmcaEnabled; }

    // Whether an Announce has been heard and a master chosen.
    //
    // Announce was sent but never received: nothing parsed message type
    // 11, so there was no dataset to compare, no best master to choose
    // and no check on where a Sync came from. Two masters on a segment
    // both drove this clock, one exchange each, and any device at all
    // could pretend to be one.
    //
    // Until the first Announce arrives, a Sync from anywhere is still
    // followed: a setup whose master sends no Announce keeps working.
    bool hasSelectedMaster() const { return masterSelected; }
    const MasterDataset &getSelectedMaster() const { return selectedMaster; }

    // Announce dataset and advertised message intervals.
    //
    // The defaults below are exactly the values this library hardcoded before
    // these became configurable, so leaving them alone changes nothing.
    //
    // A grandmaster disciplined by an external reference needs to say so:
    // with clockClass fixed at 248 and timeSource at INTERNAL_OSCILLATOR it
    // announces itself as free-running whatever its actual lock state, which
    // both misinforms the BMCA and makes it impossible for receivers to tell
    // a locked clock from one that has lost its reference.
    void setClockClass(uint8_t val);
    void setClockAccuracy(uint8_t val);
    void setOffsetScaledLogVariance(uint16_t val);
    void setPriority1(uint8_t val);
    void setPriority2(uint8_t val);
    void setTimeSource(uint8_t val);
    void setCurrentUtcOffset(int16_t val);

    // The PTP domain this port belongs to. Messages from any other domain
    // are ignored, and every message sent carries this number.
    //
    // The number was hardcoded to zero on the way out and never looked at
    // on the way in, so a second domain sharing the multicast group -- the
    // usual way of running two independent PTP networks on one wire -- fed
    // its Sync messages straight into this clock's servo.
    void setDomainNumber(uint8_t val);

    // currentUtcOffsetValid flag. Announcing an offset while leaving this
    // false says "here is a number, do not rely on it", which is the right
    // thing for a master with no traceable source of absolute time. Only set
    // it when the offset really is known good.
    void setUtcOffsetValid(bool val);

    // The rest of the Announce flags, and how far this clock sits from the
    // grandmaster. None of them could be announced before: the flag octet
    // carried PTPTimescale and nothing else, and stepsRemoved was left at
    // zero, so a boundary clock had no way to say it was not the source.
    //
    // leap59 and leap61 say the current UTC day is a second short or a
    // second long. timeTraceable and frequencyTraceable say the time and
    // the rate come from a primary reference.
    void setLeap59(bool val);
    void setLeap61(bool val);
    void setTimeTraceable(bool val);
    void setFrequencyTraceable(bool val);
    void setStepsRemoved(uint16_t val);

    // Every parameter above, read back. They could be written and not
    // read, so a sketch had no way of telling what it had configured.
    uint8_t getClockClass() const { return clockClass; }
    uint8_t getClockAccuracy() const { return clockAccuracy; }
    uint16_t getOffsetScaledLogVariance() const { return offsetScaledLogVariance; }
    uint8_t getPriority1() const { return priority1; }
    uint8_t getPriority2() const { return priority2; }
    uint8_t getTimeSource() const { return timeSource; }
    uint8_t getDomainNumber() const { return domainNumber; }
    uint8_t getMajorSdoId() const { return majorSdoId; }
    int16_t getCurrentUtcOffset() const { return currentUtcOffset; }
    bool getUtcOffsetValid() const { return utcOffsetValid; }
    bool getLeap59() const { return leap59; }
    bool getLeap61() const { return leap61; }
    bool getTimeTraceable() const { return timeTraceable; }
    bool getFrequencyTraceable() const { return frequencyTraceable; }
    uint16_t getStepsRemoved() const { return stepsRemoved; }
    int8_t getLogSyncInterval() const { return logSyncInterval; }
    int8_t getLogAnnounceInterval() const { return logAnnounceInterval; }
    int8_t getLogMinDelayReqInterval() const { return logMinDelayReqIntervalAnnounced; }
    double getKp() const { return KP; }
    double getKi() const { return KI; }
    double getKf() const { return KF; }
    double getMaxDriftNsps() const { return maxDriftNsps; }
    double getFreqModeThresholdNsps() const { return freqModeThresholdNsps; }
    NanoTime getCoarseModeThresholdNs() const { return coarseModeThresholdNs; }
    NanoTime getLockThresholdNs() const { return lockThresholdNs; }
    uint8_t getDelayFilterLength() const { return delayFilterLength; }
    NanoTime getTimestampOffset() const { return timestampOffset; }
    NanoTime getPeerOffsetCorrection() const { return peerOffsetCorrection; }
    NanoTime getPpsOffset() const { return ppsOffset; }

    // The parameter sets a profile fixes.
    //
    // Every value below can already be set one at a time; a profile is the
    // combination a given ecosystem expects, in one call, so that pointing
    // this port at RAVENNA gear or at an 802.1AS segment is a decision
    // rather than six numbers to remember. It sets ONLY the numbers: the
    // delay mechanism is chosen when the object is built (`p2p`), and the
    // transport by which class is used -- l3PTP for AES67, l2PTP for
    // 802.1AS.
    enum class Profile
    {
        // IEEE 1588-2008 default profile: Sync every second, Announce
        // every two, Delay_Req every second.
        Default1588,
        // The media profile AES67 and RAVENNA gear runs: Sync eight times
        // a second, Announce once, Delay_Req eight times, domain 0.
        AES67Media,
        // 802.1AS: Sync eight times a second, Announce once, Pdelay_Req
        // once, and majorSdoId 1, which is what makes a receiver that
        // follows that profile accept the traffic at all.
        GPTP,
    };

    // Applies a profile's numbers. Priority1, priority2 and the clock
    // quality are left alone: they describe THIS clock, not the ecosystem
    // it is speaking to.
    void applyProfile(Profile profile);

    // logMessageInterval as advertised in the PTP header. These only set what
    // is announced -- the caller still has to send at the matching rate, or
    // the announcement lies.
    void setLogSyncInterval(int8_t val);
    void setLogAnnounceInterval(int8_t val);

    // logMinDelayReqInterval, the rate this master asks its slaves to send
    // Delay_Req at. It was written as zero and could not be changed, so
    // every slave was told one per second whatever the master wanted.
    //
    // Peer to peer it is also the rate this port sends its own Pdelay_Req
    // at: there is no master to ask, the answer names no rate, and a
    // number a peer-delay port could only announce and never act on is
    // not a rate at all.
    void setLogMinDelayReqInterval(int8_t val);

    // majorSdoId, the top nibble of the first octet. Zero is the default
    // profile; 802.1AS wants one, and a receiver that follows it drops
    // everything else. Neither written nor checked before.
    void setMajorSdoId(uint8_t val);

    // The compensation applied to the PPS input path.
    //
    // The network path has had setTimestampOffset() since the constants
    // came out of the source; this one was simply absent, so the two ways
    // of measuring the same clock used different arithmetic.
    void setPpsOffset(NanoTime val);

protected:
    virtual void initSockets()=0;
    virtual void closeSockets()=0;
    virtual void updateSockets()=0;
    // generalMessage picks the general port over the event one;
    // peerAddress picks the peer-delay multicast group over the default
    // one. Peer-delay messages went to the default group before, which is
    // not where any other implementation listens for them.
    virtual void sendPTPMessage(const uint8_t *buf, int size, bool generalMessage,
                                bool peerAddress)=0;
    
    void parsePTPMessage(const uint8_t *buf, int size, const timespec &recv_ts);

public:
    // How many sends have failed since boot.
    //
    // A grandmaster that cannot get its Sync messages out is a silent
    // problem without this: a log line only helps if somebody is
    // watching the serial port.
    uint32_t getTxFailureCount() const { return txFailureCount; }

    // How many sockets or address filters could not be opened since boot.
    //
    // The result of every beginMulticast() was thrown away, so a port that
    // failed to join the group came up silent with nothing to say so --
    // the receive side of the problem getTxFailureCount() reports.
    //
    // Counted since boot, like the transmit failures: reset() used to
    // clear this one and not that one, so the two numbers a sketch logs
    // side by side covered different spans of time and neither said which.
    uint32_t getBindFailureCount() const { return bindFailureCount; }

protected:
    uint32_t txFailureCount = 0;
    uint32_t bindFailureCount = 0;

    bool master;
    bool slave;
    bool p2p;
    
private:
	void setT1(NanoTime ts);
	void setT2(NanoTime ts);
	void setT3(NanoTime ts);
	void setT4(NanoTime ts);
    // Whether this port is currently following a master. The role flags
    // say what it was configured to be; this says what it is doing.
    bool followingMaster() const { return portState == PortState::Slave; }

    // Whether an external reference is disciplining this clock right now,
    // and so whether the Sync is allowed anywhere near it.
    bool externalReferenceLive() const;

    void parseAnnounceMessage(const uint8_t *buf);
    MasterDataset ownDataset() const;
    void updatePortState();

    // The phases of update(), in the order it calls them. Split out of a
    // hundred-and-forty-line function: the order matters and the reasons are
    // on each one.
    void serviceDelayExchange();
    void serviceDelayRequestPacing();
    void serviceSyncPair();
    void serviceAnnounceTimeout();
    void serviceSyncReceipt();
    void serviceExternalReference();
    bool fromSelectedMaster(const uint8_t *buf) const;
    void parseSyncMessage(const uint8_t *buf, const timespec &recv_ts);
    void parseFollowUpMessage(const uint8_t *buf);
    bool requestingPortIdentityMatches(const uint8_t *buf) const;
    void parseDelayResponseMessage(const uint8_t *buf, const timespec &recv_ts);
    void parseDelayResponseFollowUpMessage(const uint8_t *buf);
    void parseDelayRequestMessage(const uint8_t *buf, const timespec &recv_ts);
    void parsePeerDelayRequestMessage(const uint8_t *buf, const timespec &recv_ts);
    
    bool delayRequestMessage();
    void armTxTimestamp();
    unsigned long delayRequestIntervalMillis() const;
    void scheduleNextDelayRequest();
    void peerDelayResponseMessage(const uint8_t *request_buf, uint16_t sequenceID,
                                  const timespec &request_recv_ts);
    void servicePeerFollowUp();
    void serviceSyncFollowUp();
    void serviceDelayRequestTimestamp();
    // All three, in one call: whichever message is owed the transmit
    // timestamp takes it. Run before anything that could send, and again
    // after anything that did.
    void serviceTxTimestamps();
    // Gives up the request in flight, and with it the half of an exchange
    // that would otherwise wait to be paired with the next one's.
    void abandonDelayRequest();
    // Whether the one transmit timestamp the hardware holds is already
    // owed to something. Only one message at a time may be waiting for
    // it: anything else sent meanwhile would take it.
    bool txTimestampOwed() const
    {
        return peerResponsePending || syncFollowUpPending || delayRequestPending;
    }
    void followUpMessage(const timespec &send_ts, uint16_t sequenceID);
    void delayResponseMessage(const uint8_t *request_buf, uint16_t sequenceID, const timespec &request_recv_ts);
    void initPTPMessage(uint8_t *buf, const uint16_t messageLength, const uint8_t messageType, const uint16_t sequenceID, const uint8_t controlField);
    // The feedforward term needs the step between this measurement and
    // the one before it. Passed in rather than read from t1/t1last, which
    // belong to the Sync path: the external reference keeps its own pair
    // and used to borrow those, which is why it could not be told apart
    // from a Sync in the first place.
    // Answers what the clock was moved by, which is what the local
    // timestamp of this measurement has to move by so that the next step
    // is measured against where the clock now is. Zero in every mode that
    // did not correct the offset. Each caller owns a different local
    // timestamp, so applying it is the caller's: this used to write t2
    // itself, which is the Sync path's and nobody else's.
    NanoTime updateController(NanoTime refDiff, NanoTime localDiff);
    void updateDelay();
    void resetServo();
    void adjustFrequency(double nsps);
    bool recordDelaySample(NanoTime delay);
    NanoTime filteredDelay() const;
    static unsigned long logIntervalToMillis(int8_t logInterval);
    unsigned long syncReceiptTimeoutMillis() const;
    void syncReceiptLost();
    void updateTimer();
    // The pair the reference last delivered and the one before it, passed
    // in rather than read from the members: the ISR writes those, and a
    // read of four 64-bit fields that the interrupt can land in the
    // middle of is a pair of timestamps from two different edges.
    void updatePPS(NanoTime refNow, NanoTime refLast, NanoTime localNow, NanoTime localLast);

    uint8_t clockID[8];
    bool initialised=false;

    // Whether the hardware timer has been zeroed, which happens once, on
    // the first begin().
    //
    // reset() used to do it every time, and reset() is called by begin()
    // and by both gain setters. A link that bounced, or a gain tuned at
    // run time, therefore threw the grandmaster's clock back to the epoch
    // -- state the caller was entitled to reset, and a clock it was not.
    bool timerZeroed=false;

    uint8_t clockClass = 248;          // default, not traceable
    uint8_t clockAccuracy = 0xfe;      // unknown
    uint16_t offsetScaledLogVariance = 0xffff;  // unknown
    uint8_t priority1 = 128;
    uint8_t priority2 = 128;
    uint8_t timeSource = 0xa0;         // INTERNAL_OSCILLATOR
    int16_t currentUtcOffset = 37;
    uint8_t domainNumber = 0;
    uint8_t majorSdoId = 0;
    int8_t logMinDelayReqIntervalAnnounced = 0;
    NanoTime ppsOffset = 0;
    bool utcOffsetValid = false;
    bool leap59 = false;
    bool leap61 = false;
    bool timeTraceable = false;
    bool frequencyTraceable = false;
    uint16_t stepsRemoved = 0;
    int8_t logSyncInterval = 0;        // 1 s
    int8_t logAnnounceInterval = 0;    // 1 s
    uint16_t delayRequestSequenceID = 0;
    uint16_t syncSequenceID=0;
    uint16_t syncServerSequenceID = 0;
    uint16_t announceServerSequenceID = 0;

    // Where the current Sync exchange stands.
    //
    // These two used to be read off syncSequenceID and followUpSequenceID,
    // which were zeroed once the pair had been seen and so served as both
    // the sequence number and the flag. Kept apart, syncSequenceID now only
    // says which Sync a Follow_Up has to match.
    //
    // syncCycleActive: a Sync has arrived whose Follow_Up has not.
    // syncPairMatched: a matching pair is waiting for its Delay_Req.
    bool syncCycleActive = false;
    bool syncPairMatched = false;

    // The Delay_Req (or Pdelay_Req) still waiting for its answer.
    //
    // Nothing tied a response to a request before: any Delay_Resp carrying
    // our clock identity set T4, whatever its sequence ID. A duplicate, or
    // the late answer to an earlier request, therefore landed in the
    // current exchange and produced a delay measured across two different
    // round trips.
    uint16_t outstandingRequestSequenceID = 0;
    bool requestOutstanding = false;
    bool delayResponseSeen = false;

    // The sourcePortIdentity of the Pdelay_Resp now waiting for its
    // Follow_Up.
    //
    // The two halves of a peer-delay answer were tied to each other by
    // the sequence ID and by our own requestingPortIdentity alone, both
    // of which are in the Pdelay_Resp for anyone on the segment to read.
    // Any other device could therefore send the Follow_Up: T5 came from
    // it, T4 from the real peer, and the link delay was the difference
    // between two clocks that had never met. 1588 clause 11.4.3 pairs
    // them by the responder's own identity, which is what this holds.
    uint8_t peerResponderIdentity[10] = {0};

    // The rate the master asks for its Delay_Req messages at, taken from
    // the logMessageInterval of the Delay_Resp, and when the next one is
    // due.
    //
    // One request went out per Sync before, whatever the master asked
    // for. 1588 wants the interval between requests spread uniformly over
    // twice the announced one, so a room full of slaves does not answer
    // every Sync in the same millisecond.
    int8_t logMinDelayReqInterval = 0;
    unsigned long nextDelayRequestMillis = 0;

    // When the last Sync arrived and the interval the master says it sends
    // them at, which together say when to stop believing the lock.
    PortState portState = PortState::Initializing;
    bool bmcaEnabled = true;

    MasterDataset selectedMaster;
    bool masterSelected = false;

    // Whether any Announce has ever been heard on this port.
    //
    // Before the first one, a Sync from anywhere is followed: a master
    // that sends no Announce is still a master. After it, the choice is
    // required -- releasing a master that has gone quiet must not reopen
    // the port to everyone, which is what dropping the selection alone
    // did.
    bool announceHeard = false;

    // An identity the caller has pinned, if any.
    bool masterIdentityPinned = false;
    uint8_t pinnedMasterIdentity[8] = {0};
    int8_t logAnnounceIntervalFromMaster = 0;
    unsigned long lastAnnounceMillis = 0;

    int8_t logSyncIntervalFromMaster = 0;
    unsigned long lastSyncMillis = 0;
    bool syncReceiptValid = false;
    NanoTime t1=-1;
    NanoTime t1last = -1;
    NanoTime t2 = -1;
    NanoTime t2last = -1;
    NanoTime t2new = -1;
    NanoTime t3 = -1;
    NanoTime t4 = -1;
    NanoTime t5 = -1;
    NanoTime t6 = -1;

    // T1 and T2 as they stood when the Delay_Req went out. The path delay
    // is worked out from those, not from whatever T1 and T2 have become
    // by the time the answer arrives: with the request paced, an answer
    // can now cross a Sync.
    NanoTime requestT1 = -1;
    NanoTime requestT2 = -1;

    // Whether a path delay has ever been measured. Nothing can be said
    // about the offset before that.
    bool delayValid = false;

    bool t1updated=false;
    bool t2updated=false;
    bool t3updated=false;
    bool t4updated=false;
    bool t5updated=false;
    bool t6updated=false;
    bool t1lastvalid=false;
    bool t2lastvalid=false;
    volatile bool ppsupdated=false;

    // The pair the external reference last delivered.
    //
    // Kept apart from t1/t2, which the Sync path owns. Sharing them was
    // safe only for as long as a port configured master could parse no
    // Sync at all: on one configured master and slave together, a Sync
    // arriving between the reference edge and the update() that consumes
    // it replaced both timestamps, and what reached the servo as the
    // reference offset was the master's pair measured with no path delay.
    //
    // Written from ppsInterruptTriggered(), which runs in an interrupt, so
    // volatile: without it the compiler is free to keep the copy it read
    // last time, and update() reads these on every pass through loop().
    volatile NanoTime ppsT1=0;
    volatile NanoTime ppsT2=0;
    volatile NanoTime ppsT1last=0;
    volatile NanoTime ppsT2last=0;

    // When the last reference edge arrived, and whether one ever has.
    volatile unsigned long lastExternalReferenceMillis=0;
    volatile bool externalReferenceSeen=false;

    NanoTime currentOffset=0;
    NanoTime currentDelay=0;
    // What the servo carries between measurements: the frequency term, the
    // integral term and the lock count. The arithmetic that moves them is
    // in ptp-servo.h, where it can be run without a board.
    t41ptp::ServoState servo;
    double KI=0.5;
    double KP=1.0;
    double KF=1.0;

    // The defaults are the numbers that were literals in the controller.
    double maxDriftNsps = 100000.0;
    double freqModeThresholdNsps = 1000.0;
    NanoTime coarseModeThresholdNs = 1000;
    NanoTime lockThresholdNs = 100;

    NanoTime timestampOffset=-200;
    NanoTime peerOffsetCorrection=500;

    // A Pdelay_Resp whose Follow_Up is still waiting for the transmit
    // timestamp. Only one at a time: the hardware holds one timestamp,
    // and anything else sent meanwhile would take it.
    bool peerResponsePending = false;
    uint16_t peerResponseSequenceID = 0;
    uint8_t peerRequesterIdentity[10] = {0};
    unsigned long peerResponseDeadlineMillis = 0;

    // A Sync whose Follow_Up is still waiting for the transmit timestamp,
    // and the sequence ID that Follow_Up has to carry. The same one at a
    // time as the peer-delay answer above, and for the same reason.
    bool syncFollowUpPending = false;
    uint16_t syncFollowUpSequenceID = 0;
    unsigned long syncFollowUpDeadlineMillis = 0;

    // A Delay_Req or Pdelay_Req whose T3 is still waiting for the
    // transmit timestamp. The same one at a time as the two above.
    bool delayRequestPending = false;
    unsigned long delayRequestDeadlineMillis = 0;

    // The window the delay minimum is taken over.
    NanoTime delaySamples[MAX_DELAY_FILTER_SAMPLES] = {0};

    // When each sample was taken. A window of eight is eight measurements
    // however old they are, and with the request paced they can be
    // minutes apart: a path that has changed would otherwise be held down
    // by a sample from before it changed.
    unsigned long delaySampleMillis[MAX_DELAY_FILTER_SAMPLES] = {0};
    uint8_t delayFilterLength=MAX_DELAY_FILTER_SAMPLES;
    uint8_t delaySampleCount=0;
    uint8_t delaySampleIndex=0;
    
};
