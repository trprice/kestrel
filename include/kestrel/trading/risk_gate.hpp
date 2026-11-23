#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "kestrel/trading/pnl_tracker.hpp"
#include "kestrel/trading/trading_venue.hpp"
#include "kestrel/types/decimal.hpp"
#include "kestrel/types/market.hpp"
#include "kestrel/types/market_hours.hpp"
#include "kestrel/types/symbol.hpp"
#include "kestrel/types/time.hpp"

namespace kestrel::trading {

// Every limit is optional; an unset one is not checked. Amounts are in the
// quote asset and assume all traded symbols share one quote asset.
struct RiskLimits {
    // Price * quantity of a single order.
    std::optional<types::Decimal> max_order_notional;
    // Per symbol: what is held, plus open buy orders, plus the new order.
    std::optional<types::Decimal> max_position_notional;
    // All symbols together: each symbol's position as max_position_notional
    // counts it (on its larger side, with shorts), valued at its reference
    // price, the new order's symbol at the order's price. Fails closed if a
    // symbol held or with an order open has no reference price.
    std::optional<types::Decimal> max_total_notional;
    // A symbol's position, as max_position_notional counts it, as a share of
    // the account's equity (above 0, at most 1). Equity is every balance on
    // the venue: the quote asset at 1, another asset at the reference price
    // of its symbol; an asset with no price (a coin not traded here) is left
    // out, with a warning the first time. Fails closed if the balances
    // cannot be read or equity is not above zero.
    std::optional<types::Decimal> max_symbol_share;
    std::optional<std::size_t> max_open_orders;
    // Orders that reach the venue in any sliding 60 seconds.
    std::optional<std::size_t> max_orders_per_minute;
    // Realized loss (net of fees) since the start of the UTC day or since
    // ResetDailyBaseline, summed over all symbols. Once reached, only orders
    // that reduce a position are allowed until the next UTC day. The loss is
    // worked out by the gate from the venue's fills (see PnlTracker), not read
    // from Position::realized_pnl.
    std::optional<types::Decimal> max_daily_loss;
    // Orders that reach the venue in one UTC day. Once reached, only orders
    // that reduce a position are allowed until the next day, so a strategy
    // that flips in and out of a whipsawing market cannot churn fees away.
    std::optional<std::size_t> max_orders_per_day;
    // Consecutive losing sells (a sell fill whose realized P&L, net of its
    // fee, is negative; a winning sell resets the count, as does a new UTC
    // day or ResetDailyBaseline). Once reached, only orders that reduce a
    // position are allowed until the count resets. A sell that fills in
    // several pieces counts each losing piece.
    std::optional<std::size_t> max_consecutive_losses;
    // Largest fall of the strategy's equity from its peak, in the quote
    // asset. Equity is realized P&L (net of fees) since ResetDailyBaseline
    // plus the unrealized P&L of what is held, valued at the last reference
    // price. It is measured when an order is attempted and at each reference
    // price (at most once per watch_interval), so the peak follows the market
    // between orders. Once reached, only orders that reduce a position are
    // allowed.
    // Does not reset at midnight.
    std::optional<types::Decimal> max_drawdown;
    // The same fall, measured the same way, at which the gate closes every
    // position itself: it halts (as Halt does, cancelling every open order)
    // and sells what is held, or buys back a short, at market. Trading stays
    // halted, exits aside, until a new run: Resume halts again while the fall
    // stands, and what is still held is closed again each minute while the
    // market is open. If reached while the market is closed, the halt and
    // CancelAll happen at once and the close is sent when the market opens,
    // even if the fall has eased by then: the orders that protected the
    // positions are gone. Set it alone or above max_drawdown, which then
    // blocks entries first.
    std::optional<types::Decimal> max_drawdown_close;
    // One position's unrealized loss: its quantity times the fall of the
    // reference price from its average cost (net of the fees it paid), for
    // the cost basis the loss count keeps. Measured at each reference price
    // (at most once per watch_interval) and at each order. Once reached, the
    // gate cancels the symbol's open orders, closes the position at market, and
    // refuses new positions in the symbol until the next UTC day. A price
    // noted while the market is closed only pauses the symbol: the cancel and
    // the close need the loss to stand at a price from the session that is
    // open, as a thin after-hours quote (or the last session's close) can be
    // far from where the market opens. A backstop for a strategy's own stop
    // (a gap past it, a stop the venue lost).
    std::optional<types::Decimal> max_loss_per_position;
    // Empty means any symbol.
    std::set<types::Symbol> allowed_symbols;
    // Reject orders priced from a reference price older than this. Unset
    // means a reference price never goes stale.
    std::optional<std::chrono::milliseconds> max_price_age;
    // Refuse new positions in a symbol whose quotes have stopped for this
    // long while its market is open (the Runner tells the gate, checking
    // every 10 seconds; SetQuotesStale). Exits still pass, and entries resume
    // when quotes come back. Only for a strategy that takes quotes; a bar
    // strategy is not checked.
    std::optional<std::chrono::milliseconds> max_quote_age;
    // Not a limit: the venue sells short (PaperOptions::allow_short; set by
    // the factory and the backtest, not the [risk] table). A sale of more
    // than is held opens a short, which max_position_notional counts as it
    // counts a long; a buy of no more than is short reduces a position; a
    // losing cover counts as a losing sell; and the loss count tracks shorts
    // (PnlTracker's shorts).
    bool shorts = false;
    // Not a limit: the quote asset of a symbol that names none (an equity),
    // for valuing balances under max_symbol_share
    // (PaperOptions::default_quote_asset).
    std::string default_quote_asset = "USD";
    // Not a limit: how often a reference price may measure the losses
    // (NoteReferencePrice). Each measurement reads the venue's fills, so the
    // factory sets it longer for a live venue, whose requests are counted.
    std::chrono::milliseconds watch_interval{1000};
};

// A TradingVenue that checks every order against RiskLimits before passing it
// to the real venue. Strategies that only hold the gate cannot bypass it.
// Rejected orders fail with kRejected and a message starting "risk:" and never
// reach the venue. Cancels and all queries pass straight through.
//
// The gate fails closed: if a check needs data (positions, open orders, a
// price) that cannot be had, the order is rejected.
//
// Prices: a limit order is valued at its limit price and a stop at its stop
// price; a market order has no price of its own, so it is valued at the
// reference price last given to NoteReferencePrice (the Runner supplies
// these from bars and quotes, streaming the quotes itself for a strategy
// that takes only bars). Without a usable reference price a market
// order is rejected whenever a notional limit is set.
//
// An order that reduces a position (a sell of no more than is held) is the
// only kind allowed while trading is halted or a loss, drawdown, order-count
// or stale-quote limit is hit. What is held is the venue's position, or what
// the gate has seen fill (net of a fee paid in the coin) since its loss count
// started if that is more: a venue that reports no positions (Binance.US spot)
// still has its exits let through, and max_position_notional counts what was
// bought since then.
//
// Closing: max_drawdown_close and max_loss_per_position close positions by
// themselves, with market orders the gate places on the venue directly (not
// checked, not counted towards the order limits), rounded down to the lot
// size when SetSymbolInfo gives it one. The strategy sees their fills like
// any other, and an order of its own the gate cancelled as closed. A symbol
// is closed at most once a minute, so a close the venue refuses is tried
// again then. Nothing is closed while the market is closed (MarketHours):
// outside the regular session, even when the strategy trades the extended
// hours, as a live market order waits for the regular session anyway (Schwab
// queues it; the paper venue would fill it, so there the gate does the
// waiting).
// max_drawdown_close still halts and calls CancelAll at once, and its close
// is sent when the market opens, whatever the price then. max_loss_per_position
// still pauses the symbol at once, but cancels and closes only if the loss
// stands at a price noted in the session that is open. Halts, closes at
// market, shut-market pauses and once-a-day loss-limit entry refusals are
// reported through util::Notice so configured alerts post them.
//
// Thread-safe. Orders are checked and placed one at a time, so two concurrent
// orders cannot together exceed a limit; that means the venue call is made
// while holding the gate's order lock.
class RiskGatedVenue : public TradingVenue {
   public:
    // `hours` tells when the market is open for closes (null = always open;
    // the Runner passes its data source's). Only its regular session counts.
    RiskGatedVenue(std::shared_ptr<TradingVenue> venue, RiskLimits limits,
                   std::function<types::Timestamp()> clock = types::Now,
                   std::shared_ptr<const types::MarketHours> hours =
                       types::MarketHours::AlwaysOpen());

