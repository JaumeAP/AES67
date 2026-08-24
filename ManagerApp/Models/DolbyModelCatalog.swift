//
// DolbyModelCatalog.swift
// AES67 Manager App
//
// Hand-synced mirror of NetworkEngine/DolbyModelCatalog.h — the fixed
// model → channel-count/direction facts PTP detection can't supply. Kept in
// step with that header the same way the CompatibilityProfile list is.
// The user assigns each detected element a model here; the channel count that
// implies is what will drive the driver's own input/output layout.
//

import Foundation

enum DolbyIoDirection {
    case input   // a source that feeds this driver (cinema processor)
    case output  // a sink this driver feeds (amplifier)
}

struct DolbyModel: Identifiable, Equatable, Hashable {
    let id: String          // stable key, matches the C++ catalog
    let displayName: String
    let channels: Int       // channels for ONE unit of this model
    let direction: DolbyIoDirection
}

enum DolbyModelCatalog {
    /// Mirror of DolbyModelCatalog::all() in the C++ header. Channel counts
    /// align with the same gear's CompatibilityProfile.
    static let all: [DolbyModel] = [
        // Amplifiers — outputs this driver feeds.
        DolbyModel(id: "dac3202", displayName: "Dolby DAC3202",          channels: 32, direction: .output),
        DolbyModel(id: "dma16",   displayName: "Dolby DMA (16-channel)", channels: 16, direction: .output),
        DolbyModel(id: "dma24",   displayName: "Dolby DMA (24-channel)", channels: 24, direction: .output),
        DolbyModel(id: "dma32",   displayName: "Dolby DMA (32-channel)", channels: 32, direction: .output),
        // Cinema processors — inputs that feed this driver.
        DolbyModel(id: "cp850",   displayName: "Dolby CP850",  channels: 64, direction: .input),
        DolbyModel(id: "cp950",   displayName: "Dolby CP950",  channels: 16, direction: .input),
        DolbyModel(id: "cp950a",  displayName: "Dolby CP950A", channels: 64, direction: .input),
    ]

    static func byId(_ id: String) -> DolbyModel? {
        all.first { $0.id == id }
    }

    static func forDirection(_ direction: DolbyIoDirection) -> [DolbyModel] {
        all.filter { $0.direction == direction }
    }

    /// Sum the channels of assigned model ids on one side. Unknown ids
    /// contribute 0. Mirror of the C++ totalChannels resolver.
    static func totalChannels(_ modelIds: [String], _ direction: DolbyIoDirection) -> Int {
        modelIds
            .compactMap { byId($0) }
            .filter { $0.direction == direction }
            .reduce(0) { $0 + $1.channels }
    }
}
