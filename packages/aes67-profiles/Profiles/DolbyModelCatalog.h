#ifndef DOLBY_MODEL_CATALOG_H
#define DOLBY_MODEL_CATALOG_H

//
// DolbyModelCatalog
// AES67 profiles
//
// The fixed facts PTP detection can't supply. Passive PTP observation
// (PTPPeerObserver) tells us a Dolby element is present, its vendor OUI, and
// its role/direction — but NOT its model, and therefore not its channel
// count, because every Dolby unit shares a vendor OUI and PTP carries no
// model field. This catalog is the lookup that turns a user-confirmed model
// into a channel count and a direction, so the found-elements list can drive
// the driver's own input/output channel layout.
//
// It is the authoritative table; the Swift ManagerApp keeps a hand-synced
// mirror the same way it mirrors CompatibilityProfile. Header-only and
// side-effect-free so it can be unit-tested (TestDolbyModelCatalog) without
// linking anything.
//
// Channel counts are aligned with the same gear's CompatibilityProfile
// (DAC3202 32, DMA 16/24/32 per single unit, CP850/CP950A 64, CP950 16) —
// see Profiles/CompatibilityProfile.cpp. Directions are from THIS
// driver's own side: an amplifier is a sink we feed (Output), a cinema
// processor is a source that feeds us (Input) — matching those profiles'
// TransmitOnly / ReceiveOnly directions.
//

#include <cstdint>
#include <string>
#include <vector>

namespace AES67 {

enum class DolbyIoDirection { Input, Output };

struct DolbyModel {
    std::string id;            // stable key, e.g. "dac3202", "dma32", "cp850"
    std::string displayName;   // human label
    uint32_t channels{0};      // channels for ONE unit of this model
    DolbyIoDirection direction{DolbyIoDirection::Output};
};

class DolbyModelCatalog {
public:
    static const std::vector<DolbyModel>& all() {
        static const std::vector<DolbyModel> models = {
            // Amplifiers — sinks this driver feeds (Output). One entry per
            // real single-unit model; a chain of N shows as N detected units,
            // each assigned its own model, so totals sum naturally.
            {"dac3202", "Dolby DAC3202",         32, DolbyIoDirection::Output},
            {"dma16",   "Dolby DMA (16-channel)", 16, DolbyIoDirection::Output},
            {"dma24",   "Dolby DMA (24-channel)", 24, DolbyIoDirection::Output},
            {"dma32",   "Dolby DMA (32-channel)", 32, DolbyIoDirection::Output},
            // Cinema processors — sources that feed this driver (Input).
            {"cp850",   "Dolby CP850",           64, DolbyIoDirection::Input},
            {"cp950",   "Dolby CP950",           16, DolbyIoDirection::Input},
            {"cp950a",  "Dolby CP950A",          64, DolbyIoDirection::Input},
        };
        return models;
    }

    // Model by id, or nullptr if the id isn't in the catalog.
    static const DolbyModel* byId(const std::string& id) {
        for (const auto& m : all()) {
            if (m.id == id) return &m;
        }
        return nullptr;
    }

    // Models valid for one direction — what a detected element on that side
    // may be assigned to (an Output peer can only be an amplifier, etc.).
    static std::vector<DolbyModel> forDirection(DolbyIoDirection dir) {
        std::vector<DolbyModel> out;
        for (const auto& m : all()) {
            if (m.direction == dir) out.push_back(m);
        }
        return out;
    }

    // Sum the channels of a set of assigned model ids in one direction.
    // Unknown ids contribute 0 (and are worth surfacing to the user, but the
    // sum stays well-defined). This is the core of "generate the driver's
    // ins/outs from the found list": each detected unit contributes its
    // model's channel count, in list order.
    static uint32_t totalChannels(const std::vector<std::string>& assignedModelIds,
                                  DolbyIoDirection dir) {
        uint32_t total = 0;
        for (const auto& id : assignedModelIds) {
            const DolbyModel* m = byId(id);
            if (m && m->direction == dir) total += m->channels;
        }
        return total;
    }
};

} // namespace AES67

#endif // DOLBY_MODEL_CATALOG_H
