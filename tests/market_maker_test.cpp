#include "kestrel/strategy/market_maker.hpp"

#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using kestrel::testing::RecordingVenue;
using strategy::MarketMaker;
using strategy::MarketMakerParams;
using trading::CancelOutcome;
using types::OrderState;
using types::Side;

constexpr const char* kSymbol = "ZEC/USDT";

class CancelVenue : public RecordingVenue {
   public:
    types::Result<CancelOutcome> CancelOrder(const std::string& id) override {
        cancelled.push_back(id);
        return outcome;
    }

    types::Result<CancelOutcome> outcome = CancelOutcome::kCancelled;
    std::vector<std::string> cancelled;
};

class FakeContext : public strategy::Context {
   public:
    types::Timestamp Now() const override { return now; }
    trading::TradingVenue& Venue() override { return venue; }
    types::Result<types::SymbolInfo> SymbolInfo(const types::Symbol&) override {
        return types::SymbolInfo{kSymbol, D("0.01"), D("0.001"), D("0.001")};
    }
    std::span<const types::Bar> Bars(const types::Symbol&,
                                     types::Timeframe) const override {
        return {};
    }

    types::Timestamp now = types::FromEpochSeconds(1000);
    CancelVenue venue;
};

MarketMakerParams Params() {
    MarketMakerParams p;
    p.symbol = kSymbol;
    p.quantity = D("0.5");
    p.half_spread_bps = D("10");
    p.max_inventory = D("2");
    p.requote_bps = D("5");
    return p;
}

types::Quote QuoteOf(const char* bid, const char* ask,
                     const char* symbol = kSymbol) {
    types::Quote q;
    q.symbol = symbol;
    q.bid = D(bid);
    q.ask = D(ask);
    return q;
}

types::Fill FillOf(Side side, const char* quantity, const char* order_id = "") {
    types::Fill f;
    f.symbol = kSymbol;
    f.side = side;
    f.quantity = D(quantity);
    f.price = D("100");
    f.order_id = order_id;
    return f;
}

types::OrderStatus StatusOf(const char* id, OrderState state) {
    types::OrderStatus s;
    s.order_id = id;
    s.state = state;
    return s;
}

// A strategy that starts holding `held` base units, with the context ready.
struct Maker {
    explicit Maker(const char* held = "1", MarketMakerParams p = Params())
        : strategy(std::move(p)) {
        if (D(held).Sign() > 0) {
            context.venue.balances = {{"ZEC", D(held), D(held), D("0")},
                                      {"USDT", D("1000"), D("1000"), D("0")}};
        }
        strategy.OnStart(context);
    }
    void Quote(const char* bid, const char* ask) {
        strategy.OnQuote(context, QuoteOf(bid, ask));
    }
    const std::vector<types::OrderRequest>& Orders() const {
        return context.venue.orders;
    }

    FakeContext context;
    MarketMaker strategy;
};

TEST(MarketMakerTest, RejectsBadParameters) {
    for (auto change : std::vector<void (*)(MarketMakerParams&)>{
             [](MarketMakerParams& p) { p.quantity = D("0"); },
             [](MarketMakerParams& p) { p.half_spread_bps = D("0"); },
             [](MarketMakerParams& p) { p.max_inventory = D("0.1"); },
             [](MarketMakerParams& p) { p.skew_bps = D("-1"); },
             [](MarketMakerParams& p) { p.requote_bps = D("-1"); },
             [](MarketMakerParams& p) {
                 p.max_quote_age = std::chrono::milliseconds(-1);
             }}) {
        auto p = Params();
        change(p);
        EXPECT_THROW(MarketMaker{p}, std::invalid_argument);
    }
}

TEST(MarketMakerTest, AsksForQuotesOnly) {
    MarketMaker strategy(Params());
    const auto needs = strategy.Needs();
    EXPECT_TRUE(needs.quotes);
    EXPECT_TRUE(needs.bar_timeframes.empty());
    EXPECT_EQ(needs.symbols, std::vector<types::Symbol>{kSymbol});
}