    // Latest price for valuing market orders, stamped with the clock. With
    // max_drawdown, max_drawdown_close or max_loss_per_position, also
    // measures the drawdown and each position's loss (at most once every
    // RiskLimits::watch_interval, a second by default, and not while an
    // order is being placed), closing positions as those
    // limits say. That reads the venue's fills (and, to close, its orders,
    // positions and balances) on the caller's thread, holding the order
    // lock, so a slow venue delays the caller and any order meanwhile.
    void NoteReferencePrice(const types::Symbol& symbol,
                            const types::Decimal& price);

    // Sends the max_drawdown_close close that waited for the market to open,
    // once it has. The Runner calls this as time passes (at each session
    // event and at least once a minute), so the close does not wait for the
    // next price or order. Cheap when nothing waits.
    void SendPendingClose();

    // Whether `symbol`'s quotes have stopped (max_quote_age); the Runner
    // calls this when they stop and when they come back.
    void SetQuotesStale(const types::Symbol& symbol, bool stale);

    // How a close rounds its quantity to the venue's lot size. Without one,
    // or when it fails, the quantity held is sent as it is.
    void SetSymbolInfo(
        std::function<types::Result<types::SymbolInfo>(const types::Symbol&)>
            info);

    // Starts the loss count: takes the venue's current positions as the cost
    // basis and counts fills from now on. The gate does this by itself at the
    // first order if it has not been done, but a loss realized before then
    // would be missed, so call it when trading starts. Needed only with
    // max_daily_loss. A fill is counted if its time is at or after this call,
    // so the venue's and the gate's clocks should roughly agree.
    types::Result<void> ResetDailyBaseline();

