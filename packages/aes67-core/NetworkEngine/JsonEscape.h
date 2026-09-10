//
// JsonEscape.h
// AES67 core
//
// The one thing three files did to a string before writing it into JSON.
//
// It was written out three times -- twice in this package and once in the
// driver -- identically, which is the only reason the three agreed. A JSON
// writer that escapes a quote in one file and not in another produces a
// document that parses on one side of a link and not the other, and the file
// that got it wrong is the one nobody reads.
//
#pragma once

#include <string>

namespace AES67 {

/// Escapes what this project's JSON writers put inside a quoted string: the
/// quote itself and the backslash.
///
/// Deliberately not a full RFC 8259 escaper. What goes through here is
/// labels, identifiers and device names -- text a person typed -- and a
/// control character in one of those is a bug upstream of this function, not
/// something to encode and pass on. Widening it is a change of contract for
/// three call sites, so it wants its own commit and its own reason.
inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

} // namespace AES67