TEST(MarketMakerTest, QuotesBothSidesAroundTheMid) {
    Maker m;
    m.Quote("99.99", "100.01");  // mid 100, so 10 bps is 0.10
    ASSERT_EQ(m.Orders().size(), 2u);
    EXPECT_EQ(m.Orders()[0].side, Side::kBuy);
    EXPECT_EQ(m.Orders()[0].type, types::OrderType::kLimit);
    EXPECT_EQ(m.Orders()[0].limit_price, D("99.90"));
    EXPECT_EQ(m.Orders()[0].quantity, D("0.5"));
    EXPECT_EQ(m.Orders()[1].side, Side::kSell);
    EXPECT_EQ(m.Orders()[1].limit_price, D("100.10"));
}

TEST(MarketMakerTest, RoundsTheBidDownAndTheAskUp) {
    // The bid rounds down and the ask up, so rounding never narrows the spread.
    Maker m;
    m.Quote("99.98", "100.03");  // mid 100.005: +-0.100005
    ASSERT_EQ(m.Orders().size(), 2u);
    EXPECT_EQ(m.Orders()[0].limit_price, D("99.90"));
    EXPECT_EQ(m.Orders()[1].limit_price, D("100.11"));
}

TEST(MarketMakerTest, LeavesAQuoteAloneUntilTheTargetMovesEnough) {
    Maker m;
    m.Quote("99.99", "100.01");
    ASSERT_EQ(m.Orders().size(), 2u);

    m.Quote("100.02", "100.04");  // mid 100.03: 0.03 < 5 bps (0.05)
    EXPECT_EQ(m.Orders().size(), 2u);
    EXPECT_TRUE(m.context.venue.cancelled.empty());

    m.Quote("100.09", "100.11");  // mid 100.10: 0.10 >= 0.05, both replaced
    EXPECT_EQ(m.context.venue.cancelled,
              (std::vector<std::string>{"order-1", "order-2"}));
    ASSERT_EQ(m.Orders().size(), 4u);
    EXPECT_EQ(m.Orders()[2].limit_price, D("99.99"));  // 100.10 - 0.1001
    EXPECT_EQ(m.Orders()[3].limit_price, D("100.21"));
}

TEST(MarketMakerTest, ReplacesAQuoteOlderThanTheMaximumAge) {
    auto p = Params();
    p.max_quote_age = std::chrono::milliseconds(5000);
    Maker m("1", p);
    m.Quote("99.99", "100.01");
    m.context.now += std::chrono::seconds(4);
    m.Quote("99.99", "100.01");
    EXPECT_EQ(m.Orders().size(), 2u);
    m.context.now += std::chrono::seconds(2);
    m.Quote("99.99", "100.01");
    EXPECT_EQ(m.Orders().size(), 4u);
}

TEST(MarketMakerTest, LeansAwayFromTheHeavySideOfTheInventory) {
    auto p = Params();
    p.skew_bps = D("20");
    Maker m("1", p);
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "1"));  // half of the max
    m.Quote("99.99", "100.01");
    ASSERT_EQ(m.Orders().size(), 2u);
    // 10 bps of lean: reservation 99.90, so the bid is 99.80 and the ask
    // 100.00 (still behind the touch).
    EXPECT_EQ(m.Orders()[0].limit_price, D("99.80"));
    EXPECT_EQ(m.Orders()[1].limit_price, D("100.00"));
}

TEST(MarketMakerTest, NeverCrossesTheBook) {
    auto p = Params();
    p.skew_bps = D("100");
    p.half_spread_bps = D("1");
    Maker m("1", p);
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "2"));  // at the limit
    m.Quote("99.99", "100.01");
    // The reservation falls to 99.00, but the ask is held a tick over the bid.
    ASSERT_EQ(m.Orders().size(), 1u);
    EXPECT_EQ(m.Orders()[0].side, Side::kSell);
    EXPECT_EQ(m.Orders()[0].limit_price, D("100.00"));
}

TEST(MarketMakerTest, StopsBuyingAtTheInventoryLimitAndSizesDownNearIt) {
    Maker m;
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "1.8"));  // 0.2 of room
    m.Quote("99.99", "100.01");
    ASSERT_EQ(m.Orders().size(), 2u);
    EXPECT_EQ(m.Orders()[0].side, Side::kBuy);
    EXPECT_EQ(m.Orders()[0].quantity, D("0.2"));

    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "0.2", "order-1"));
    m.strategy.OnOrderUpdate(m.context,
                             StatusOf("order-1", OrderState::kFilled));
    m.context.venue.orders.clear();
    m.Quote("99.99", "100.01");
    for (const auto& order : m.Orders()) {
        EXPECT_EQ(order.side, Side::kSell);  // no bid
    }
}

