//
// TestResourceCleanup.cpp
// AES67 macOS Driver
//
// The teardown plumbing: the manager the driver registers its sockets and
// threads with, and the RAII guards that close a resource when a function
// leaves by any path.
//
// This runs while the driver is being torn down inside coreaudiod, which is
// where an exception or a double close costs the whole audio daemon rather
// than one stream — and it was at zero coverage until 2026-09-04. The
// invariants that matter are that a cleanup runs exactly once, that one
// throwing cleanup does not take the others with it, and that a released
// guard does nothing.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Shared/ResourceCleanupManager.h"

#include <fcntl.h>
#include <unistd.h>

#include <stdexcept>
#include <string>
#include <vector>

using namespace AES67;

TEST_CASE("A registered cleanup runs, once") {
    ResourceCleanupManager manager;
    int runs = 0;

    manager.registerCleanup([&runs] { ++runs; });
    CHECK(manager.hasPendingCleanup());
    CHECK(manager.getCleanupCount() == 1);

    manager.performCleanup();
    CHECK(runs == 1);

    // The list is emptied as it is run, so asking again does not run it
    // again — a second close of the same socket is what this prevents.
    CHECK_FALSE(manager.hasPendingCleanup());
    CHECK(manager.getCleanupCount() == 0);
    manager.performCleanup();
    CHECK(runs == 1);
}

TEST_CASE("Cleanups run in the order they were registered") {
    ResourceCleanupManager manager;
    std::vector<int> order;

    manager.registerCleanup([&order] { order.push_back(1); });
    manager.registerCleanup([&order] { order.push_back(2); });
    manager.registerCleanup([&order] { order.push_back(3); });
    manager.performCleanup();

    CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("An empty function is not registered") {
    // A default-constructed std::function would throw when called, in the
    // middle of teardown.
    ResourceCleanupManager manager;
    manager.registerCleanup(CleanupFunction{});

    CHECK_FALSE(manager.hasPendingCleanup());
    manager.performCleanup();  // nothing to call, and nothing thrown
}

TEST_CASE("One cleanup that throws does not stop the rest") {
    // Teardown is exactly where a throw is plausible — a thread join, a
    // container touched while something else is shutting down — and the
    // sockets registered after it still have to be closed.
    ResourceCleanupManager manager;
    bool before = false;
    bool after = false;

    manager.registerCleanup([&before] { before = true; });
    manager.registerCleanup([] { throw std::runtime_error("during cleanup"); });
    manager.registerCleanup([&after] { after = true; });

    CHECK_NOTHROW(manager.performCleanup());
    CHECK(before);
    CHECK(after);
}

TEST_CASE("A cleanup that throws something that is not an exception is caught too") {
    ResourceCleanupManager manager;
    bool after = false;

    manager.registerCleanup([] { throw 42; });
    manager.registerCleanup([&after] { after = true; });

    CHECK_NOTHROW(manager.performCleanup());
    CHECK(after);
}

TEST_CASE("A cleanup registered from inside a cleanup is not lost") {
    // The list is swapped out before it is run, so a cleanup that registers
    // another leaves the new one pending rather than running it in the same
    // pass or dropping it.
    ResourceCleanupManager manager;
    bool nested = false;

    manager.registerCleanup([&manager, &nested] {
        manager.registerCleanup([&nested] { nested = true; });
    });

    manager.performCleanup();
    CHECK_FALSE(nested);
    CHECK(manager.hasPendingCleanup());

    manager.performCleanup();
    CHECK(nested);
}

TEST_CASE("performCleanupAndReset leaves nothing behind") {
    ResourceCleanupManager manager;
    int runs = 0;
    manager.registerCleanup([&runs] { ++runs; });

    manager.performCleanupAndReset();
    CHECK(runs == 1);
    CHECK_FALSE(manager.hasPendingCleanup());
}

TEST_CASE("The destructor cleans up what is still pending") {
    int runs = 0;
    {
        ResourceCleanupManager manager;
        manager.registerCleanup([&runs] { ++runs; });
    }
    CHECK(runs == 1);
}

TEST_CASE("A guard cleans up when it goes out of scope") {
    bool cleaned = false;
    {
        ResourceGuard<std::function<void()>> guard([&cleaned] { cleaned = true; });
        CHECK_FALSE(cleaned);
    }
    CHECK(cleaned);
}

TEST_CASE("A released guard does nothing, and an explicit cleanup happens once") {
    bool released = false;
    {
        ResourceGuard<std::function<void()>> guard([&released] { released = true; });
        guard.release();
    }
    CHECK_FALSE(released);

    int runs = 0;
    {
        ResourceGuard<std::function<void()>> guard([&runs] { ++runs; });
        guard.cleanup();
        CHECK(runs == 1);
    }
    CHECK(runs == 1);  // the destructor does not repeat it
}

TEST_CASE("A socket guard closes the descriptor it was given") {
    const int fd = ::open("/dev/null", O_RDONLY);
    REQUIRE(fd >= 0);

    {
        SocketGuard guard(fd);
        CHECK(::fcntl(fd, F_GETFD) != -1);  // still open inside the scope
    }

    // Closed on the way out: the descriptor is no longer valid.
    CHECK(::fcntl(fd, F_GETFD) == -1);
}

TEST_CASE("A released socket guard leaves the descriptor open") {
    const int fd = ::open("/dev/null", O_RDONLY);
    REQUIRE(fd >= 0);

    {
        SocketGuard guard(fd);
        guard.release();
    }

    CHECK(::fcntl(fd, F_GETFD) != -1);
    ::close(fd);
}

TEST_CASE("A memory guard frees once, with the deleter it was given") {
    int frees = 0;
    int value = 7;
    {
        MemoryGuard guard(&value, [&frees](void*) { ++frees; });
        guard.free();
        CHECK(frees == 1);
    }
    CHECK(frees == 1);  // not freed a second time on the way out

    int released = 0;
    {
        MemoryGuard guard(&value, [&released](void*) { ++released; });
        guard.release();
    }
    CHECK(released == 0);
}
