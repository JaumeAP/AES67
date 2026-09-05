//
// AudioStatusView+Preview.swift
// AES67 Manager
// Xcode canvas previews, kept out of the view file itself.
//
// ManagerApp/build.sh lists its sources explicitly and does not include
// this directory: the #Preview macro needs the PreviewsMacros plugin,
// which ships with full Xcode and not with the Command Line Tools, so a
// command-line build would fail on it. Open the project in Xcode and add
// these files to the target to get the canvas back.
//

import SwiftUI

#Preview("Level Meter") {
    VStack(spacing: 20) {
        LevelMeter(level: 0.3, peak: 0.4)
        LevelMeter(level: 0.6, peak: 0.75)
        LevelMeter(level: 0.85, peak: 0.92)
        LevelMeter(level: 0.99, peak: 1.0)
    }
    .padding()
    .frame(width: 300)
}

#Preview("Channel Meters") {
    ChannelMetersView(
        channelLevels: StreamAudioLevels.mockData(channelCount: 8).channelLevels
    )
    .padding()
    .frame(width: 400)
}

#Preview("Compact Meters") {
    CompactChannelMeters(
        channelLevels: StreamAudioLevels.mockData(channelCount: 8).channelLevels
    )
    .padding()
    .frame(width: 200)
}

#Preview("Signal Indicator") {
    VStack(spacing: 20) {
        SignalPresentIndicator(hasSignal: true)
        SignalPresentIndicator(hasSignal: false)
        SignalPresentIndicator(hasSignal: true, showLabel: false)
    }
    .padding()
}

#Preview("Stream Audio Panel") {
    StreamAudioStatusPanel(stream: StreamInfo.example)
        .padding()
        .frame(width: 400)
}

#Preview("Legacy Views") {
    VStack {
        AudioStatusPanel()
        Divider()
        StreamStatusView(stream: StreamInfo(
            id: UUID(),
            name: "Test Stream",
            multicastIP: "239.1.2.3",
            port: 5004,
            encoding: "L24",
            sampleRate: 48000,
            numChannels: 2,
            isActive: true
        ))
    }
    .padding()
}