TEST(MarketMakerTest, CannotSellWhatItDoesNotHold) {
    Maker m("0");  // flat: spot cannot go short
    m.Quote("99.99", "100.01");
    ASSERT_EQ(m.Orders().size(), 1u);
    EXPECT_EQ(m.Orders()[0].side, Side::kBuy);

    // After a buy there is something to sell.
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "0.5", "order-1"));
    m.strategy.OnOrderUpdate(m.context,
                             StatusOf("order-1", OrderState::kFilled));
    m.Quote("99.99", "100.01");
    bool sells = false;
    for (const auto& order : m.Orders()) {
        sells |= order.side == Side::kSell;
    }
    EXPECT_TRUE(sells);
}

TEST(MarketMakerTest, PullsAQuoteThatNoLongerFits) {
    Maker m;
    m.Quote("99.99", "100.01");
    ASSERT_EQ(m.Orders().size(), 2u);  // bid order-1, ask order-2
    // 0.4 of the bid fills, leaving 0.1 resting; other buying takes the
    // position to 1 + 1.95, leaving room for only 0.05.
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "0.4", "order-1"));
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "1.55", "elsewhere"));
    m.Quote("99.99", "100.01");
    EXPECT_FALSE(m.context.venue.cancelled.empty());
    EXPECT_EQ(m.context.venue.cancelled.front(), "order-1");
}

TEST(MarketMakerTest, WaitsOutACancelThatLostToAFill) {
    Maker m;
    m.Quote("99.99", "100.01");
    ASSERT_EQ(m.Orders().size(), 2u);

    m.context.venue.outcome = CancelOutcome::kAlreadyFilled;
    m.Quote("100.09", "100.11");  // both would be replaced
    EXPECT_EQ(m.context.venue.cancelled.size(), 2u);
    EXPECT_EQ(m.Orders().size(), 2u);  // no replacement while unsettled

    m.Quote("100.09", "100.11");
    EXPECT_EQ(m.context.venue.cancelled.size(), 2u);  // not cancelled again
    EXPECT_EQ(m.Orders().size(), 2u);

    // The bid's final state arrives, so it quotes again; the ask is still
    // unsettled.
    m.context.venue.outcome = CancelOutcome::kCancelled;
    m.strategy.OnOrderUpdate(m.context,
                             StatusOf("order-1", OrderState::kFilled));
    m.Quote("100.09", "100.11");
    ASSERT_EQ(m.Orders().size(), 3u);
    EXPECT_EQ(m.Orders()[2].side, Side::kBuy);
}

TEST(MarketMakerTest, QuotesAgainAfterARestingQuoteIsRejectedOrExpires) {
    Maker m;
    m.Quote("99.99", "100.01");
    ASSERT_EQ(m.Orders().size(), 2u);
    m.strategy.OnOrderUpdate(m.context,
                             StatusOf("order-1", OrderState::kCancelled));
    m.strategy.OnOrderUpdate(m.context, StatusOf("order-2", OrderState::kNew));
    m.Quote("99.99", "100.01");
    ASSERT_EQ(m.Orders().size(), 3u);  // only the bid was lost
    EXPECT_EQ(m.Orders()[2].side, Side::kBuy);
    EXPECT_TRUE(m.context.venue.cancelled.empty());
}

TEST(MarketMakerTest, IgnoresBooksItCannotQuote) {
    Maker m;
    m.Quote("100.00", "100.00");  // locked
    m.Quote("100.01", "100.00");  // crossed
    m.Quote("0", "1");            // no bid
    m.strategy.OnQuote(m.context, QuoteOf("99.99", "100.01", "ETH/USDT"));
    types::Quote one_sided;
    one_sided.symbol = kSymbol;
    one_sided.bid = D("99.99");
    m.strategy.OnQuote(m.context, one_sided);
    EXPECT_TRUE(m.Orders().empty());
}

TEST(MarketMakerTest, DoesNothingWithoutSymbolInfo) {
    MarketMaker strategy(Params());
    struct NoInfo : FakeContext {
        types::Result<types::SymbolInfo> SymbolInfo(
            const types::Symbol&) override {
            return types::MakeError(types::ErrorCategory::kNotFound, "none");
        }
    } context;
    strategy.OnStart(context);
    strategy.OnQuote(context, QuoteOf("99.99", "100.01"));
    EXPECT_TRUE(context.venue.orders.empty());
}

