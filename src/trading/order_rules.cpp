#include "kestrel/trading/order_rules.hpp"

namespace kestrel::trading {

using types::ErrorCategory;
using types::MakeError;
using types::OrderType;
using types::Rounding;
using types::Side;

types::Result<types::OrderRequest> ApplySymbolRules(const types::OrderRequest& request,
                                                    const types::SymbolInfo& info) {
  if (info.tick_size.Sign() <= 0 || info.lot_size.Sign() <= 0)
  {
    return MakeError(ErrorCategory::kInternal, "symbol " + request.symbol + " has non-positive tick or lot size");
  }
  if (request.quantity.Sign() <= 0)
  {
    return MakeError(ErrorCategory::kInvalidArgument, "order quantity must be positive");
  }

  types::OrderRequest out = request;
  if (request.type == OrderType::kLimit)
  {
    if (!request.limit_price || request.limit_price->Sign() <= 0)
    {
      return MakeError(ErrorCategory::kInvalidArgument, "limit order needs a positive limit price");
    }
    out.limit_price = request.limit_price->RoundToIncrement(
        info.tick_size, request.side == Side::kBuy ? Rounding::kDown : Rounding::kUp);
    if (out.limit_price->Sign() <= 0)
    {
      return MakeError(ErrorCategory::kRejected, "limit price rounds to zero at tick size " + info.tick_size.ToString());
    }
  }
  if (request.type == OrderType::kStop)
  {
    if (!request.stop_price || request.stop_price->Sign() <= 0)
    {
      return MakeError(ErrorCategory::kInvalidArgument, "stop order needs a positive stop price");
    }
    out.stop_price = request.stop_price->RoundToIncrement(info.tick_size, Rounding::kNearest);
    if (out.stop_price->Sign() <= 0)
    {
      return MakeError(ErrorCategory::kRejected, "stop price rounds to zero at tick size " + info.tick_size.ToString());
    }
  }

  out.quantity = request.quantity.RoundToIncrement(info.lot_size, Rounding::kDown);
  if (out.quantity.Sign() <= 0 || out.quantity < info.min_order_size)
  {
    return MakeError(ErrorCategory::kRejected,
                     "quantity " + request.quantity.ToString() + " is below the minimum order size " +
                         info.min_order_size.ToString() + " (lot size " + info.lot_size.ToString() + ")");
  }
  return out;
}

}  // namespace kestrel::trading
