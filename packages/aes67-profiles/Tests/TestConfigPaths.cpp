//
// TestConfigPaths.cpp
// AES67 profiles
//
// Where the files this tree persists are looked for, and where a new one goes.
//
// The second question is the one that had no answer worth having:
// StreamConfigManager picked /Library/Application Support/AES67Driver for a
// configuration that did not exist yet, and nothing that runs can create that
// directory -- coreaudiod runs as _coreaudiod, and a tool or a test is
// whoever started it. The Manager app makes it, once, through an
// administrator prompt. So the first save failed for everything else and said
// so only in a debug log.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Profiles/ConfigPaths.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace AES67;

namespace {

/// A directory of this test's own, removed when it goes out of scope.
class TempDirectory {
public:
    TempDirectory() {
        char pattern[] = "/tmp/aes67-configpaths-XXXXXX";
        const char* made = ::mkdtemp(pattern);
        REQUIRE(made != nullptr);
        path_ = made;
    }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

bool contains(const std::vector<std::string>& paths, const std::string& wanted) {
    return std::find(paths.begin(), paths.end(), wanted) != paths.end();
}

} // namespace

TEST_CASE("The override comes first and is taken whole") {
    const TempDirectory home;
    ::setenv("AES67_TEST_CONFIG_PATH", "/somewhere/else/named.json", 1);

    const auto paths = configSearchPaths("AES67_TEST_CONFIG_PATH", "streams.json");
    REQUIRE_FALSE(paths.empty());

    // A file, not a directory: the file name is not appended to it.
    CHECK(paths.front() == "/somewhere/else/named.json");

    ::unsetenv("AES67_TEST_CONFIG_PATH");
}

TEST_CASE("systemBeforeHome decides which of the two comes first") {
    const auto homeFirst = configSearchPaths(nullptr, "streams.json", false);
    const auto systemFirst = configSearchPaths(nullptr, "streams.json", true);

    REQUIRE(homeFirst.size() >= 2);
    REQUIRE(systemFirst.size() >= 2);

    const std::string system = "/Library/Application Support/AES67Driver/streams.json";
    CHECK(systemFirst.front() == system);
    CHECK(homeFirst.back() == system);

    // The same set either way, in a different order.
    CHECK(homeFirst.size() == systemFirst.size());
    for (const auto& path : homeFirst) CHECK(contains(systemFirst, path));
}

TEST_CASE("An unwritable override is passed over for one that is writable") {
    const TempDirectory writable;

    // /dev/null is not a directory, so nothing can be created below it: the
    // nearest existing component is a file, and that is the case the walk up
    // has to answer no for rather than keep climbing to /.
    ::setenv("AES67_TEST_CONFIG_PATH", "/dev/null/nowhere/streams.json", 1);
    ::setenv("HOME", writable.path().c_str(), 1);

    const std::string chosen = firstWritableConfigPath("AES67_TEST_CONFIG_PATH", "streams.json");

    CHECK(chosen != "/dev/null/nowhere/streams.json");
    CHECK(contains(configSearchPaths("AES67_TEST_CONFIG_PATH", "streams.json"), chosen));

    ::unsetenv("AES67_TEST_CONFIG_PATH");
}

TEST_CASE("A writable override is taken") {
    const TempDirectory writable;
    const std::string wanted = writable.path() + "/named.json";
    ::setenv("AES67_TEST_CONFIG_PATH", wanted.c_str(), 1);

    CHECK(firstWritableConfigPath("AES67_TEST_CONFIG_PATH", "streams.json") == wanted);

    ::unsetenv("AES67_TEST_CONFIG_PATH");
}

TEST_CASE("A bare file name is judged against the current directory") {
    // configSearchPaths()'s own contract: an override is taken whole and
    // names a file, not a directory, so a bare name with no '/' at all
    // resolves relative to the current directory the same way
    // std::ifstream(path) would read it. This used to answer "not writable"
    // for every such override, whatever the current directory's actual
    // permissions -- the walk that decides never looked at it.
    const TempDirectory cwd;
    const std::string previous = std::filesystem::current_path().string();
    std::filesystem::current_path(cwd.path());

    ::setenv("AES67_TEST_CONFIG_PATH", "bare.json", 1);
    const std::string chosen = firstWritableConfigPath("AES67_TEST_CONFIG_PATH", "streams.json");
    ::unsetenv("AES67_TEST_CONFIG_PATH");

    std::filesystem::current_path(previous);

    CHECK(chosen == "bare.json");
}

TEST_CASE("What comes back is always one of the paths that were searched") {
    const TempDirectory home;
    ::setenv("HOME", home.path().c_str(), 1);

    for (bool systemFirst : {false, true}) {
        const auto searched = configSearchPaths(nullptr, "streams.json", systemFirst);
        const std::string chosen = firstWritableConfigPath(nullptr, "streams.json", systemFirst);

        INFO("systemBeforeHome: " << systemFirst);
        CHECK_FALSE(chosen.empty());
        CHECK(contains(searched, chosen));
    }
}

TEST_CASE("Asking does not create anything") {
    // It is called from a constructor, before anybody has asked for a save.
    // Making the directory there would put one under every HOME that ever
    // loads this driver, whether or not a stream is ever configured.
    const TempDirectory home;
    ::setenv("HOME", home.path().c_str(), 1);

    const std::string chosen = firstWritableConfigPath(nullptr, "streams.json", true);
    REQUIRE_FALSE(chosen.empty());

    CHECK_FALSE(std::filesystem::exists(chosen));
    CHECK_FALSE(std::filesystem::exists(home.path() + "/Library/Application Support/AES67Driver"));
}

TEST_CASE("ensureParentDirectory is what does create it") {
    const TempDirectory home;
    const std::string target = home.path() + "/Library/Application Support/AES67Driver/streams.json";

    REQUIRE(ensureParentDirectory(target, "TestConfigPaths"));
    CHECK(std::filesystem::is_directory(home.path() + "/Library/Application Support/AES67Driver"));

    // Again on a directory that is already there is not a failure.
    CHECK(ensureParentDirectory(target, "TestConfigPaths"));
}
