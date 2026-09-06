//
// PTPSettingsMapping.h
// AES67
//
// What the installation configured, carried into the two configs the PTP
// engines actually take.
//
// It lives on its own, and as a free function, so it can be tested
// without opening a socket and without dragging in CoreAudio: the whole
// point of the dataset being settable is that the numbers reach the wire,
// and until this existed the only way to know was to capture packets.
//
#pragma once

#include "NetworkEngine/PTP/PTPMasterSettings.h"
#include "NetworkEngine/PTP/PTPProtocolTypes.h"

namespace AES67 {

/// clockClass and clockAccuracy are deliberately NOT carried: the master
/// announces what its clock source really is (PTPMaster reads them from
/// PTPClockSource), and announcing a better class than the source has is
/// telling the segment a lie that BMCA then acts on.
void applyPTPSettings(const PTPMasterSettings& settings,
                      PTPSlaveConfig& slaveConfig,
                      PTPMasterConfig& masterConfig);

} // namespace AES67
