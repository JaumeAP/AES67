#pragma once

#include <cstdint>

// The dataset comparison of IEEE 1588-2008 §9.3, and the dataset it compares.
//
// Split out of ptp-base.h so that it can be included on its own. It is plain
// data and one pure function: no Arduino, no QNEthernet, no Teensy register,
// nothing that assumes this is a microcontroller at all. That is deliberate --
// this header is also consumed off the board, by the AES67 macOS driver's
// platform-free core, which had its own copy of the same comparison written
// against the same clause. Two copies of a rule that decides which clock a
// segment follows can disagree, and a segment where the two ends disagree is
// one with two masters on it.
//
// Anything added here has to stay free of the board: an include of Arduino.h
// or a Teensy register in this file breaks the macOS build, which is exactly
// the check that keeps it honest.

// A clock further away than this is not one 1588 lets a port follow: the
// field is eight bits and 255 is what a message that has gone round a
// loop ends up carrying.
constexpr uint16_t MAX_STEPS_REMOVED = 255;

// What an Announce says about the clock behind it. The fields the
// dataset comparison of 1588 needs, in the order it compares them.
struct MasterDataset
{
    uint8_t priority1 = 255;
    uint8_t clockClass = 255;
    uint8_t clockAccuracy = 0xff;
    uint16_t offsetScaledLogVariance = 0xffff;
    uint8_t priority2 = 255;
    uint8_t grandmasterIdentity[8] = {0};
    uint16_t stepsRemoved = 0xffff;

    // Which port sent it: the sourcePortIdentity of the Announce, and the
    // only source whose Sync this clock will follow once it has chosen.
    uint8_t portIdentity[10] = {0};
};

// IEEE 1588 clockClass 255: "slave-only", per §7.6.2.4 -- a clock advertising
// this can never legally become a master. Anything at or above this in the
// comparison already loses on clockClass, but a port that builds its own
// Announce should refuse to transmit at all if its source reports this class,
// rather than rely on the comparison rejecting it.
constexpr uint8_t PTP_CLOCK_CLASS_SLAVE_ONLY = 255;

// Whether the candidate is the better master of the two (IEEE 1588-2008
// Figure 27/28, §9.3.2.5). Lower wins at every step; a tie on every quality
// field falls through to comparing grandmasterIdentity, then stepsRemoved,
// then the sending port -- arbitrary, but the same rule at both ends, so two
// clocks can never each conclude they are the better one.
inline bool isBetterMaster(const MasterDataset &candidate, const MasterDataset &current)
{
    if (candidate.priority1 != current.priority1)
    {
        return candidate.priority1 < current.priority1;
    }
    if (candidate.clockClass != current.clockClass)
    {
        return candidate.clockClass < current.clockClass;
    }
    if (candidate.clockAccuracy != current.clockAccuracy)
    {
        return candidate.clockAccuracy < current.clockAccuracy;
    }
    if (candidate.offsetScaledLogVariance != current.offsetScaledLogVariance)
    {
        return candidate.offsetScaledLogVariance < current.offsetScaledLogVariance;
    }
    if (candidate.priority2 != current.priority2)
    {
        return candidate.priority2 < current.priority2;
    }
    for (int i = 0; i < 8; i++)
    {
        if (candidate.grandmasterIdentity[i] != current.grandmasterIdentity[i])
        {
            return candidate.grandmasterIdentity[i] < current.grandmasterIdentity[i];
        }
    }
    // Same grandmaster, reached two ways: the shorter way through the
    // network wins, and a tie there is broken by the port that spoke.
    if (candidate.stepsRemoved != current.stepsRemoved)
    {
        return candidate.stepsRemoved < current.stepsRemoved;
    }
    for (int i = 0; i < 10; i++)
    {
        if (candidate.portIdentity[i] != current.portIdentity[i])
        {
            return candidate.portIdentity[i] < current.portIdentity[i];
        }
    }
    return false;
}

// Whether two datasets came from the same port.
inline bool sameSource(const MasterDataset &a, const MasterDataset &b)
{
    for (int i = 0; i < 10; i++)
    {
        if (a.portIdentity[i] != b.portIdentity[i])
        {
            return false;
        }
    }
    return true;
}
