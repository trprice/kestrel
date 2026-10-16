#pragma once

#include "kestrel/types/error.hpp"
#include "kestrel/types/market.hpp"
#include "kestrel/types/orders.hpp"

namespace kestrel::trading {

// Validates a request and conforms it to the symbol's increments:
//   limit price   buy -> rounded down to tick, sell -> rounded up
//   stop price    rounded to the nearest tick
//   quantity      rounded down to the lot size
// Fails with kInvalidArgument for malformed requests (missing/non-positive
// prices or quantity) and kRejected when the rounded quantity is below the
// symbol's minimum order size.
types::Result<types::OrderRequest> ApplySymbolRules(const types::OrderRequest& request,
                                                    const types::SymbolInfo& info);

}  // namespace kestrel::trading
