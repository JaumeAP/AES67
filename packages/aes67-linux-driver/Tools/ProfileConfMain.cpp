//
// ProfileConfMain.cpp
// aes67-linux-driver
// aes67-profile-conf: write the daemon's configuration for a profile.
//

#include "Tools/ConfCheck.h"
#include "Tools/ProfileConf.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace AES67;
using namespace AES67::LinuxDriver;

namespace {

int usage(std::ostream& out, int code) {
    out << "usage: aes67-profile-conf --profile <id> [--rate Hz] [--ptime us]\n"
           "                         [--interface name] [--base file] [-o file]\n"
           "       aes67-profile-conf --check <file> [--profile <id>]\n"
           "\n"
           "Rewrites the daemon's configuration for a compatibility profile.\n"
           "Every key the profile does not determine is left as the base file\n"
           "has it, which by default is the one the vendored daemon ships.\n"
           "\n"
           "--check reads a configuration back instead and reports what is\n"
           "wrong with it: with a profile, what the profile forbids as well.\n"
           "It exits 1 when anything is an error, 0 when only warnings are.\n"
           "\n"
           "Profiles:\n";
    for (const auto& profile : CompatibilityProfile::all()) {
        out << "  " << CompatibilityProfile::kindToString(profile.kind)
            << "\t" << profile.displayName << "\n";
    }
    return code;
}

bool readFile(const std::string& path, std::string& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    ConfRequest request;
    std::string profileName;
    std::string basePath = AES67_DEFAULT_DAEMON_CONF;
    std::string outPath;
    std::string checkPath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "aes67-profile-conf: " << what << " wants a value\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--profile")        profileName = value("--profile");
        else if (arg == "--rate")      request.sampleRate = std::stod(value("--rate"));
        else if (arg == "--ptime")     request.ptimeUs =
                                           static_cast<uint32_t>(std::stoul(value("--ptime")));
        else if (arg == "--interface") request.interfaceName = value("--interface");
        else if (arg == "--base")      basePath = value("--base");
        else if (arg == "--check")     checkPath = value("--check");
        else if (arg == "-o")          outPath = value("-o");
        else if (arg == "-h" || arg == "--help") return usage(std::cout, 0);
        else {
            std::cerr << "aes67-profile-conf: unknown argument " << arg << "\n";
            return usage(std::cerr, 2);
        }
    }

    if (!profileName.empty() && !isKnownProfileName(profileName)) {
        std::cerr << "aes67-profile-conf: unknown profile " << profileName << "\n";
        return usage(std::cerr, 2);
    }
    if (!profileName.empty()) request.kind = CompatibilityProfile::kindFromString(profileName);

    if (!checkPath.empty()) {
        std::string conf;
        std::string readError;
        if (!readFile(checkPath, conf, readError)) {
            std::cerr << "aes67-profile-conf: " << readError << "\n";
            return 1;
        }

        std::optional<CompatibilityProfileKind> kind;
        if (!profileName.empty()) kind = request.kind;

        const auto findings = checkConf(conf, kind, [](const std::string& path) {
            return std::ifstream(path).good() || std::filesystem::exists(path);
        });
        for (const auto& finding : findings) {
            std::cout << (finding.severity == Severity::Error ? "error" : "warning") << ": "
                      << (finding.key.empty() ? checkPath : finding.key) << ": "
                      << finding.message << "\n";
        }
        if (findings.empty()) std::cout << checkPath << ": nothing to report\n";
        return hasError(findings) ? 1 : 0;
    }

    if (profileName.empty()) return usage(std::cerr, 2);

    std::string base;
    std::string error;
    if (!readFile(basePath, base, error)) {
        std::cerr << "aes67-profile-conf: " << error << "\n";
        return 1;
    }

    const ConfResult result = applyProfile(base, request);
    if (!result.ok) {
        std::cerr << "aes67-profile-conf: " << result.error << "\n";
        return 1;
    }

    if (outPath.empty()) {
        std::cout << result.text;
        return 0;
    }

    std::ofstream out(outPath);
    if (!out) {
        std::cerr << "aes67-profile-conf: cannot write " << outPath << "\n";
        return 1;
    }
    out << result.text;
    return 0;
}
