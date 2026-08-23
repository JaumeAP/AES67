//
// CustomProperties.h
// AES67 macOS Driver
// The gateway: custom AudioObject properties AES67Device exposes on its own
// AudioObjectID, so ManagerApp — a separate process, no other channel to
// the driver running inside coreaudiod — can query live state instead of
// showing PTPDiagnosticView's old placeholder data.
//
// CoreAudio's AudioObjectGetPropertyData is already cross-process (that's
// its whole point: a plugin inside coreaudiod, queried by any client) —
// this needed no XPC, no shared files, no extra entitlements, just
// aspl::Object::RegisterCustomProperty (Driver/AES67Device.cpp) on the
// write side and the plain AudioObjectGetPropertyData Swift already uses
// elsewhere (DriverManager.swift's findAES67DeviceID) on the read side.
//
// Swift has no equivalent header to include, so the selector value and the
// dictionary key strings are declared identically in
// ManagerApp/Models/DriverManager.swift's PTP diagnostics gateway section —
// keep the two in sync by hand if either changes.
//
#pragma once

#include <CoreAudio/CoreAudio.h>

namespace AES67 {

// FourCharCode 'a67d' ("AES67 Diagnostics"). Custom property selectors just
// need to not collide with any kAudio*PropertySelector constant — this one
// won't, none of Apple's own selectors spell an ASCII word starting 'a67'.
inline constexpr AudioObjectPropertySelector kPTPDiagnosticsPropertySelector = 0x61363764;

// Keys in the CFPropertyListRef (a CFDictionary) this property returns.
// Values: masterClockID/role are CFString; everything else is CFNumber
// (Boolean as 0/1, matching how CFPropertyList already represents bools).
inline constexpr const char* kPTPDiagKeyIsConnected = "isConnected";
inline constexpr const char* kPTPDiagKeyIsLocked = "isLocked";
inline constexpr const char* kPTPDiagKeyMasterClockID = "masterClockID";
inline constexpr const char* kPTPDiagKeyClockClass = "clockClass";
inline constexpr const char* kPTPDiagKeyClockAccuracy = "clockAccuracy";
inline constexpr const char* kPTPDiagKeyOffsetNs = "offsetNs";
inline constexpr const char* kPTPDiagKeyCurrentDomain = "currentDomain";
inline constexpr const char* kPTPDiagKeyRole = "role";                 // "master" or "slave"
inline constexpr const char* kPTPDiagKeyEverWasMaster = "everWasMaster";
inline constexpr const char* kPTPDiagKeyHasCompetitor = "hasCompetitor";
inline constexpr const char* kPTPDiagKeyCompetitorPriority1 = "competitorPriority1";
inline constexpr const char* kPTPDiagKeyCompetitorPriority2 = "competitorPriority2";
inline constexpr const char* kPTPDiagKeySyncMessagesReceived = "syncMessagesReceived";
inline constexpr const char* kPTPDiagKeyAnnounceMessagesReceived = "announceMessagesReceived";

// FourCharCode 'a67s' ("AES67 Sessions") — the SAP discovery gateway, same
// mechanism as the diagnostics one above. Returns a CFArray of
// CFDictionaries, one per session currently being announced on the
// network, so ManagerApp can offer them instead of making the user type a
// multicast address by hand. Sessions that stop being announced drop out
// of the array on their own (SAPListener::kSessionTimeout).
inline constexpr AudioObjectPropertySelector kDiscoveredSessionsPropertySelector = 0x61363773;

// Keys in each element of that array. sessionName/sourceAddress/
// multicastAddress/sdp are CFString; port/ptpDomain are CFNumber.
inline constexpr const char* kSessionKeyName = "sessionName";
inline constexpr const char* kSessionKeySourceAddress = "sourceAddress";
inline constexpr const char* kSessionKeyMulticastAddress = "multicastAddress";
inline constexpr const char* kSessionKeyPort = "port";
inline constexpr const char* kSessionKeyPtpDomain = "ptpDomain";
inline constexpr const char* kSessionKeySDP = "sdp";

} // namespace AES67
