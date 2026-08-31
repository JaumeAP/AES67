//
// AES67Device.h
// AES67 macOS Driver - Build #1
// Core Audio device implementation using libASPL
// 128-channel input/output AudioServerPlugIn
//

#pragma once

#include "Shared/Types.h"
#include "Shared/RingBuffer.hpp"
#include "NetworkEngine/StreamManager.h"
#include "NetworkEngine/Discovery/SAPListener.h"
#include "NetworkEngine/Discovery/MDNSBrowser.h"
#include "NetworkEngine/Discovery/SAPAnnouncer.h"
#include "NetworkEngine/PTP/PTPPeerObserver.h"
#include "NetworkEngine/Discovery/RTCPMonitor.h"
#include "NetworkEngine/RTSafeStreamInterface.h"
#include <aspl/Device.hpp>
#include <aspl/Stream.hpp>
#include <aspl/Context.hpp>
#include <memory>
#include <array>
#include <atomic>

namespace AES67 {

class AES67IOHandler;

//
// AES67 Audio Device
//
// 128-channel bidirectional Core Audio device
// Integrates with macOS Core Audio via AudioServerPlugIn (libASPL)
//
class AES67Device : public aspl::Device {
public:
    /// Capacity of the RT ring buffer arrays — fixed at compile time, never
    /// resized. The number of channels actually advertised to Core Audio is
    /// activeChannelCount_ (see GetInputChannelCount), which is <= this and
    /// comes from DeviceChannelSettings.
    static constexpr size_t kNumChannels = 128;

    // Supported sample rates
    static constexpr std::array<Float64, 8> kSupportedSampleRates = {
        44100.0, 48000.0, 88200.0, 96000.0,
        176400.0, 192000.0, 352800.0, 384000.0
    };

    // Supported buffer sizes (in samples)
    static constexpr std::array<UInt32, 8> kSupportedBufferSizes = {
        16, 32, 48, 64, 128, 192, 288, 480
    };

    //
    // Constructor
    //
    explicit AES67Device(std::shared_ptr<aspl::Context> context);
    ~AES67Device();

    // Initialize device (must be called after construction)
    void Initialize();

    //
    // Device Configuration
    //

    // Get/Set sample rate
    Float64 GetSampleRate() const;
    OSStatus SetSampleRate(Float64 sampleRate);

    // Get available sample rates
    std::vector<AudioValueRange> GetAvailableSampleRates() const override;

    // Get/Set buffer size
    UInt32 GetBufferSize() const;
    OSStatus SetBufferSize(UInt32 bufferSize);

    // Get available buffer sizes
    std::vector<UInt32> GetAvailableBufferSizes() const;

    //
    // Device Information
    //

    std::string GetDeviceName() const;
    std::string GetDeviceManufacturer() const;
    std::string GetDeviceUID() const override;

    // Channels exposed to Core Audio: always all of them. What the user's
    // channel-count settings narrow is usableRxChannelCount_/
    // usableTxChannelCount_ below, i.e. how many the stream mapper will
    // actually hand out, per direction.
    UInt32 GetInputChannelCount() const { return kNumChannels; }
    UInt32 GetOutputChannelCount() const { return kNumChannels; }

    /// Channels the mapper may assign to RX streams, from the persisted
    /// DeviceChannelSettings.rx. <= kNumChannels; the rest stay advertised
    /// but unused.
    UInt32 GetUsableRxChannelCount() const { return usableRxChannelCount_; }

    /// Same as above, for TX streams / DeviceChannelSettings.tx.
    UInt32 GetUsableTxChannelCount() const { return usableTxChannelCount_; }

    //
    // Stream Access
    //

    // Get input/output streams
    std::shared_ptr<aspl::Stream> GetInputStream() const { return inputStream_; }
    std::shared_ptr<aspl::Stream> GetOutputStream() const { return outputStream_; }

    //
    // Ring Buffer Access (for NetworkEngine)
    //

    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, kNumChannels>;

    DeviceChannelBuffers& GetInputBuffers() { return inputBuffers_; }
    DeviceChannelBuffers& GetOutputBuffers() { return outputBuffers_; }

    //
    // RT-Safe Interface Access
    //

    // Returns the RT-safe interface for use by AES67IOHandler.
    // The interface is created during Initialize() and is valid for the
    // lifetime of this device. Only pass this to RT-safe code paths.
    RTSafeStreamInterface* GetRTInterface() { return rtInterface_.get(); }
    const RTSafeStreamInterface* GetRTInterface() const { return rtInterface_.get(); }

    //
    // Stream Manager Access
    //

    StreamManager* GetStreamManager() { return streamManager_.get(); }
    const StreamManager* GetStreamManager() const { return streamManager_.get(); }

    //
    // Control
    //

    // Start/Stop IO (overrides from aspl::Device)
    OSStatus StartIOImpl(UInt32 clientID, UInt32 startCount) override;
    OSStatus StopIOImpl(UInt32 clientID, UInt32 startCount) override;

