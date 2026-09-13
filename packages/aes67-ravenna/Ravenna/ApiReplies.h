//
// ApiReplies.h
// AES67 RAVENNA session layer
// What all three NMOS APIs do the same way.
//
// IS-04, IS-05 and IS-08 are three specifications and one shape: a path of
// segments, a JSON body, and an error object with a code, a summary and a
// detail. Each of the three files that serve them had written that shape out
// for itself -- three segmentsOf, three jsonResponse, three errorResponse --
// and the copies had already begun to disagree about which status codes get
// which summary.
//
// Text handling is here for the same reason: lower case and trimming are what
// every header parser in this package needs, and there were three of the first
// and two of the second.
//
#pragma once

#include "Ravenna/ConnectionApi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace AES67::Ravenna {

/// One JSON answer.
ApiResponse jsonResponse(int status, const JsonValue& value);

/// An error as NMOS writes one: the code, a summary of it, and a detail a
/// controller shows to a person.
ApiResponse errorResponse(int status, const std::string& detail);

/// A listing, which is what every collection endpoint answers with.
ApiResponse listResponse(const std::vector<std::string>& entries);

/// A path split into its segments, dropping the empty ones a trailing slash
/// leaves behind.
std::vector<std::string> segmentsOf(const std::string& path);

/// An IPv4 address in host byte order, written out.
std::string addressText(uint32_t addressV4);

/// Lower case, ASCII only, for the comparisons a protocol says ignore it:
/// HTTP and RTSP header names, DNS labels, SDP attribute keys.
std::string lowered(std::string text);

/// Without the spaces, tabs and line endings at either end.
std::string trimmed(const std::string& text);

}  // namespace AES67::Ravenna
