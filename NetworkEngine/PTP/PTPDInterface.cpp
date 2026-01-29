#include "PTPDInterface.h"

// Include ptpd headers
extern "C" {
#include "../vendor/ptpd/src/ptpd.h"
#include "../vendor/ptpd/src/datatypes.h"
#include "../vendor/ptpd/src/constants.h"
#include "../vendor/ptpd/src/ptp_timers.h"
#include "../vendor/ptpd/src/datatypes_dep.h"
#include "../vendor/ptpd/src/dep/ptpd_dep.h"
}

#include "CustomSys.h"
#include <thread>
#include <iostream>

namespace AES67 {

// Global pointer to PTPDInterface instance to access from C callbacks
static PTPDInterface* g_ptpdInterface = nullptr;

// Custom time adjustment function that updates our shared state
void customAdjustSystemTime(double offset, Boolean negative, void* arg) {
    PTPDInterface* instance = static_cast<PTPDInterface*>(arg);
    if (instance) {
        // Convert offset from microseconds to nanoseconds
        int64_t offsetNs = static_cast<int64_t>(offset * 1000.0);

        // Instead of adjusting the system time, update our shared state
        instance->getState().offsetNs.store(offsetNs);
        instance->getState().masterOffsetNs.fetch_add(offsetNs);
    }
}

// Custom frequency adjustment function
void customAdjustSystemFrequency(double freq_offset, void* arg) {
    PTPDInterface* instance = static_cast<PTPDInterface*>(arg);
    if (instance) {
        // Store frequency drift in our state
        instance->getState().frequencyDrift.store(freq_offset);
    }
}

PTPDInterface::PTPDInterface() 
    : running_(false), ptpdInstance_(nullptr) {
    // Initialize state
    state_.masterOffsetNs.store(0);
    state_.frequencyDrift.store(0.0);
    state_.isLocked.store(false);
    state_.clockClass.store(248); // Default to slave-only clock
    state_.clockAccuracy.store(0xFE); // Unknown accuracy
    
    g_ptpdInterface = this;
}

PTPDInterface::~PTPDInterface() {
    stop();
    g_ptpdInterface = nullptr;
}

bool PTPDInterface::init(const std::string& interfaceName) {
    interfaceName_ = interfaceName;
    
    // Initialize ptpd data structures
    // We'll create a minimal initialization here that doesn't fork or become a daemon
    ptpdInstance_ = malloc(sizeof(RunTimeOpts) + sizeof(Global));
    if (!ptpdInstance_) {
        return false;
    }
    
    RunTimeOpts* rtOpts = static_cast<RunTimeOpts*>(ptpdInstance_);
    Global* glo = reinterpret_cast<Global*>(
        reinterpret_cast<char*>(ptpdInstance_) + sizeof(RunTimeOpts)
    );
    
    // Initialize runtime options with minimal configuration
    memset(rtOpts, 0, sizeof(RunTimeOpts));
    memset(glo, 0, sizeof(Global));
    
    // Set interface name
    strncpy(rtOpts->ifaceName, interfaceName_.c_str(), sizeof(rtOpts->ifaceName) - 1);
    
    // Set custom time adjustment functions to update our state instead of the system time
    rtOpts->adjustMethod = ADJ_CUSTOM;
    rtOpts->custom_adjust_func = customAdjustSystemTime;
    rtOpts->custom_freqadj_func = customAdjustSystemFrequency;
    rtOpts->custom_arg = this;
    
    // Additional initialization
    rtOpts->daemon = FALSE;  // Don't become a daemon
    rtOpts->verbose = FALSE; // Reduce verbosity for embedded use
    
    return true;
}

void PTPDInterface::start() {
    if (running_) {
        return;
    }
    
    running_ = true;
    
    // Start ptpd in a separate thread
    std::thread ptpdThread([this]() {
        RunTimeOpts* rtOpts = static_cast<RunTimeOpts*>(ptpdInstance_);
        Global* glo = reinterpret_cast<Global*>(
            reinterpret_cast<char*>(ptpdInstance_) + sizeof(RunTimeOpts)
        );
        
        // Initialize ptpd
        if (!initPTP(rtOpts, glo)) {
            std::cerr << "Failed to initialize PTP" << std::endl;
            return;
        }
        
        // Main protocol loop
        while (running_) {
            if (!protocol(rtOpts, glo)) {
                std::cerr << "PTP protocol error" << std::endl;
                break;
            }
            
            // Update our state based on PTP status
            if (glo->number_foreign_records > 0) {
                // Check if we're synchronized
                bool isLocked = (glo->port_state == PTP_SLAVE) || (glo->port_state == PTP_MASTER);
                
                state_.isLocked.store(isLocked);
                state_.clockClass.store(glo->clock_quality.clock_class);
                state_.clockAccuracy.store(glo->clock_quality.clock_accuracy);
                
                // Update offset
                double currentOffset = glo->offset_from_master;
                state_.masterOffsetNs.store(static_cast<int64_t>(currentOffset * 1000.0)); // Convert to nanoseconds
            }
            
            // Small delay to prevent busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // Cleanup
        cleanupPTP(glo);
    });
    
    ptpdThread.detach();  // Detach thread, we'll handle stopping through the running_ flag
}

void PTPDInterface::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Give some time for the thread to stop gracefully
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

PTPState& PTPDInterface::getState() {
    return state_;
}

PTPDiagnostics& PTPDInterface::getDiagnostics() {
    return diagnostics_;
}

} // namespace AES67