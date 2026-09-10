#pragma once

// The few things the PTPBase translation units share and nothing outside them
// needs.
//
// PTPBase's implementation is split across five files by what each part does
// -- the port and its loop, the configuration, the clock discipline, the
// parsers and the senders. They were one 2000-line file; splitting it is what
// this header exists for, since these helpers were file-scope functions that
// every part used and that no consumer of the library should see.
//
// Nothing here is part of the public API. src/t41-ptp.h does not include it.

#include <TimeLib.h>

#include "ptp-base.h"

// How much this library says on the serial port. Compile-time, so a
// switched-off level costs nothing in flash: see ptp-base.h.
constexpr int logging = T41PTP_LOGGING_LEVEL;

// Prints a NanoTime as a date and a time, for the logging above.
void printTime(const NanoTime t);

// The conversions between the three shapes a time takes in this library: a
// timespec from the transport, a NanoTime for the arithmetic, and the ten
// bytes of a PTP timestamp on the wire.
NanoTime timespecToNanoTime(const timespec &tm);
NanoTime bufferToNanoTime(const uint8_t *buf);
void nanoTimeToTimespec(const NanoTime t, timespec &tm);

// The correctionField of the common header: 48 bits of nanoseconds and 16 of
// sub-nanoseconds, which a transparent clock adds its residence time to.
NanoTime bufferToCorrection(const uint8_t *buf);
void copyCorrectionField(const uint8_t *src, uint8_t *dst);

// The ten bytes of a PTP timestamp, written from a timespec.
void timespecToBuffer(const timespec &tm, uint8_t *buf);
