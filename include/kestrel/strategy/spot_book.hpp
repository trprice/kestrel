#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "kestrel/strategy/strategy.hpp"
#include "kestrel/types/decimal.hpp"

// Bookkeeping for long-only spot strategies that trade several symbols
// against one quote asset (momentum_rotation, trend_ensemble).
namespace kestrel::strategy {

// What the strategy holds of each symbol and the orders it has out.
//
// Only what the strategy bought itself counts as held, as the account's
// other balances may belong to someone else, unless AdoptHoldings is told
// the account is the strategy's own. Prices are the last close of
// `timeframe`.
class SpotBook {
   public:
    // A symbol without a quote asset (a stock, "SPY") is quoted in
    // `default_quote`. Throws std::invalid_argument when a symbol has no
    // quote asset and there is no default, is listed twice, or the quote
    // assets differ.
    SpotBook(const std::vector<types::Symbol>& symbols,
             types::Timeframe timeframe, const std::string& default_quote = {});

    const std::string& Quote() const { return mQuote; }

    // For OnStart: with `adopt`, counts the universe's coins the account
    // holds (their free part) as held, so a restarted run sells or keeps
    // them; without, warns that they are neither counted nor sold.
    void AdoptHoldings(Context& context, bool adopt);

    // An order found closed without its closing update.
    struct Closed {
        bool sell = false;
        types::Symbol symbol;
    };
    // Asks the venue about each order still out (one whose closing update
    // never came would hold up every rebalance), counts what those found
    // closed filled, and drops them.
    std::vector<Closed> Reconcile(Context& context);
    bool OrdersOut() const { return !mOrders.empty(); }
    std::size_t OrderCount() const { return mOrders.size(); }
    bool SellOut() const;

    // Bought and not yet sold.
    types::Decimal Held(const types::Symbol& symbol) const;
    // What of `symbol` can be sold: held, to the lot size; zero for dust
    // under the minimum order.
    types::Decimal Sellable(Context& context,
                            const types::Symbol& symbol) const;
    std::optional<types::Decimal> LastClose(Context& context,
                                            const types::Symbol& symbol) const;
    // The total quote balance (free and locked) plus holdings at their last
    // close; nullopt if the balances cannot be read or a holding has no
    // price.
    std::optional<types::Decimal> Equity(Context& context) const;
    std::optional<types::Decimal> FreeCash(Context& context) const;

    // Places a market order and logs it; its ID if the venue took it. It is
    // tracked (OrdersOut) until its fills total its quantity, its closing
    // update comes, or Reconcile finds it closed: even one the venue reports
    // filled at once, until its fills are counted.
    std::optional<std::string> Place(Context& context,
                                     const types::Symbol& symbol,
                                     types::Side side,
                                     const types::Decimal& quantity);

    // Counts a fill of one of the universe's symbols. A fee taken in the
    // coin (Binance.US takes a buy's fee in what was bought) leaves less
    // held than was filled.
    void OnFill(const types::Fill& fill);
    // An order of ours closed: what of it filled counted (once, whether its
    // fills come before or after), dropped, and returned, with whether it
    // filled completely. An order whose fills already totalled its quantity
    // was dropped then, and is not returned.
    struct Done {
        bool sell = false;
        bool filled = false;
        types::Symbol symbol;
    };
    std::optional<Done> OnOrderUpdate(const types::OrderStatus& status);

   private:
    const types::Timeframe mTimeframe;
    std::string mQuote;                          // the shared quote asset
    std::map<types::Symbol, std::string> mBase;  // each symbol's base asset
    std::map<types::Symbol, types::Decimal> mHeld;
    struct OrderInfo {
        bool sell = false;
        types::Symbol symbol;
        types::Decimal quantity;  // ordered
        types::Decimal filled;    // seen as fills
    };
    // Counts what a closed order's status says filled beyond the fills seen,
    // and notes it so those fills are not counted again when they come.
    void CountFilled(const std::string& order_id, const OrderInfo& info,
                     const types::Symbol& symbol,
                     const types::Decimal& filled_quantity);
    std::map<std::string, OrderInfo> mOrders;  // active orders by ID
    // Of orders found closed by Reconcile, what was counted from their status
    // and not yet seen as fills: those fills are not counted again.
    std::map<std::string, types::Decimal> mCounted;
};

// Gathers one period's bars across a universe that arrive one at a time.
// A period is complete once every symbol's bar for it is in, or once a bar
// of the next arrives (a symbol without one is then left out). A symbol that
// missed a period, halted or not listed yet, is not waited for again until
// it sends a bar, so it holds up only the first period it misses rather than
// every one after. Bars backfilled symbol by symbol after a disconnect can
// shrink the set the same way (the first symbol's bars complete each
// period alone); the others are waited for again from their next bar, so
// that costs a period, not a symbol.
class PeriodGate {
   public:
    explicit PeriodGate(std::size_t symbols) : mSymbols(symbols) {}
    // The bar of `symbol` opening at `open_time`: the periods this
    // completes, oldest first (the last one, late, and this one, if its bar
    // was the only one missing). Each period is returned once; a bar of a
    // period already returned or older is ignored.
    std::vector<types::Timestamp> Add(const types::Symbol& symbol,
                                      types::Timestamp open_time);

   private:
    // Whether mSeen holds every symbol waited for.
    bool Complete() const;

    const std::size_t mSymbols;
    std::optional<types::Timestamp> mPeriod;  // being collected
    std::set<types::Symbol> mSeen;            // with a bar for it so far
    bool mDone = false;                       // returned already
    // Those with a bar for the last complete period (and any since): the
    // ones waited for. Until a period has completed, all of them.
    std::set<types::Symbol> mActive;
};

}  // namespace kestrel::strategy
