//
// AddStreamView.swift
// AES67 Manager - Build #12
// Sheet for adding new streams manually with sample rate mismatch handling
//

import SwiftUI

// MARK: - Sample Rate Mismatch Alert View

struct SampleRateMismatchAlert: View {
    let streamRate: UInt32
    let deviceRate: Double
    let onChangeDevice: () -> Void
    let onCancel: () -> Void

    var body: some View {
        VStack(spacing: 16) {
            // Warning icon
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 48))
                .foregroundColor(.orange)

            // Title
            Text("Sample Rate Mismatch")
                .font(.headline)

            // Description
            Text("The stream uses **\(formatRate(Double(streamRate)))** but the device is set to **\(formatRate(deviceRate))**.")
                .multilineTextAlignment(.center)
                .foregroundColor(.secondary)

            // Additional info
            Text("Streams must match the device sample rate for proper audio playback.")
                .font(.caption)
                .multilineTextAlignment(.center)
                .foregroundColor(.secondary)
                .padding(.horizontal)

            Divider()
                .padding(.vertical, 8)

            // Action buttons
            VStack(spacing: 12) {
                Button(action: onChangeDevice) {
                    HStack {
                        Image(systemName: "arrow.triangle.2.circlepath")
                        Text("Change Device to \(formatRate(Double(streamRate)))")
                    }
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)

                Button(action: onCancel) {
                    Text("Cancel")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .controlSize(.regular)
            }
        }
        .padding(24)
        .frame(width: 320)
        .background(Color(nsColor: .windowBackgroundColor))
        .cornerRadius(12)
    }

    private func formatRate(_ rate: Double) -> String {
        if rate >= 1000 {
            let khz = rate / 1000.0
            if khz.truncatingRemainder(dividingBy: 1) == 0 {
                return "\(Int(khz)) kHz"
            } else {
                return String(format: "%.1f kHz", khz)
            }
        }
        return "\(Int(rate)) Hz"
    }
}

// MARK: - Add Stream View

struct AddStreamView: View {
    @EnvironmentObject var driverManager: DriverManager
    @Environment(\.dismiss) var dismiss

    @State private var streamName = ""
    @State private var multicastIP = "239.0.0.1"
    @State private var port = "5004"
    @State private var numChannels = 8
    @State private var sampleRate = 48000
    @State private var encoding = "L24"
    @State private var showMismatchAlert = false
    @State private var pendingAddition = false

    let sampleRates = [44100, 48000, 88200, 96000, 176400, 192000]
    let encodings = ["L16", "L24"]

