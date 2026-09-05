#pragma once

#include <Arduino.h>

// CONFIGURATION SERVER.
//
// A minimal HTTP server on port 80 serving a single page with two lists: the
// PTP profiles from src/profiles.h and the word clock rates from
// src/wordclock.h, each with its current choice marked and a button to change
// it. Both choices are stored in EEPROM and applied without a restart.
//
// They are two separate lists and not one because they are two separate
// things: the profile is how the box presents itself to the network, and the
// rate is what the generator feeding it puts out.
//
// It is plain HTTP with no password: anyone who can reach the box over the
// network can change its profile. It goes on the audio network, which is not
// the internet, and adding authentication with nowhere to keep the
// credentials would be worse than having none. If the box has to sit on an
// open network, this needs rethinking.
//
// It uses no server library: it is a couple of dozen lines of text over a
// QNEthernet EthernetClient.

// Function applying a profile that has already been chosen. main.cpp
// implements it, being the one holding the PTP object and the timers.
typedef void (*ProfileApplyFn)(size_t index);

// The same, for the word clock rate: it restarts the divider.
typedef void (*RateApplyFn)(size_t index);

// Starts the server. Each function passed in is called when somebody picks
// that thing from the page, AFTER it has been stored.
void webconfigBegin(ProfileApplyFn apply, RateApplyFn applyRate);

// Serves whichever client is waiting, if any. Called from loop(); it does not
// block waiting for somebody to arrive.
void webconfigUpdate();
