//
// ProfileLog.h
// AES67 profiles
// Where this package's log lines go, which is nowhere unless it is told.
//
// The profile manager reads and writes a file, and it says so when that fails.
// It used to say it through the macOS driver's own logger, which is a
// dependency the wrong way round: a table of constraints has no business
// knowing how the program around it logs.
//
// So: silent by default, and a consumer that wants the lines defines
// AES67_PROFILES_LOG_HEADER as the header carrying AES67_LOG and AES67_LOGF
// before including anything from this package. The macOS driver does, so its
// behaviour is unchanged.
//
#pragma once

#if defined(AES67_PROFILES_LOG_HEADER)
#include AES67_PROFILES_LOG_HEADER
#else
#define AES67_LOG(message) ((void)0)
#define AES67_LOGF(...) ((void)0)
#endif
