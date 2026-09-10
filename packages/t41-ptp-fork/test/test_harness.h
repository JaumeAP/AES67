#pragma once

#include <cstdio>

// Counters live in test_main.cpp.
extern int checksRun;
extern int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checksRun++;                                                         \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        checksRun++;                                                         \
        const long long a_ = (long long)(actual);                            \
        const long long e_ = (long long)(expected);                          \
        if (a_ != e_) {                                                      \
            std::printf("FAIL %s:%d: %s == %lld, expected %lld\n",           \
                        __FILE__, __LINE__, #actual, a_, e_);                \
            failures++;                                                      \
        }                                                                    \
    } while (0)
