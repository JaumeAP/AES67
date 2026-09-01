//
// PTPSettingsMapping.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/PTP/PTPSettingsMapping.h"

namespace AES67 {

void applyPTPSettings(const PTPMasterSettings& settings,
                      PTPSlaveConfig& slaveConfig,
                      PTPMasterConfig& masterConfig) {
    // Slave side: how often we ask for the path delay, how we ask, and the
    // queue the question travels in.
    slaveConfig.delayReqIntervalMs = settings.delayReqIntervalMs;
    slaveConfig.delayMechanism = (settings.delayMechanism == "p2p")
                                     ? DelayMechanism::PeerToPeer
                                     : DelayMechanism::EndToEnd;
    slaveConfig.dscp = settings.dscp;

    // Master side: where we sit in BMCA and how often we speak.
    masterConfig.priority1 = static_cast<uint8_t>(settings.priority1);
    masterConfig.priority2 = static_cast<uint8_t>(settings.priority2);
    masterConfig.syncIntervalMs = settings.syncIntervalMs;
    masterConfig.announceIntervalMs = settings.announceIntervalMs;
    masterConfig.dscp = settings.dscp;
}

} // namespace AES67
