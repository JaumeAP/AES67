#pragma once

#include <Arduino.h>
#include <t41-ptp.h>

// PREDEFINED PTP PROFILES.
//
// A profile is the set of things the box announces that decide how it fits
// into the network you put it on: the domain, how often it sends, the
// priority it presents to the BMCA, and what it considers being locked.
//
// All of this used to be chosen at compile time. Here it is chosen at run
// time, out of a closed list, and the choice is stored in EEPROM. It is done
// as a list and not field by field on purpose: a profile is a coherent set of
// values, and letting them be changed one at a time from a web page invites
// combinations that do not hold together (the advertised interval and the
// real rate coming apart, for instance).
//
// What is NOT per profile and stays in src/main.cpp: the clockClass and the
// timeSource, which depend on whether there is PPS and not on the network;
// the servo gains and boundaries, which are hardware tuning; and the UTC
// offset, which is a property of this box (it does not know absolute time).

struct PtpProfile
{
  const char *id;       // for the URL and the EEPROM, no spaces
  const char *name;     // for display
  const char *summary;  // one line saying what it is for

  uint8_t domainNumber;
  int8_t logSyncInterval;
  int8_t logAnnounceInterval;
  uint8_t priority1;
  uint8_t priority2;
  NanoTime lockThresholdNs;
};

// Number of profiles, and access to the table.
size_t profileCount();
const PtpProfile &profileAt(size_t index);

// Index of the default profile: the one the box used before this existed, so
// an empty EEPROM changes no behaviour.
size_t profileDefaultIndex();

// The stored choice. If there is none, or what is there does not add up, it
// returns the default profile without complaining: a blank EEPROM is the
// normal case.
size_t profileLoadSelection();

// Stores the choice. Returns false if the index does not exist or if reading
// it back does not match what was written.
bool profileSaveSelection(size_t index);

// Period of the sync timer, in microseconds, derived from logSyncInterval.
// The advertised interval and the real rate both come out of this field,
// which is the only way they cannot come apart.
uint32_t profileSyncIntervalUs(const PtpProfile &profile);

// Sync timer ticks that fit in a second, minimum 1. noPPSCount is counted in
// ticks, not in seconds, and the PPS loss threshold is expressed in seconds:
// this is the conversion.
int profileSyncTicksPerSecond(const PtpProfile &profile);
