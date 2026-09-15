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

#include <cstddef>
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

/// Undoes jsonEscape: the reader half of the round trip.
///
/// It was missing for as long as the writer existed, so every reader in this
/// repository took what jsonEscape had produced and handed back something
/// else: `My "Main" Feed` was written correctly and read back as `My \`, and
/// `two\nlines` came back as ten literal characters. Names reach these files
/// straight off the network -- an SDP session name is written into
/// streams.json unvalidated -- so a remote device with a quote in its name was
/// enough to corrupt the configuration the next boot restores.
///
/// `\u00XX` is decoded for the range jsonEscape writes it in (below a space).
/// A `\uXXXX` above 0x7F is left as it was written rather than guessed at:
/// nothing here emits one, and the alternative is inventing an encoding.
inline std::string jsonUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        const char next = s[++i];
        switch (next) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned value = 0;
                bool ok = (i + 4 < s.size());
                for (size_t digit = 1; ok && digit <= 4; ++digit) {
                    const char hex = s[i + digit];
                    if (hex >= '0' && hex <= '9') value = value * 16 + static_cast<unsigned>(hex - '0');
                    else if (hex >= 'a' && hex <= 'f') value = value * 16 + static_cast<unsigned>(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F') value = value * 16 + static_cast<unsigned>(hex - 'A' + 10);
                    else ok = false;
                }
                if (ok && value <= 0x7F) {
                    out.push_back(static_cast<char>(value));
                    i += 4;
                } else {
                    // Not one of ours: keep the text as written.
                    out.push_back('\\');
                    out.push_back('u');
                }
                break;
            }
            default:
                // Not an escape this writes. Keep both characters rather than
                // silently eating the backslash.
                out.push_back('\\');
                out.push_back(next);
        }
    }
    return out;
}

} // namespace AES67
