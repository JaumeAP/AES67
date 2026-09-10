#pragma once

// Builders for the PTPv2 messages the tests feed to the library, and the
// small helpers that go with them.

#include <cstdint>
#include <ctime>
#include <vector>

#include "ptp/ptp-base.h"

// The hardware offset ptp-base.cpp applies to every timestamp it takes or
// publishes. Kept here as a literal so a change to the library shows up
// as a test failure rather than silently following it.
constexpr NanoTime HW_OFFSET = -200;

void putHeader(std::vector<uint8_t> &buf, uint8_t messageType, uint16_t sequenceID);
void putTimestamp(std::vector<uint8_t> &buf, NanoTime t);

// Writes a sourcePortIdentity: the eight-byte clock identity and port 1.
void putSource(std::vector<uint8_t> &buf, const uint8_t *identity);

// The clockID the library builds in reset(): the EUI-48 to EUI-64 mapping
// of the MAC address, first three octets, FF FE, last three.
void expectedClockID(uint8_t *out);

std::vector<uint8_t> makeSync(uint16_t sequenceID);
std::vector<uint8_t> makeFollowUp(uint16_t sequenceID, NanoTime t1);

// messageType 9 is Delay_Resp, 3 is Pdelay_Resp and 10 is
// Pdelay_Resp_Follow_Up; all three carry a timestamp at offset 34 and the
// requesting port identity at offset 44.
std::vector<uint8_t> makeResponse(uint8_t messageType, uint16_t sequenceID, NanoTime ts,
                                  bool matchingIdentity = true);
std::vector<uint8_t> makePdelayRespFollowUp(uint16_t sequenceID, NanoTime t5);
// Writes a correctionField: nanoseconds, scaled by 2^16 the way the wire
// carries it. What the transparent clocks of a path leave behind.
void putCorrection(std::vector<uint8_t> &buf, NanoTime ns);

std::vector<uint8_t> makeDelayRequest(uint16_t sequenceID, const uint8_t *sourcePortIdentity);
std::vector<uint8_t> makePeerDelayRequest(uint16_t sequenceID, const uint8_t *sourcePortIdentity);

// A one-step Sync: no twoStepFlag, and the origin timestamp travels in
// the Sync itself.
std::vector<uint8_t> makeOneStepSync(uint16_t sequenceID, NanoTime t1);

// An Announce from a master with the dataset given. sourcePortIdentity
// and grandmasterIdentity are both built from identity.
std::vector<uint8_t> makeAnnounce(uint16_t sequenceID, const uint8_t *identity, uint8_t priority1,
                                  uint8_t clockClass = 248, uint8_t priority2 = 128,
                                  uint16_t stepsRemoved = 0);

// The timestamp the hardware will post for the next frame it is asked to
// stamp.
void setTxTimestamp(NanoTime t);

// A timestamp posted right now and read by nobody: what a wait that gave
// up too early leaves sitting in the register.
void postTxTimestamp(NanoTime t);

// Free helpers with external linkage in ptp-base.cpp.
NanoTime timespecToNanoTime(const timespec &tm);
NanoTime bufferToNanoTime(const uint8_t *buf);
NanoTime bufferToCorrection(const uint8_t *buf);
void timespecToBuffer(const timespec &tm, uint8_t *buf);
void nanoTimeToTimespec(NanoTime t, timespec &tm);