TEST(MarketMakerTest, CancelsItsQuotesWhenStopped) {
    Maker m;
    m.Quote("99.99", "100.01");
    m.strategy.OnStop(m.context);
    EXPECT_EQ(m.context.venue.cancelled,
              (std::vector<std::string>{"order-1", "order-2"}));
}

// ---- Protection rules ----

using std::chrono::minutes;
using std::chrono::seconds;

// The orders that are market orders, in the order placed.
std::vector<types::OrderRequest> MarketOrders(const Maker& m) {
    std::vector<types::OrderRequest> out;
    for (const auto& order : m.Orders()) {
        if (order.type == types::OrderType::kMarket) out.push_back(order);
    }
    return out;
}

bool Quotes(const Maker& m, Side side, std::size_t after = 0) {
    for (std::size_t i = after; i < m.Orders().size(); ++i) {
        if (m.Orders()[i].type == types::OrderType::kLimit &&
            m.Orders()[i].side == side) {
            return true;
        }
    }
    return false;
}

TEST(MarketMakerProtectionTest, EveryRuleIsOffByDefault) {
    Maker m;
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "1"));
    m.Quote("50.00", "50.02");  // a 50% fall from the fill at 100
    m.context.now += minutes(5);
    m.Quote("40.00", "40.02");
    EXPECT_TRUE(MarketOrders(m).empty());
    EXPECT_TRUE(Quotes(m, Side::kBuy));
}

TEST(MarketMakerProtectionTest, RejectsBadProtectionParameters) {
    for (auto change : std::vector<void (*)(MarketMakerParams&)>{
             [](MarketMakerParams& p) {
                 p.protection.stop_loss = true;
                 p.protection.stop_loss_bps = D("0");
             },
             [](MarketMakerParams& p) { p.protection.loss_cap = true; },
             [](MarketMakerParams& p) { p.protection.drawdown_cap = true; },
             [](MarketMakerParams& p) {
                 p.protection.spike_standdown = true;
                 p.protection.spike_5m_bps = D("0");
             },
             [](MarketMakerParams& p) {
                 p.protection.downtrend_filter = true;
                 p.protection.downtrend_bps = D("0");
             },
             [](MarketMakerParams& p) {
                 p.protection.momentum_filter = true;
                 p.protection.momentum_bps = D("-1");
             },
             [](MarketMakerParams& p) {
                 p.protection.cooldown = std::chrono::milliseconds(-1);
             }}) {
        auto p = Params();
        change(p);
        EXPECT_THROW(MarketMaker{p}, std::invalid_argument);
    }
}

TEST(MarketMakerProtectionTest, StopLossExitsAtMarketOnceAndPausesQuoting) {
    auto p = Params();
    p.protection.stop_loss = true;  // 40 bps
    Maker m("1", p);
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "0.5"));  // entry 100
    m.Quote("99.70", "99.72");  // 30 bps under: no stop yet
    EXPECT_TRUE(MarketOrders(m).empty());
    ASSERT_EQ(m.Orders().size(), 2u);

    m.Quote("99.59", "99.61");  // bid 41 bps under the entry
    ASSERT_EQ(MarketOrders(m).size(), 1u);
    EXPECT_EQ(MarketOrders(m)[0].side, Side::kSell);
    EXPECT_EQ(MarketOrders(m)[0].quantity, D("0.5"));
    EXPECT_EQ(m.context.venue.cancelled.size(), 2u);  // quotes pulled first

    const auto placed = m.Orders().size();
    m.Quote("99.50", "99.52");  // the exit's fill has not arrived
    EXPECT_EQ(m.Orders().size(), placed);

    // The fill arrives; the position is closed and quoting resumes.
    m.strategy.OnFill(m.context, FillOf(Side::kSell, "0.5", "order-3"));
    m.Quote("99.50", "99.52");
    EXPECT_EQ(MarketOrders(m).size(), 1u);
    EXPECT_GT(m.Orders().size(), placed);
}

