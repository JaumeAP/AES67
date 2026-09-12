//
// NmosRoot.h
// AES67 RAVENNA session layer
// The two listings above the APIs themselves.
//
// A controller does not start at /x-nmos/connection/v1.1/. It starts at
// /x-nmos/, reads which APIs this device has, asks one of them which versions
// it speaks, and only then goes in. Each API here knows its own root and
// nothing about the others, so neither of them can answer those two questions
// -- and answering them with a 404 tells a controller there is no NMOS device
// at this address at all.
//
#pragma once

#include "Ravenna/ConnectionApi.h"

#include <optional>
#include <string>

namespace AES67::Ravenna {

/// Answers `/x-nmos/` with the APIs this device serves and `/x-nmos/<api>/`
/// with the versions of that one. Nothing, when the path goes deeper than
/// that: it belongs to one of the APIs and the caller passes it on.
std::optional<ApiResponse> nmosRootListing(const std::string& method, const std::string& path);

}  // namespace AES67::Ravenna
