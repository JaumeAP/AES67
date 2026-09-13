//
// TestNonBlockingLogger.cpp
// The logger every other package writes through, and which had no tests at all.
//
// Three things a caller depends on: a line comes out in the shape the log
// readers expect, a level below the threshold never reaches the file, and a
// logger that cannot open its file says so instead of silently swallowing
// everything written to it.
//
// The writer thread flushes on its own every 10 ms, so a test that read the
// file straight after log() would be racing it. Every case here closes the
// logger first -- the destructor joins the thread and flushes what is left --
// and reads the file afterwards, which is the only point at which the content
// is settled.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Shared/NonBlockingLogger.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using AES67::LogLevel;
using AES67::NonBlockingLogger;

namespace {

// A path in the system temp directory, unique per case, removed by the guard.
class TempLogFile {
public:
    explicit TempLogFile(const std::string& name)
        : path_(std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                "/aes67_test_" + name + ".log") {
        (void)std::remove(path_.c_str()); // absent is the normal case
    }

    ~TempLogFile() { (void)std::remove(path_.c_str()); }

    TempLogFile(const TempLogFile&) = delete;
    TempLogFile& operator=(const TempLogFile&) = delete;

    const std::string& path() const { return path_; }

    std::vector<std::string> lines() const {
        std::vector<std::string> out;
        std::ifstream in(path_);
        std::string line;
        while (std::getline(in, line)) out.push_back(line);
        return out;
    }

    std::string text() const {
        std::ifstream in(path_);
        std::ostringstream all;
        all << in.rdbuf();
        return all.str();
    }

private:
    std::string path_;
};

} // namespace

TEST_CASE("A written line carries its timestamp, its level and its message") {
    TempLogFile file("shape");

    {
        NonBlockingLogger logger(file.path());
        logger.info("the engine came up");
    }

    const auto lines = file.lines();
    REQUIRE(lines.size() == 1);

    // [YYYY-MM-DD HH:MM:SS.mmm] [INFO] the engine came up
    const std::string& line = lines.front();
    CHECK(line.front() == '[');
    CHECK(line.find("] [INFO] the engine came up") != std::string::npos);

    const std::size_t stampEnd = line.find(']');
    REQUIRE(stampEnd != std::string::npos);
    // "2026-09-13 23:59:59.999" is 23 characters between the brackets.
    CHECK(stampEnd == 24);
    CHECK(line[5] == '-');
    CHECK(line[8] == '-');
    CHECK(line[14] == ':');
    CHECK(line[17] == ':');
    CHECK(line[20] == '.');
}

TEST_CASE("Every level prints under its own name") {
    TempLogFile file("levels");

    {
        NonBlockingLogger logger(file.path());
        logger.setLogLevel(LogLevel::DEBUG);
        logger.debug("d");
        logger.info("i");
        logger.warning("w");
        logger.error("e");
        logger.critical("c");
    }

    const auto lines = file.lines();
    REQUIRE(lines.size() == 5);
    CHECK(lines[0].find("[DEBUG] d") != std::string::npos);
    CHECK(lines[1].find("[INFO] i") != std::string::npos);
    CHECK(lines[2].find("[WARNING] w") != std::string::npos);
    CHECK(lines[3].find("[ERROR] e") != std::string::npos);
    CHECK(lines[4].find("[CRITICAL] c") != std::string::npos);
}

TEST_CASE("A level the logger was not raised to never reaches the file") {
    TempLogFile file("threshold");

    {
        // INFO is the default, so DEBUG is below it and is dropped.
        NonBlockingLogger logger(file.path());
        logger.debug("this one is beneath the threshold");
        logger.info("this one is not");
    }

    const auto lines = file.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("[INFO] this one is not") != std::string::npos);
}

TEST_CASE("Raising the threshold silences the levels below it") {
    TempLogFile file("raised");

    {
        NonBlockingLogger logger(file.path());
        logger.setLogLevel(LogLevel::ERROR);
        logger.debug("no");
        logger.info("no");
        logger.warning("no");
        logger.error("yes");
        logger.critical("yes");
    }

    const auto lines = file.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("[ERROR] yes") != std::string::npos);
    CHECK(lines[1].find("[CRITICAL] yes") != std::string::npos);
}

TEST_CASE("The threshold is read at the moment of the call, not at construction") {
    TempLogFile file("moving-threshold");

    {
        NonBlockingLogger logger(file.path());
        logger.setLogLevel(LogLevel::CRITICAL);
        logger.warning("dropped");
        logger.setLogLevel(LogLevel::DEBUG);
        logger.warning("kept");
    }

    const auto lines = file.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("[WARNING] kept") != std::string::npos);
}

TEST_CASE("A logger that cannot open its file refuses to be built") {
    // A directory that does not exist: ofstream cannot create the file under
    // it, and the constructor is specified to throw rather than hand back a
    // logger whose every write disappears.
    CHECK_THROWS_AS(NonBlockingLogger("/nonexistent-directory-aes67/driver.log"),
                    std::runtime_error);
}

TEST_CASE("A second logger appends rather than truncating what the first wrote") {
    TempLogFile file("append");

    {
        NonBlockingLogger logger(file.path());
        logger.info("first run");
    }
    {
        NonBlockingLogger logger(file.path());
        logger.info("second run");
    }

    const auto lines = file.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("first run") != std::string::npos);
    CHECK(lines[1].find("second run") != std::string::npos);
}

TEST_CASE("Order is preserved across a burst larger than one writer pass") {
    TempLogFile file("burst");

    constexpr int kMessages = 500;
    {
        NonBlockingLogger logger(file.path());
        for (int i = 0; i < kMessages; ++i) {
            logger.info("message " + std::to_string(i));
        }
    }

    const auto lines = file.lines();
    REQUIRE(lines.size() == static_cast<std::size_t>(kMessages));
    for (int i = 0; i < kMessages; ++i) {
        CHECK(lines[static_cast<std::size_t>(i)].find("message " + std::to_string(i)) !=
              std::string::npos);
    }
}

TEST_CASE("An empty message still produces its line") {
    TempLogFile file("empty-message");

    {
        NonBlockingLogger logger(file.path());
        logger.error("");
    }

    const auto lines = file.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("[ERROR] ") != std::string::npos);
}

TEST_CASE("A logger that was never written to leaves an empty file behind") {
    TempLogFile file("silent");

    { NonBlockingLogger logger(file.path()); }

    CHECK(file.text().empty());
}

TEST_CASE("The logging macros are inert while the global logger is absent") {
    // Shared/NonBlockingLogger.h declares g_logger and leaves it null; the
    // macros guard on it, so a package that logs before installing one must
    // not fault.
    REQUIRE(AES67::g_logger == nullptr);
    LOG_INFO("nobody is listening");
    LOG_CRITICAL("still nobody");
    CHECK(AES67::g_logger == nullptr);
}

TEST_CASE("The macros write through the global logger once it is installed") {
    TempLogFile file("global");

    AES67::g_logger = std::make_unique<NonBlockingLogger>(file.path());
    AES67::g_logger->setLogLevel(LogLevel::DEBUG);
    LOG_DEBUG("debug through the macro");
    LOG_INFO("info through the macro");
    LOG_WARNING("warning through the macro");
    LOG_ERROR("error through the macro");
    LOG_CRITICAL("critical through the macro");
    AES67::g_logger.reset();

    const auto lines = file.lines();
    REQUIRE(lines.size() == 5);
    CHECK(lines[0].find("[DEBUG] debug through the macro") != std::string::npos);
    CHECK(lines[4].find("[CRITICAL] critical through the macro") != std::string::npos);
}