TEST(MarketMakerProtectionTest, StopLossAveragesTheEntryAcrossFills) {
    auto p = Params();
    p.protection.stop_loss = true;
    Maker m("1", p);
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "0.5"));  // at 100
    auto dearer = FillOf(Side::kBuy, "0.5");
    dearer.price = D("102");  // average 101, a stop at 100.596
    m.strategy.OnFill(m.context, dearer);
    m.Quote("100.60", "100.62");
    EXPECT_TRUE(MarketOrders(m).empty());
    m.Quote("100.58", "100.60");
    ASSERT_EQ(MarketOrders(m).size(), 1u);
    EXPECT_EQ(MarketOrders(m)[0].quantity, D("1"));
}

TEST(MarketMakerProtectionTest, StopLossBuysBackAPositionBelowTheStart) {
    auto p = Params();
    p.protection.stop_loss = true;
    Maker m("1", p);
    m.strategy.OnFill(m.context, FillOf(Side::kSell, "0.5"));  // short at 100
    m.Quote("100.40", "100.42");  // ask 42 bps over
    ASSERT_EQ(MarketOrders(m).size(), 1u);
    EXPECT_EQ(MarketOrders(m)[0].side, Side::kBuy);
    EXPECT_EQ(MarketOrders(m)[0].quantity, D("0.5"));
}

TEST(MarketMakerProtectionTest, StopLossNeverSellsTheStartingHolding) {
    auto p = Params();
    p.protection.stop_loss = true;
    Maker m("1", p);  // no fills: nothing beyond the start
    m.Quote("50.00", "50.02");
    EXPECT_TRUE(MarketOrders(m).empty());
}

TEST(MarketMakerProtectionTest, CooldownHoldsBothSidesAfterAStop) {
    auto p = Params();
    p.protection.stop_loss = true;
    p.protection.cooldown_after_stop = true;
    p.protection.cooldown = std::chrono::milliseconds(60000);
    Maker m("1", p);
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "0.5"));
    m.Quote("99.59", "99.61");  // stop
    m.strategy.OnFill(m.context, FillOf(Side::kSell, "0.5", "order-1"));
    const auto placed = m.Orders().size();

    m.context.now += seconds(30);
    m.Quote("99.59", "99.61");
    EXPECT_EQ(m.Orders().size(), placed);

    m.context.now += seconds(31);
    m.Quote("99.59", "99.61");
    EXPECT_GT(m.Orders().size(), placed);
}

TEST(MarketMakerProtectionTest, NoCooldownUnlessItsRuleIsOn) {
    auto p = Params();
    p.protection.stop_loss = true;
    Maker m("1", p);
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "0.5"));
    m.Quote("99.59", "99.61");
    m.strategy.OnFill(m.context, FillOf(Side::kSell, "0.5", "order-1"));
    const auto placed = m.Orders().size();
    m.Quote("99.59", "99.61");
    EXPECT_GT(m.Orders().size(), placed);
}

TEST(MarketMakerProtectionTest, LossCapHaltsAndFlattensForGood) {
    auto p = Params();
    p.protection.loss_cap = true;
    p.protection.loss_cap_amount = D("1");
    Maker m("1", p);
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "1"));  // -100 cash
    m.Quote("99.20", "99.22");  // P&L about -0.79: still quoting
    EXPECT_TRUE(MarketOrders(m).empty());

    m.Quote("98.98", "99.00");  // mid 98.99: -1.01
    ASSERT_EQ(MarketOrders(m).size(), 1u);
    EXPECT_EQ(MarketOrders(m)[0].side, Side::kSell);
    EXPECT_EQ(MarketOrders(m)[0].quantity, D("1"));

    m.strategy.OnFill(m.context, FillOf(Side::kSell, "1", "order-3"));
    const auto placed = m.Orders().size();
    m.context.now += minutes(60);
    m.Quote("110.00", "110.02");  // recovered, but the halt is permanent
    EXPECT_EQ(m.Orders().size(), placed);
}

TEST(MarketMakerProtectionTest, LossCapCountsFees) {
    auto p = Params();
    p.protection.loss_cap = true;
    p.protection.loss_cap_amount = D("1");
    Maker m("1", p);
    auto fill = FillOf(Side::kBuy, "1");
    fill.fee = D("0.5");
    fill.fee_asset = "USDT";
    m.strategy.OnFill(m.context, fill);
    m.Quote("99.40", "99.42");  // -0.59 on the price, -1.09 with the fee
    EXPECT_EQ(MarketOrders(m).size(), 1u);
}

