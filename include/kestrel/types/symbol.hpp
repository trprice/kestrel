#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "kestrel/types/error.hpp"

namespace kestrel::types {

// Normalized symbol format used everywhere outside provider classes:
//   crypto pairs   BASE/QUOTE   e.g. BTC/USD, ETH/USDT
//   equities       TICKER       e.g. AAPL, BRK.B
// Upper case, no whitespace. Provider classes translate to/from their native
// format (see kestrel/data/symbols.hpp).
using Symbol = std::string;

struct SymbolParts {
    std::string base;
    std::optional<std::string> quote;  // empty for equities
};

// Trims, upper-cases, and maps '-' and '_' separators to '/'. Fails with
// kInvalidArgument if the result is not a valid normalized symbol.
Result<Symbol> NormalizeSymbol(std::string_view text);

// Splits an already-normalized symbol.
Result<SymbolParts> SplitSymbol(std::string_view symbol);

std::string JoinSymbol(std::string_view base, std::string_view quote);

}  // namespace kestrel::types
