#include "Options.h"

#include "Shared/ToolOptions.h"

#include <cstdio>
#include <string>

namespace AES67::LinuxPtpd {

void usage() {
    std::fprintf(stderr,
                 "usage: aes67-ptpd [--interface NAME] [--profile NAME]\n"
                 "                  [--priority1 N] [--priority2 N] [--utc-offset N]\n"
                 "                  [--phc /dev/ptpN] [--reference]\n"
                 "                  [--reference-channel N]\n"
                 "                  [--allow-software-timestamps] [--verbose]\n"
                 "\n"
                 "profiles: aes67, aes67-tight, default1588, gptp\n"
                 "          (packages/aes67-profiles holds the numbers)\n");
}

CommandLineResult parseCommandLine(int argc, char** argv, CommandLine& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const char* value = nullptr;
        long long number = 0;

        if (option == "--help" || option == "-h") {
            usage();
            return CommandLineResult::UsagePrinted;
        } else if (option == "--allow-software-timestamps") {
            out.allowSoftwareTimestamps = true;
        } else if (option == "--verbose" || option == "-v") {
            out.config.verbose = true;
        } else if (option == "--reference") {
            out.useReference = true;
        } else if (option == "--reference-channel") {
            if (!ToolOptions::integerOption(argc, argv, i, 0, 4294967295LL, number)) {
                return CommandLineResult::Bad;
            }
            out.useReference = true;
            out.referenceChannel = static_cast<unsigned int>(number);
        } else if (option == "--interface") {
            if ((value = ToolOptions::value(argc, argv, i)) == nullptr) {
                return CommandLineResult::Bad;
            }
            out.config.interfaceName = value;
        } else if (option == "--profile") {
            // Which names are legal is the shared profile table's answer,
            // given when the grandmaster starts and fails with the list in
            // hand; repeating it here would be a second copy to keep.
            if ((value = ToolOptions::value(argc, argv, i)) == nullptr) {
                return CommandLineResult::Bad;
            }
            out.config.profileName = value;
        } else if (option == "--priority1") {
            // IEEE 1588 §8.2.1.3: one octet, and the first field of the BMCA
            // comparison. Lower wins, so a value that wrapped into this range
            // is a box that quietly took over the segment.
            if (!ToolOptions::integerOption(argc, argv, i, 0, 255, number)) {
                return CommandLineResult::Bad;
            }
            out.config.priority1 = static_cast<uint8_t>(number);
        } else if (option == "--priority2") {
            if (!ToolOptions::integerOption(argc, argv, i, 0, 255, number)) {
                return CommandLineResult::Bad;
            }
            out.config.priority2 = static_cast<uint8_t>(number);
        } else if (option == "--utc-offset") {
            // currentUtcOffset is a signed 16-bit field on the wire.
            if (!ToolOptions::integerOption(argc, argv, i, -32768, 32767, number)) {
                return CommandLineResult::Bad;
            }
            out.config.currentUtcOffset = static_cast<int16_t>(number);
        } else if (option == "--phc") {
            if ((value = ToolOptions::value(argc, argv, i)) == nullptr) {
                return CommandLineResult::Bad;
            }
            out.phcDevice = value;
        } else {
            ToolOptions::unknownOption(option.c_str());
            usage();
            return CommandLineResult::Bad;
        }
    }
    return CommandLineResult::Ok;
}

}  // namespace AES67::LinuxPtpd