    var body: some View {
        ZStack {
            // Main content
            VStack(spacing: 0) {
                // Header
                HStack {
                    Text("Add AES67 Stream")
                        .font(.title2)
                        .fontWeight(.bold)
                    Spacer()
                    Button("Cancel") {
                        dismiss()
                    }
                }
                .padding()

                Divider()

                // Form
                Form {
                    Section("Stream Information") {
                        TextField("Stream Name", text: $streamName)
                            .textFieldStyle(.roundedBorder)
                    }

                    Section("Network") {
                        TextField("Multicast IP", text: $multicastIP)
                            .textFieldStyle(.roundedBorder)

                        TextField("Port", text: $port)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 100)
                    }

                    Section("Audio Format") {
                        HStack {
                            Text("Channels")
                            Spacer()
                            Stepper("\(numChannels)", value: $numChannels, in: 1...128)
                                .frame(width: 120)
                        }

                        HStack {
                            Text("Sample Rate")
                            Spacer()
                            Picker("", selection: $sampleRate) {
                                ForEach(sampleRates, id: \.self) { rate in
                                    HStack {
                                        Text("\(rate) Hz")
                                        if rate != Int(driverManager.currentDeviceSampleRate) {
                                            Image(systemName: "exclamationmark.circle")
                                                .foregroundColor(.orange)
                                        }
                                    }
                                    .tag(rate)
                                }
                            }
                            .pickerStyle(.menu)
                            .frame(width: 150)
                        }

                        // Sample rate warning indicator
                        if sampleRate != Int(driverManager.currentDeviceSampleRate) {
                            HStack(spacing: 8) {
                                Image(systemName: "exclamationmark.triangle.fill")
                                    .foregroundColor(.orange)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("Sample rate differs from device")
                                        .font(.caption)
                                        .foregroundColor(.orange)
                                    Text("Device is set to \(DriverManager.formatSampleRate(driverManager.currentDeviceSampleRate))")
                                        .font(.caption2)
                                        .foregroundColor(.secondary)
                                }
                            }
                            .padding(.vertical, 4)
                        }

                        HStack {
                            Text("Encoding")
                            Spacer()
                            Picker("", selection: $encoding) {
                                ForEach(encodings, id: \.self) { enc in
                                    Text(enc).tag(enc)
                                }
                            }
                            .pickerStyle(.segmented)
                            .frame(width: 150)
                        }
                    }

                    Section("Channel Mapping") {
                        HStack {
                            Text("Current device sample rate:")
                                .font(.caption)
                                .foregroundColor(.secondary)
                            Spacer()
                            Text(DriverManager.formatSampleRate(driverManager.currentDeviceSampleRate))
                                .font(.caption)
                                .fontWeight(.medium)
                        }

                        Text("Stream will be mapped to first available device channels")
                            .font(.caption)
                            .foregroundColor(.secondary)

                        if driverManager.totalChannelsUsed + numChannels > 128 {
                            Label("Not enough channels available", systemImage: "exclamationmark.triangle.fill")
                                .foregroundColor(.orange)
                                .font(.caption)
                        }
                    }
                }
                .formStyle(.grouped)
                .scrollContentBackground(.hidden)

                Divider()

                // Footer
                HStack {
                    Spacer()
                    Button("Cancel") {
                        dismiss()
                    }
                    .keyboardShortcut(.cancelAction)

                    Button("Add Stream") {
                        addStream()
                    }
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
                    .disabled(!isValid)
                }
                .padding()
            }
            .frame(width: 500, height: 600)
            .blur(radius: showMismatchAlert ? 2 : 0)
            .disabled(showMismatchAlert)

            // Sample rate mismatch overlay
            if showMismatchAlert {
                Color.black.opacity(0.3)
                    .ignoresSafeArea()

                SampleRateMismatchAlert(
                    streamRate: UInt32(sampleRate),
                    deviceRate: driverManager.currentDeviceSampleRate,
                    onChangeDevice: {
                        handleChangeDeviceSampleRate()
                    },
                    onCancel: {
                        showMismatchAlert = false
                    }
                )
                .shadow(radius: 20)
                .transition(.scale.combined(with: .opacity))
            }
        }
        .animation(.easeInOut(duration: 0.2), value: showMismatchAlert)
    }

    private var isValid: Bool {
        !streamName.isEmpty &&
        !multicastIP.isEmpty &&
        !port.isEmpty &&
        driverManager.totalChannelsUsed + numChannels <= 128
    }

    private func addStream() {
        guard let portNum = UInt16(port) else { return }

        // Check for sample rate mismatch before adding
        if sampleRate != Int(driverManager.currentDeviceSampleRate) {
            showMismatchAlert = true
            return
        }

        // No mismatch, add directly
        performAddStream(portNum: portNum)
    }

    private func handleChangeDeviceSampleRate() {
        guard let portNum = UInt16(port) else { return }

        // Change device sample rate
        if driverManager.setDeviceSampleRate(Double(sampleRate)) {
            showMismatchAlert = false
            // Now add the stream
            performAddStream(portNum: portNum)
        }
    }

    private func performAddStream(portNum: UInt16) {
        let result = driverManager.addStream(
            name: streamName,
            multicastIP: multicastIP,
            port: portNum,
            numChannels: UInt16(numChannels),
            sampleRate: UInt32(sampleRate),
            encoding: encoding,
            bypassSampleRateCheck: true
        )

        switch result {
        case .success:
            dismiss()
        case .sampleRateMismatch:
            // This shouldn't happen with bypassSampleRateCheck: true
            showMismatchAlert = true
        case .channelLimitExceeded:
            // Already handled by isValid, but just in case
            break
        }
    }
}

// MARK: - Preview

#Preview("Add Stream View") {
    AddStreamView()
        .environmentObject(DriverManager())
}

#Preview("Sample Rate Mismatch Alert") {
    SampleRateMismatchAlert(
        streamRate: 96000,
        deviceRate: 48000,
        onChangeDevice: { print("Change device") },
        onCancel: { print("Cancel") }
    )
    .padding()
    .background(Color.gray.opacity(0.3))
}
