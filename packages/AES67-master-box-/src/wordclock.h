#pragma once

#include <Arduino.h>

// Word clock to 1 PPS divider, built on the i.MX RT1062's QuadTimer 3.
//
// THE PROBLEM
//
// The Rosendahl Nanosyncs puts out word clock, that is the sampling rate
// (44.1 kHz to 192 kHz), not one pulse per second. This box's PTP engine
// expects 1 PPS on the 1588 capture input. A division is missing.
//
// On the old Raspberry Pi project a separate ATtiny85 did this. Here it is
// done with a peripheral the Teensy already carries, adding no hardware and
// spending no CPU: once configured, the divider runs on its own.
//
// WIRING (it has to be done, or none of this works)
//
//   conditioned word clock -> pin 14   (divider input)
//   pin 19 -> pin 15                   (PHYSICAL BRIDGE, output to the 1588)
//
// The bridge is needed because the QuadTimer output and the 1588 capture
// input are different pins and cannot be joined internally.
//
// The word clock canNOT be connected directly: it is 3 Vpp into 75 ohms. It
// needs a 75 ohm termination and a comparator or buffer producing clean
// 3.3 V logic. See HARDWARE.md.

// SAMPLING RATE OF THE WORD CLOCK.
//
// It is no longer chosen at compile time: it is picked at run time from the
// web page, out of the closed list below, and the choice is stored in EEPROM.
// The list is closed because the divider only takes rates that are a multiple
// of 100 and whose second factor fits in 16 bits, which is every standard
// rate and nothing else.
//
// It has to match what the generator puts out. If it does not, the PPS will
// not run at 1 Hz and PTP will be off by the same proportion. At start-up the
// real frequency is measured and a warning goes out over serial if it does not
// match, but nothing is changed automatically: better that you see it than
// that the box changes behaviour without saying so.
size_t wordclockRateCount();
uint32_t wordclockRateAt(size_t index);

// Index of the default rate, 48000: the value that used to be hardcoded, so
// an empty EEPROM changes no behaviour.
size_t wordclockRateDefaultIndex();

// The stored choice. If there is none, or what is there does not add up, it
// returns the default rate without complaining: a blank EEPROM is the normal
// case.
size_t wordclockRateLoadSelection();

// Stores the choice. Returns false if the index does not exist or if reading
// it back does not match what was written.
bool wordclockRateSaveSelection(size_t index);

// Starts the divider for the given rate. Returns false if that frequency
// cannot be divided with this hardware.
bool wordclockDividerBegin(uint32_t rateHz);

// Measures the word clock frequency actually coming in, in hertz, or 0 if
// none is. Blocks for the measurement window. Meant for start-up and
// diagnostics, not for calling often.
uint32_t wordclockMeasureHz();
