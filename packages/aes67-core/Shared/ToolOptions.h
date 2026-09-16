//
// ToolOptions.h
// AES67 core
// Command-line values, read once and checked, for the executables in this
// tree: the tools under packages/aes67-macos-driver/Tools, the two PTP
// daemons, and packages/aes67-ravenna's announcer.
//
// They all used to read their numbers with atoi()/atof(), which have no way
// of saying "that was not a number": both answer 0. So `--duration abc` ran
// AES67TestReceiver forever instead of the ten seconds it had just printed,
// `--port abc` bound port 0, and `--freq x` sent a DC offset. A flag given
// as the last word on the command line was worse than silent: the `i + 1 <
// argc` guard was part of the match, so the flag fell through to the
// unknown-option branch and was reported as an option nobody had heard of,
// which is not what happened.
//
// Header-only and free of everything but the C library: these are programs
// with a main(), not a library, and the only thing they share is this. It
// lives in the core for the same reason the profiles do -- three packages
// need it, and none of them should carry a copy.
//
#pragma once

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace AES67 {
namespace ToolOptions {

/// The value that follows the flag at argv[i], advancing i past it. Null
/// when the flag was the last word on the command line, with the reason
/// already on stderr.
inline const char* value(int argc, char* argv[], int& i) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s needs a value\n", argv[i]);
        return nullptr;
    }
    return argv[++i];
}

/// The value that follows the flag, as a whole number in [min, max].
///
/// Everything atoi() would have turned into a silent 0 is refused here: an
/// empty string, a word, a number with a unit stuck to the end of it, and a
/// number that does not fit what the caller is about to store it in.
inline bool integerOption(int argc, char* argv[], int& i,
                          long long min, long long max, long long& out) {
    const char* flag = argv[i];
    const char* text = value(argc, argv, i);
    if (text == nullptr) return false;

    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text, &end, 10);

    if (end == text || *end != '\0' || errno == ERANGE) {
        std::fprintf(stderr, "Error: %s wants a whole number, not \"%s\"\n", flag, text);
        return false;
    }
    if (parsed < min || parsed > max) {
        std::fprintf(stderr, "Error: %s must be between %lld and %lld, not %lld\n",
                     flag, min, max, parsed);
        return false;
    }

    out = parsed;
    return true;
}

/// The value that follows the flag, as a number in [min, max]. Same refusals
/// as integerOption, and the range is the caller's because what a frequency
/// or a duration may be is the tool's business, not this file's.
inline bool realOption(int argc, char* argv[], int& i,
                       double min, double max, double& out) {
    const char* flag = argv[i];
    const char* text = value(argc, argv, i);
    if (text == nullptr) return false;

    char* end = nullptr;
    const double parsed = std::strtod(text, &end);

    // Not errno == ERANGE: strtod sets it for underflow as well as overflow,
    // and an underflow has still read the number -- "1e-320" comes back as a
    // representable subnormal with ERANGE set, and refusing it as "not a
    // number" is a refusal whose reason is false. Overflow is the case that
    // has to be caught, and it is the one that comes back not finite.
    if (end == text || *end != '\0' || !std::isfinite(parsed)) {
        std::fprintf(stderr, "Error: %s wants a number, not \"%s\"\n", flag, text);
        return false;
    }
    if (!(parsed >= min && parsed <= max)) {
        // Written as a positive test so that a NaN, which compares false
        // against everything, is refused here rather than stored.
        std::fprintf(stderr, "Error: %s must be between %g and %g, not \"%s\"\n",
                     flag, min, max, text);
        return false;
    }

    out = parsed;
    return true;
}

/// The value that follows the flag, as an unsigned number up to max, written
/// in whatever base it announces: an SSRC is quoted in hexadecimal as often
/// as in decimal, and RFC 3550 gives it no preferred form.
inline bool unsignedOption(int argc, char* argv[], int& i,
                           unsigned long long max, unsigned long long& out) {
    const char* flag = argv[i];
    const char* text = value(argc, argv, i);
    if (text == nullptr) return false;

    // strtoull accepts a leading '-' and wraps it around, which is how
    // `--ssrc -1` would otherwise have become 0xFFFFFFFF. It also skips
    // leading whitespace before looking at the sign, so the minus has to be
    // looked for where strtoull would look for it and not only at text[0]:
    // `--ssrc " -1"` is one argument, and it wrapped.
    const char* sign = text;
    while (std::isspace(static_cast<unsigned char>(*sign))) ++sign;
    if (*sign == '-') {
        std::fprintf(stderr, "Error: %s wants an unsigned number, not \"%s\"\n", flag, text);
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);

    if (end == text || *end != '\0' || errno == ERANGE) {
        std::fprintf(stderr, "Error: %s wants an unsigned number, not \"%s\"\n", flag, text);
        return false;
    }
    if (parsed > max) {
        std::fprintf(stderr, "Error: %s must be at most %llu, not %llu\n", flag, max, parsed);
        return false;
    }

    out = parsed;
    return true;
}

/// The one wording for an option none of these programs knows. They said it
/// six different ways -- "Unknown option: x (use --help)", "unknown option:
/// x" -- which is a difference with nothing behind it, and a suite that
/// checks what a program says has to know which spelling each one picked.
inline void unknownOption(const char* flag) {
    std::fprintf(stderr, "Unknown option: %s (use --help)\n", flag);
}

} // namespace ToolOptions
} // namespace AES67