    // Kill switch: stops all orders except position-reducing ones and cancels
    // everything open. The first cancel is issued at once, without the order
    // lock. Halt then waits for any order already in flight to land and
    // cancels again, so that order does not survive the halt. The result is
    // the IDs from both cancels (an order can appear in only one). The halt
    // holds even if a cancel fails; if the second cancel fails its error is
    // returned.
    types::Result<std::vector<std::string>> Halt(const std::string& reason);
    void Resume() {
        mDrawdownNoticed = false;
        mHalted = false;
    }
    bool IsHalted() const { return mHalted; }

    std::string_view Name() const override { return mVenue->Name(); }
    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) override;
    // Checked as its limit order, counting as two orders towards the order
    // limits.
    types::Result<types::OcoAcknowledgement> PlaceOco(
        const types::OcoRequest& request) override;
    types::Result<CancelOutcome> CancelOrder(
        const std::string& order_id) override {
        return mVenue->CancelOrder(order_id);
    }
    types::Result<std::vector<std::string>> CancelAll() override {
        return mVenue->CancelAll();
    }
    types::Result<std::vector<types::OrderStatus>> GetOpenOrders() override {
        return mVenue->GetOpenOrders();
    }
    types::Result<types::OrderStatus> GetOrder(
        const std::string& order_id) override {
        return mVenue->GetOrder(order_id);
    }
    types::Result<std::vector<types::Fill>> GetFills(
        types::Timestamp since) override {
        return mVenue->GetFills(since);
    }
    types::Result<std::unique_ptr<data::Subscription>> StreamExecutions(
        ExecutionCallback callback) override {
        return mVenue->StreamExecutions(std::move(callback));
    }
    types::Result<std::vector<types::Position>> GetPositions() override {
        return mVenue->GetPositions();
    }
    types::Result<std::vector<types::Balance>> GetBalances() override {
        return mVenue->GetBalances();
    }

   private:
    // The checks PlaceOrder makes, for `orders` orders at once (an OCO is
    // two), counting them if they pass. mMutex must be held.
    types::Result<void> Admit(const types::OrderRequest& request,
                              std::size_t orders);
    struct ReferencePrice {
        types::Decimal price;
        types::Timestamp time;
    };

