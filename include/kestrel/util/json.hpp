#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

#include "kestrel/types/decimal.hpp"
#include "kestrel/types/error.hpp"

namespace kestrel::util {

// Parses JSON without ever converting a fractional number to a double: every
// non-integer number is stored as a string holding its original text, so it
// can be handed to Decimal::Parse losslessly. Integers stay integers.
types::Result<nlohmann::json> ParseJson(std::string_view text);

// Reads a Decimal from a JSON string or integer (what ParseJson produces for
// numbers). Fails with kParse on any other type.
types::Result<types::Decimal> JsonDecimal(const nlohmann::json& value);

}  // namespace kestrel::util