    //
    // Statistics
    //

    uint64_t GetInputUnderrunCount() const { return inputUnderruns_.load(); }
    uint64_t GetOutputUnderrunCount() const { return outputUnderruns_.load(); }
    void ResetStatistics();

private:
    // Internal handlers
    OSStatus OnSetSampleRate(Float64 sampleRate);
    OSStatus OnSetBufferSize(UInt32 bufferSize);

    // The gateway: backs the kPTPDiagnosticsPropertySelector custom
    // property (Shared/CustomProperties.h), registered on `this` in the
    // constructor via RegisterCustomProperty(). Any process — ManagerApp,
    // in practice — can AudioObjectGetPropertyData() this device's ID and
    // read live PTP state, no other channel needed: that's what a custom
    // AudioObject property already is, cross-process, for free.
    CFPropertyListRef GetPTPDiagnosticsProperty() const;

    /// Sessions currently being announced over SAP, as a CFArray of
    /// CFDictionaries — see Shared/CustomProperties.h. Same gateway
    /// mechanism as the diagnostics property above.
    CFPropertyListRef GetDiscoveredSessionsProperty() const;

    /// Backs kPtpPeersPropertySelector: a CFArray of CFDictionaries, one per
    /// distinct PTP clock identity currently seen (PTPPeerObserver). Lets
    /// ManagerApp list the Dolby elements present by role. Same gateway
    /// mechanism as the two above.
    CFPropertyListRef GetPtpPeersProperty() const;

    /// Backs kRtcpReceiversPropertySelector: a CFArray of CFDictionaries, one
    /// per distinct receiver that has sent an RTCP report on a transmit
    /// stream (RTCPMonitor) - the second amplifier-detection vector.
    CFPropertyListRef GetRtcpReceiversProperty() const;

    // Initialize streams and IO handler
    void InitializeStreams();
    void InitializeIOHandler();

    // Calculate optimal ring buffer size based on sample rate
    // Returns size for desired latency (default: 3ms for network jitter tolerance)
    // Result is rounded up to power of 2 for efficient modulo operations
    static size_t CalculateRingBufferSize(Float64 sampleRate, double latencyMs = 3.0);

    // Ring buffers for audio data
    // Network threads write to input buffers, read from output buffers
    // Core Audio thread reads from input buffers, writes to output buffers
    DeviceChannelBuffers inputBuffers_;   // Network → CoreAudio
    DeviceChannelBuffers outputBuffers_;  // CoreAudio → Network

    // Streams
    std::shared_ptr<aspl::Stream> inputStream_;
    std::shared_ptr<aspl::Stream> outputStream_;

    // IO Handler
    std::shared_ptr<AES67IOHandler> ioHandler_;

    // Stream Manager (manages all AES67 network streams)
    std::unique_ptr<StreamManager> streamManager_;

    // RT-safe interface (compile-time boundary for IO handler)
    // Created during Initialize(), references inputBuffers_/outputBuffers_/atomics
    std::unique_ptr<RTSafeStreamInterface> rtInterface_;

    // SAP discovery. Runs for the driver's whole life once Initialize()
    // starts it — passive listening only, it announces nothing.
    std::unique_ptr<SAPListener> sapListener_;
    /// mDNS/DNS-SD browsing, the discovery half SAP does not cover:
    /// professional gear publishes its sessions as `_rtsp._tcp` services
    /// rather than (or as well as) shouting SDP over SAP. Declared next
    /// to sapListener_ so both are destroyed before streamManager_, which
    /// their callbacks reach into (2026-08-31).
    std::unique_ptr<MDNSBrowser> mdnsBrowser_;
    std::unique_ptr<SAPAnnouncer> sapAnnouncer_;
    std::unique_ptr<PTPPeerObserver> ptpPeerObserver_;
    std::unique_ptr<RTCPMonitor> rtcpMonitor_;

    // Channels the mapper may hand out to streams, per direction. Set once
    // in the constructor from the persisted DeviceChannelSettings.rx/.tx and
    // never changed afterwards — selecting a different count only takes
    // effect the next time Core Audio starts this driver, which is why
    // ManagerApp disables the relevant selector while the driver is
    // installed (and disables the whole direction's selector when the
    // active compatibility profile rules that direction out).
    UInt32 usableRxChannelCount_{static_cast<UInt32>(kNumChannels)};
    UInt32 usableTxChannelCount_{static_cast<UInt32>(kNumChannels)};

    // Current configuration
    std::atomic<Float64> currentSampleRate_{48000.0};
    std::atomic<UInt32> currentBufferSize_{64};

    // State
    std::atomic<bool> ioRunning_{false};

    // Statistics
    std::atomic<uint64_t> inputUnderruns_{0};
    std::atomic<uint64_t> outputUnderruns_{0};

    // Resize ring buffers to accommodate new sample rate
    void ResizeRingBuffers(Float64 sampleRate);
};

} // namespace AES67
