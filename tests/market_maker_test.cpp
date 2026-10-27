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

types::Fill FillOf(Side side, const char* quantity,
                   const char* order_id = "") {
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
            context.venue.balances = {
                {"ZEC", D(held), D(held), D("0")}, {"USDT", D("1000"), D("1000"), D("0")}};
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
    m.strategy.OnOrderUpdate(m.context, StatusOf("order-1", OrderState::kFilled));
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
    m.strategy.OnOrderUpdate(m.context, StatusOf("order-1", OrderState::kFilled));
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
    m.strategy.OnOrderUpdate(m.context, StatusOf("order-1", OrderState::kFilled));
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

}  // namespace
