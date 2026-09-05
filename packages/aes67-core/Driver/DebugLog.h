//
// DebugLog.h
// AES67 macOS Driver - Build #7
// Debug logging utilities
//

#pragma once

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <cstdarg>

namespace AES67 {
namespace Debug {

// Log file location
inline const char* GetLogPath() {
    return "/tmp/aes67driver_debug.log";
}

// Write timestamped log message
inline void Log(const char* message) {
    FILE* f = fopen(GetLogPath(), "a");
    if (f) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);

        char timestamp[64];
        time_t nowtime = tv.tv_sec;
        struct tm* nowtm = localtime(&nowtime);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", nowtm);

        fprintf(f, "[%s.%06d] %s\n", timestamp, (int)tv.tv_usec, message);
        fflush(f);
        fclose(f);
    }
}

// Log with formatted string
inline void LogF(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(buffer);
}

// Clear log file
inline void ClearLog() {
    FILE* f = fopen(GetLogPath(), "w");
    if (f) {
        fprintf(f, "=== AES67 Driver Debug Log ===\n");
        fclose(f);
    }
}

} // namespace Debug
} // namespace AES67

// Convenience macros
#define AES67_LOG(msg) AES67::Debug::Log(msg)
#define AES67_LOGF(fmt, ...) AES67::Debug::LogF(fmt, __VA_ARGS__)
