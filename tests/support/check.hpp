#pragma once

#include <cstdio>
#include <cstdlib>

#define AW_CHECK(expression)                                                           \
    do {                                                                               \
        if (!(expression)) {                                                           \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,    \
                         #expression);                                                 \
            std::exit(1);                                                              \
        }                                                                              \
    } while (false)
