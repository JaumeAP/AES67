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

/// Escapes what this project's JSON writers put inside a quoted string.
///
/// It used to do the quote and the backslash and nothing else, on the reading
/// that a control character in a device name is a bug upstream of here. It is
/// -- and the document this wrote was still one nothing could read back,
/// which is a worse place to find out. What goes through here is labels,
/// identifiers and names out of settings files that nobody validates first,
/// so a newline in one costs a file this repository can no longer parse.
///
/// RFC 8259 sec 7: the quote, the backslash, and everything below a space,
/// which has the named escapes where there is one and \u00XX where there is
/// not. The macOS driver had written half of this again for the same reason.
inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                // Everything below a space has to be escaped, and the only
                // general way to write one is \u00XX (RFC 8259 sec 7). A
                // control character written raw is a document nothing will
                // read back, and these strings come from device names and
                // settings files that nobody validates first.
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                    out += kHex[static_cast<unsigned char>(c) & 0xF];
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

} // namespace AES67
