#pragma once

#include <cstdio>
#include <cstdlib>

// A violated invariant is a finding: abort, so libFuzzer keeps the input.
#define FUZZ_REQUIRE(condition)                                                                      \
    do {                                                                                             \
        if (!(condition)) {                                                                          \
            std::fprintf(stderr, "fuzz invariant failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
            std::abort();                                                                            \
        }                                                                                            \
    } while (false)
