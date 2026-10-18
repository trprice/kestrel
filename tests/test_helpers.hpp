#pragma once

#include <gtest/gtest.h>

#include "kestrel/types/decimal.hpp"
#include "kestrel/types/error.hpp"

namespace kestrel::testing {

// Parses a decimal literal; fails the test on malformed input.
inline types::Decimal D(const char* text) {
    auto d = types::Decimal::Parse(text);
    if (!d) {
        throw std::runtime_error(d.error().ToString());
    }
    return *d;
}

}  // namespace kestrel::testing

// Unwraps a Result in a test, failing with the error text instead of crashing.
#define ASSERT_OK(expr)                                       \
    do {                                                      \
        const auto& _r = (expr);                              \
        ASSERT_TRUE(_r.has_value()) << _r.error().ToString(); \
    } while (0)