    // All require mMutex.
    types::Result<void> StartLossCount();
    // Starts the loss count if needed and applies the venue's fills since
    // the last call to mPnl.
    types::Result<void> CatchUpFills(types::Timestamp now);
    // Why trading is blocked by the losses, and whether a limit was reached
    // (not when the drawdown is only unknown, a held symbol unpriced).
    struct LossBlock {
        std::string reason;
        bool limit_reached = true;
    };
    // Whether the daily loss, consecutive loss or drawdown limit blocks
    // trading; the fills must have been caught up on.
    std::optional<LossBlock> Losses(types::Timestamp now);
    // The unrealized P&L of everything held, or nullopt if a held symbol has
    // no usable reference price.
    std::optional<types::Decimal> Unrealized(types::Timestamp now) const;
    // The fall of equity from its peak, raising the peak first; nullopt if
    // a held symbol has no usable reference price.
    std::optional<types::Decimal> Drawdown(types::Timestamp now);
    // Catches up on fills, then applies max_drawdown_close and
    // max_loss_per_position: closes what they say to close, while the market
    // is open. Measures the drawdown (raising its peak) whenever max_drawdown
    // is set.
    types::Result<void> Watch(types::Timestamp now);
    // Watch, logging a failure (once) instead of returning or throwing it.
    void WatchSafely(types::Timestamp now);
    // The reason of the max_drawdown_close close that waited for the open,
    // if the market is open now; it no longer waits.
    std::optional<std::string> TakePendingClose(types::Timestamp now);
    // Closes what is held of the symbols in `closing` (symbol -> reason), or
    // with `everything` (a reason) all that is held.
    void CloseHeld(const std::map<types::Symbol, std::string>& closing,
                   const std::optional<std::string>& everything,
                   types::Timestamp now);
    // Cancels `symbol`'s open orders and closes `held` of it at market.
    void Close(const types::Symbol& symbol, const types::Decimal& held,
               const std::string& reason, types::Timestamp now);
    // What is held of each symbol: the venue's position, or what the loss
    // count has seen fill if that is more (with shorts, the larger either
    // way). `fills` says whether the loss count is up to date.
    std::map<types::Symbol, types::Decimal> Holdings(
        const std::vector<types::Position>& positions, bool fills) const;
    // max_position_notional, max_total_notional and max_symbol_share for
    // `request`, an order that adds to a position, valued at `price`.
    types::Result<void> CheckSize(
        const types::OrderRequest& request, const types::Decimal& price,
        const types::Decimal& held,
        const std::map<types::Symbol, types::Decimal>& holdings,
        const std::vector<types::OrderStatus>& open, types::Timestamp now);
    // The account's equity in the quote asset of `symbol` (see
    // max_symbol_share).
    types::Result<types::Decimal> Equity(const types::Symbol& symbol,
                                         types::Timestamp now);
    void NewDay(std::chrono::sys_days day);
    std::optional<types::Decimal> ReferencePriceFor(const types::Symbol& symbol,
                                                    types::Timestamp now) const;
    // Whether `symbol`'s reference price was noted in the session that is
    // open now (false while the market is shut).
    bool HasOpenMarketPrice(const types::Symbol& symbol,
                            types::Timestamp now) const;
    std::optional<types::Decimal> PriceFor(const types::OrderRequest& request,
                                           types::Timestamp now) const;

    const std::shared_ptr<TradingVenue> mVenue;
    const RiskLimits mLimits;
    const std::function<types::Timestamp()> mClock;
    const types::MarketHours mHours;  // the regular session

    std::atomic<bool> mHalted{false};
    // max_drawdown_close's halt has been posted (util::Notice); cleared by
    // Resume and StartLossCount. Atomic, as Resume takes no lock.
    std::atomic<bool> mDrawdownNoticed{false};

    mutable std::mutex mPriceMutex;
    std::map<types::Symbol, ReferencePrice> mPrices;
    std::set<types::Symbol> mStaleQuotes;
    std::function<types::Result<types::SymbolInfo>(const types::Symbol&)>
        mSymbolInfo;  // set before trading starts

    std::mutex mMutex;  // serializes checking and placing orders
    std::deque<types::Timestamp> mRecentOrders;
    // The daily loss count. mFillCursor is the newest fill time seen, or an
    // hour before the last read if later; fills at or after it come back on
    // every poll, so mSeenFills (pruned to the cursor) filters them out.
    bool mCounting = false;
    PnlTracker mPnl;
    std::chrono::sys_days mDay{};
    std::size_t mConsecutiveLosses = 0;
    types::Decimal mTotalRealized;  // since the baseline; not reset daily
    types::Decimal mPeakEquity;
    std::size_t mOrdersToday = 0;
    std::chrono::sys_days mOrderDay{};
    types::Timestamp mFillCursor;
    std::map<std::string, types::Timestamp> mSeenFills;
    types::Timestamp mLastWatch;
    bool mWatchFailing = false;     // warned that Watch could not read fills
    bool mDrawdownUnknown = false;  // warned that it could not be measured
    // Symbols that reached max_loss_per_position, and the UTC day of it.
    std::map<types::Symbol, std::chrono::sys_days> mStopped;
    std::map<types::Symbol, types::Timestamp> mLastClose;
    bool mDrawdownClosed = false;  // max_drawdown_close has halted the gate
    // The day a loss limit's refusal was last posted as a notice.
    std::optional<std::chrono::sys_days> mLossNoticeDay;
    std::optional<types::Timestamp> mLastDrawdownClose;
    // Why max_drawdown_close closes everything, while that waits for the
    // market to open.
    std::optional<std::string> mPendingDrawdownClose;
    std::set<std::string> mUnpricedAssets;  // warned about once
};

}  // namespace kestrel::trading
