#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "kestrel/trading/trading_venue.hpp"
#include "kestrel/types/error.hpp"
#include "kestrel/types/market.hpp"
#include "kestrel/types/symbol.hpp"
#include "kestrel/types/time.hpp"
#include "kestrel/types/timeframe.hpp"

namespace kestrel::strategy {

// What a strategy needs from the runner.
struct Requirements {
    std::vector<types::Symbol> symbols;
    // Closed-bar streams to subscribe to for every symbol; empty = none.
    std::vector<types::Timeframe> bar_timeframes;
    bool quotes = false;
    // Closed bars kept per (symbol, timeframe) and exposed through
    // Context::Bars. They are loaded with GetBars before OnStart, so the
    // strategy starts with a full window. At least 1 bar is always kept.
    std::size_t warmup_bars = 0;
};

// What a strategy may touch. Valid only for the duration of a hook call.
class Context {
   public:
    virtual ~Context() = default;

    virtual types::Timestamp Now() const = 0;

    virtual trading::TradingVenue& Venue() = 0;

    virtual types::Result<types::SymbolInfo> SymbolInfo(
        const types::Symbol& symbol) = 0;

    // The last closed bars of the series (at most Requirements::warmup_bars),
    // oldest first. The span is invalidated by the next bar for that series.
    virtual std::span<const types::Bar> Bars(
        const types::Symbol& symbol, types::Timeframe timeframe) const = 0;
};

// All hooks run on the runner's single strategy thread, one at a time, so a
// strategy needs no locking of its own. A hook that throws is logged and the
// runner carries on with the next event. Hooks should return quickly: events
// queue up behind a slow one.
class Strategy {
   public:
    virtual ~Strategy() = default;

    virtual std::string_view Name() const = 0;

    // Called once, before anything is subscribed.
    virtual Requirements Needs() const = 0;

    // After the warmup bars are loaded, before the first event.
    virtual void OnStart(Context&) {}
    // Each closed bar, in order. Bars missed during a disconnect are
    // backfilled, so open times are contiguous.
    virtual void OnBar(Context&, const types::Symbol&, types::Timeframe,
                       const types::Bar&) {}
    virtual void OnQuote(Context&, const types::Quote&) {}
    // Each fill of an order placed through the venue, partial fills included,
    // once. The runner finds fills by polling the venue, so they can arrive
    // some time after the order was accepted.
    virtual void OnFill(Context&, const types::Fill&) {}
    // An order the strategy placed, and that was still active when the venue
    // acknowledged it, changed state or filled further. Its last call for an
    // order has a closed state (filled, cancelled, rejected or expired), so a
    // strategy waiting on an order learns of an asynchronous rejection or an
    // expiry here, and of a cancel it asked for. The runner finds updates by
    // polling the venue, so they can arrive some time after the change, and
    // intermediate states can be skipped. An order's update and its fills are
    // not ordered relative to each other. An acknowledgement that is already
    // closed produces no update: the strategy sees it in PlaceOrder's result.
    virtual void OnOrderUpdate(Context&, const types::OrderStatus&) {}
    // After the last event, when the runner stops.
    virtual void OnStop(Context&) {}
};

}  // namespace kestrel::strategy
