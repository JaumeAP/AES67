#include "ptp_messages.h"

#include "stubs/stub_state.h"

void putHeader(std::vector<uint8_t> &buf, uint8_t messageType, uint16_t sequenceID)
{
    buf[0] = messageType;
    buf[1] = 2;  // versionPTP
    buf[2] = (uint8_t)((buf.size() >> 8) & 0xff);
    buf[3] = (uint8_t)(buf.size() & 0xff);
    buf[30] = (uint8_t)((sequenceID >> 8) & 0xff);
    buf[31] = (uint8_t)(sequenceID & 0xff);
}

void putTimestamp(std::vector<uint8_t> &buf, NanoTime t)
{
    const NanoTime s = t / NS_PER_S;
    const NanoTime ns = t % NS_PER_S;
    buf[34] = (uint8_t)((s >> 40) & 0xff);
    buf[35] = (uint8_t)((s >> 32) & 0xff);
    buf[36] = (uint8_t)((s >> 24) & 0xff);
    buf[37] = (uint8_t)((s >> 16) & 0xff);
    buf[38] = (uint8_t)((s >> 8) & 0xff);
    buf[39] = (uint8_t)(s & 0xff);
    buf[40] = (uint8_t)((ns >> 24) & 0xff);
    buf[41] = (uint8_t)((ns >> 16) & 0xff);
    buf[42] = (uint8_t)((ns >> 8) & 0xff);
    buf[43] = (uint8_t)(ns & 0xff);
}

void putSource(std::vector<uint8_t> &buf, const uint8_t *identity)
{
    for (size_t i = 0; i < 8; i++)
    {
        buf[20 + i] = identity[i];
    }
    buf[28] = 0;
    buf[29] = 1;
}

void expectedClockID(uint8_t *out)
{
    const uint8_t *mac = ptptest::state().mac;
    out[0] = mac[0];
    out[1] = mac[1];
    out[2] = mac[2];
    out[3] = 0xFF;
    out[4] = 0xFE;
    out[5] = mac[3];
    out[6] = mac[4];
    out[7] = mac[5];
}

std::vector<uint8_t> makeSync(uint16_t sequenceID)
{
    std::vector<uint8_t> buf(44, 0);
    putHeader(buf, 0, sequenceID);
    buf[6] = 0x02;  // twoStepFlag
    return buf;
}

std::vector<uint8_t> makeFollowUp(uint16_t sequenceID, NanoTime t1)
{
    std::vector<uint8_t> buf(44, 0);
    putHeader(buf, 8, sequenceID);
    putTimestamp(buf, t1);
    return buf;
}

std::vector<uint8_t> makeResponse(uint8_t messageType, uint16_t sequenceID, NanoTime ts,
                                  bool matchingIdentity)
{
    std::vector<uint8_t> buf(54, 0);
    putHeader(buf, messageType, sequenceID);
    putTimestamp(buf, ts);
    uint8_t id[8];
    expectedClockID(id);
    for (size_t i = 0; i < 8; i++)
    {
        buf[44 + i] = matchingIdentity ? id[i] : (uint8_t)(id[i] ^ 0xff);
    }
    buf[52] = 0;
    buf[53] = 1;  // portNumber
    return buf;
}

std::vector<uint8_t> makePdelayRespFollowUp(uint16_t sequenceID, NanoTime t5)
{
    return makeResponse(10, sequenceID, t5);
}

void putCorrection(std::vector<uint8_t> &buf, NanoTime ns)
{
    const uint64_t scaled = (uint64_t)ns << 16;
    for (size_t i = 0; i < 8; i++)
    {
        buf[8 + i] = (uint8_t)((scaled >> (56 - 8 * i)) & 0xff);
    }
}

std::vector<uint8_t> makeDelayRequest(uint16_t sequenceID, const uint8_t *sourcePortIdentity)
{
    std::vector<uint8_t> buf(44, 0);
    putHeader(buf, 1, sequenceID);
    for (size_t i = 0; i < 10; i++)
    {
        buf[20 + i] = sourcePortIdentity[i];
    }
    return buf;
}

std::vector<uint8_t> makePeerDelayRequest(uint16_t sequenceID, const uint8_t *sourcePortIdentity)
{
    std::vector<uint8_t> buf(54, 0);
    putHeader(buf, 2, sequenceID);
    for (size_t i = 0; i < 10; i++)
    {
        buf[20 + i] = sourcePortIdentity[i];
    }
    return buf;
}

std::vector<uint8_t> makeOneStepSync(uint16_t sequenceID, NanoTime t1)
{
    std::vector<uint8_t> buf(44, 0);
    putHeader(buf, 0, sequenceID);
    putTimestamp(buf, t1);
    return buf;
}

std::vector<uint8_t> makeAnnounce(uint16_t sequenceID, const uint8_t *identity, uint8_t priority1,
                                  uint8_t clockClass, uint8_t priority2, uint16_t stepsRemoved)
{
    std::vector<uint8_t> buf(64, 0);
    putHeader(buf, 11, sequenceID);
    buf[7] = 0x08;
    for (size_t i = 0; i < 8; i++)
    {
        buf[20 + i] = identity[i];   // sourcePortIdentity, clock identity
        buf[53 + i] = identity[i];   // grandmasterIdentity
    }
    buf[28] = 0;
    buf[29] = 1;                     // sourcePortIdentity, port number
    buf[47] = priority1;
    buf[48] = clockClass;
    buf[49] = 0xfe;                  // clockAccuracy, unknown
    buf[50] = 0xff;
    buf[51] = 0xff;                  // offsetScaledLogVariance, unknown
    buf[52] = priority2;
    buf[61] = (uint8_t)((stepsRemoved >> 8) & 0xff);
    buf[62] = (uint8_t)(stepsRemoved & 0xff);
    buf[63] = 0xa0;                  // timeSource, internal oscillator
    return buf;
}

void postTxTimestamp(NanoTime t)
{
    timespec ts;
    nanoTimeToTimespec(t, ts);
    ptptest::state().txTimestamp = ts;
    ptptest::state().txPending = true;
}

void setTxTimestamp(NanoTime t)
{
    timespec ts;
    nanoTimeToTimespec(t, ts);
    ptptest::state().txTimestamp = ts;
    ptptest::state().txAvailable = true;
    ptptest::state().txPending = false;
}