TEST(MarketMakerProtectionTest, DrawdownCapMeasuresFromThePeak) {
    auto p = Params();
    p.protection.drawdown_cap = true;
    p.protection.drawdown_cap_amount = D("1");
    Maker m("1", p);
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "1"));
    m.Quote("100.99", "101.01");  // +1.00: the peak
    m.Quote("100.20", "100.22");  // 0.79 off the peak
    EXPECT_TRUE(MarketOrders(m).empty());
    m.Quote("99.98", "100.00");  // 1.01 off the peak, though still ahead of 0
    ASSERT_EQ(MarketOrders(m).size(), 1u);
    EXPECT_EQ(MarketOrders(m)[0].side, Side::kSell);
}

TEST(MarketMakerProtectionTest, SpikeStandsDownThenResumes) {
    auto p = Params();
    p.protection.spike_standdown = true;  // 50 bps in 3 minutes
    Maker m("1", p);
    m.Quote("99.99", "100.01");
    ASSERT_EQ(m.Orders().size(), 2u);

    m.context.now += seconds(200);
    m.Quote("100.59", "100.61");  // 60 bps in 200 s
    EXPECT_EQ(m.context.venue.cancelled.size(), 2u);
    EXPECT_EQ(m.Orders().size(), 2u);

    m.context.now += minutes(10);
    m.Quote("100.59", "100.61");  // inside the 15 minutes
    EXPECT_EQ(m.Orders().size(), 2u);

    m.context.now += minutes(6);
    m.Quote("100.59", "100.61");
    EXPECT_GT(m.Orders().size(), 2u);
}

TEST(MarketMakerProtectionTest, SpikeNeedsEnoughHistoryToCount) {
    auto p = Params();
    p.protection.spike_standdown = true;
    Maker m("1", p);
    m.Quote("99.99", "100.01");
    m.context.now += seconds(10);  // nothing 3 minutes old to compare with
    m.Quote("102.99", "103.01");
    EXPECT_EQ(m.Orders().size(), 4u);  // requoted, not stood down
}

TEST(MarketMakerProtectionTest, DowntrendFilterPullsOnlyTheBid) {
    auto p = Params();
    p.protection.downtrend_filter = true;  // 30 bps in 15 minutes
    Maker m("1", p);
    m.Quote("99.99", "100.01");
    ASSERT_EQ(m.Orders().size(), 2u);

    m.context.now += seconds(901);
    m.Quote("99.59", "99.61");  // 40 bps down
    EXPECT_EQ(m.context.venue.cancelled.front(), "order-1");
    EXPECT_FALSE(Quotes(m, Side::kBuy, 2));
    EXPECT_TRUE(Quotes(m, Side::kSell, 2));
}

TEST(MarketMakerProtectionTest, MomentumFilterBlocksBidsOnAFallOnly) {
    auto p = Params();
    p.protection.momentum_filter = true;
    Maker m("1", p);
    m.Quote("99.99", "100.01");
    m.context.now += seconds(181);
    m.Quote("99.89", "99.91");  // down over 3 minutes
    EXPECT_FALSE(Quotes(m, Side::kBuy, 2));

    m.context.now += seconds(181);
    m.Quote("99.99", "100.01");  // up over 3 minutes
    EXPECT_TRUE(Quotes(m, Side::kBuy, 2));
}

TEST(MarketMakerProtectionTest, ExitIsRetriedWhenTheVenueRefusesIt) {
    auto p = Params();
    p.protection.stop_loss = true;
    Maker m("1", p);
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "0.5"));
    m.Quote("99.50", "99.52");
    ASSERT_EQ(MarketOrders(m).size(), 1u);
    // The order closes without filling, so the next quote tries again.
    m.strategy.OnOrderUpdate(m.context,
                             StatusOf("order-1", OrderState::kRejected));
    m.Quote("99.50", "99.52");
    EXPECT_EQ(MarketOrders(m).size(), 2u);
}

TEST(MarketMakerProtectionTest, DoesNotExitWhileACancelIsUnsettled) {
    auto p = Params();
    p.protection.stop_loss = true;
    Maker m("1", p);
    m.Quote("99.99", "100.01");
    m.strategy.OnFill(m.context, FillOf(Side::kBuy, "0.5"));  // at 100
    m.context.venue.outcome = CancelOutcome::kAlreadyFilled;
    m.Quote("99.50", "99.52");
    EXPECT_TRUE(MarketOrders(m).empty());
}

}  // namespace
