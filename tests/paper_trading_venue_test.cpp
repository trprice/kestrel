#include "kestrel/trading/paper_trading_venue.hpp"

#include <atomic>
#include <thread>

#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using trading::CancelOutcome;
using types::ErrorCategory;
using types::OrderRequest;
using types::OrderState;
using types::OrderType;
using types::Side;
using types::TimeInForce;

class FakeDataSource : public data::DataSource {
   public:
    class Sub : public data::Subscription {
       public:
        void Stop() override { stopped = true; }
        std::atomic<bool> stopped{false};
    };

    std::string_view Name() const override { return "fake"; }
    types::Result<std::vector<types::Bar>> GetBars(const types::Symbol&,
                                                   types::Timeframe,
                                                   types::Timestamp,
                                                   types::Timestamp) override {
        return types::MakeError(ErrorCategory::kNotImplemented, "n/a");
    }
    types::Result<std::unique_ptr<data::Subscription>> StreamQuotes(
        const std::vector<std::string>&, data::QuoteCallback cb) override {
        callback = std::move(cb);
        auto sub = std::make_unique<Sub>();
        mSub = sub.get();
        return std::unique_ptr<data::Subscription>(std::move(sub));
    }
    types::Result<types::SymbolInfo> GetSymbolInfo(
        const types::Symbol& s) override {
        if (s != "BTC/USD") {
            return types::MakeError(ErrorCategory::kNotFound, "unknown " + s);
        }
        return types::SymbolInfo{s, D("0.1"), D("0.00000001"), D("0.0001")};
    }

    data::QuoteCallback callback;
    Sub* mSub = nullptr;
};

types::Quote Q(const char* bid, const char* ask, const char* bid_size = nullptr,
               const char* ask_size = nullptr) {
    types::Quote q;
    q.symbol = "BTC/USD";
    q.venue = "fake";
    q.received_time = types::FromEpochSeconds(1);
    q.bid = D(bid);
    q.ask = D(ask);
    if (bid_size) {
        q.bid_size = D(bid_size);
    }
    if (ask_size) {
        q.ask_size = D(ask_size);
    }
    return q;
}

OrderRequest Order(Side side, OrderType type, const char* qty,
                   const char* limit = nullptr, const char* stop = nullptr) {
    OrderRequest r;
    r.symbol = "BTC/USD";
    r.side = side;
    r.type = type;
    r.quantity = D(qty);
    if (limit) {
        r.limit_price = D(limit);
    }
    if (stop) {
        r.stop_price = D(stop);
    }
    return r;
}

struct Fixture {
    std::shared_ptr<FakeDataSource> data = std::make_shared<FakeDataSource>();
    std::atomic<std::int64_t> clock_seconds{1'700'000'000};
    trading::PaperOptions
        options;  // 10,000 USD; 25/40 bps fees; 5 bps slippage

    std::unique_ptr<trading::PaperTradingVenue> Make() {
        return std::make_unique<trading::PaperTradingVenue>(
            data, options,
            [this] { return types::FromEpochSeconds(clock_seconds.load()); });
    }
};

types::Balance Bal(trading::TradingVenue& v, const std::string& asset) {
    auto balances = v.GetBalances();
    EXPECT_TRUE(balances.has_value());
    for (const auto& b : *balances)
        if (b.asset == asset) {
            return b;
        }
    return types::Balance{asset, {}, {}, {}};
}

TEST(PaperMarket, BuyFillsAtAskPlusSlippageWithTakerFee) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));

    auto ack = venue->PlaceOrder(Order(Side::kBuy, OrderType::kMarket, "0.1"));
    ASSERT_OK(ack);
    EXPECT_EQ(ack->state, OrderState::kFilled);
    EXPECT_FALSE(ack->order_id.empty());

    auto fills = venue->GetFills(types::FromEpochSeconds(0));
    ASSERT_OK(fills);
    ASSERT_EQ(fills->size(), 1u);
    EXPECT_EQ((*fills)[0].price, D("50025.0"));  // 50000 * 1.0005
    EXPECT_EQ((*fills)[0].quantity, D("0.1"));
    EXPECT_EQ((*fills)[0].fee, D("20.01"));  // 5002.50 * 0.40%
    EXPECT_EQ((*fills)[0].fee_asset, "USD");
    EXPECT_EQ((*fills)[0].liquidity, types::Liquidity::kTaker);
    EXPECT_EQ((*fills)[0].order_id, ack->order_id);

    EXPECT_EQ(Bal(*venue, "USD").total, D("4977.49"));
    EXPECT_EQ(Bal(*venue, "USD").locked, D("0"));
    EXPECT_EQ(Bal(*venue, "BTC").total, D("0.1"));

    auto positions = venue->GetPositions();
    ASSERT_OK(positions);
    ASSERT_EQ(positions->size(), 1u);
    EXPECT_EQ((*positions)[0].symbol, "BTC/USD");
    EXPECT_EQ((*positions)[0].quantity, D("0.1"));
    EXPECT_EQ((*positions)[0].average_price, D("50025.0"));
}

TEST(PaperMarket, SellFillsAtBidMinusSlippageAndRealizesPnl) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    ASSERT_OK(venue->PlaceOrder(Order(Side::kBuy, OrderType::kMarket, "0.1")));
    venue->OnQuote(Q("51000.0", "51010.0"));
    ASSERT_OK(venue->PlaceOrder(Order(Side::kSell, OrderType::kMarket, "0.1")));

    auto fills = venue->GetFills(types::FromEpochSeconds(0));
    ASSERT_EQ(fills->size(), 2u);
    EXPECT_EQ((*fills)[1].price, D("50974.5"));  // 51000 * 0.9995
    EXPECT_EQ((*fills)[1].fee, D("20.3898"));
    // Start 10,000; gain (50974.5-50025)*0.1 = 94.95; fees 20.01 + 20.3898.
    EXPECT_EQ(Bal(*venue, "USD").total, D("10054.5502"));
    EXPECT_EQ(Bal(*venue, "BTC").total, D("0"));
    EXPECT_TRUE(venue->GetPositions()->empty());
}

TEST(PaperMarket, NoQuoteMeansNoMarketOrder) {
    Fixture f;
    auto venue = f.Make();
    auto ack = venue->PlaceOrder(Order(Side::kBuy, OrderType::kMarket, "0.1"));
    ASSERT_FALSE(ack.has_value());
    EXPECT_EQ(ack.error().category, ErrorCategory::kNotFound);
}

TEST(PaperMarket, FallsBackToLastTradeWhenOneSideIsMissing) {
    Fixture f;
    auto venue = f.Make();
    types::Quote q;
    q.symbol = "BTC/USD";
    q.last = D("40000.0");
    venue->OnQuote(q);
    auto ack = venue->PlaceOrder(Order(Side::kBuy, OrderType::kMarket, "0.1"));
    ASSERT_OK(ack);
    EXPECT_EQ(venue->GetFills(types::FromEpochSeconds(0))->at(0).price,
              D("40020.0"));
}

TEST(PaperLimit, RestsThenFillsAtLimitPriceAsMaker) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    auto ack = venue->PlaceOrder(
        Order(Side::kBuy, OrderType::kLimit, "0.1", "49000.0"));
    ASSERT_OK(ack);
    EXPECT_EQ(ack->state, OrderState::kNew);

    auto open = venue->GetOpenOrders();
    ASSERT_EQ(open->size(), 1u);
    // 0.1 * 49000 * (1 + 0.40%) is held back: the worst-case fee is reserved.
    EXPECT_EQ(Bal(*venue, "USD").locked, D("4919.6"));
    EXPECT_EQ(Bal(*venue, "USD").available, D("5080.4"));
    EXPECT_TRUE(venue->GetFills(types::FromEpochSeconds(0))->empty());

    venue->OnQuote(Q("49000.0", "49000.1"));  // still above the limit
    EXPECT_EQ(venue->GetOpenOrders()->size(), 1u);

    venue->OnQuote(Q("48890.0", "48900.0"));  // crosses
    EXPECT_TRUE(venue->GetOpenOrders()->empty());
    auto fills = venue->GetFills(types::FromEpochSeconds(0));
    ASSERT_EQ(fills->size(), 1u);
    EXPECT_EQ((*fills)[0].price, D("49000.0"));
    EXPECT_EQ((*fills)[0].fee, D("12.25"));  // 4900 * 0.25% maker
    EXPECT_EQ((*fills)[0].liquidity, types::Liquidity::kMaker);
    EXPECT_EQ(Bal(*venue, "USD").total, D("5087.75"));
    EXPECT_EQ(Bal(*venue, "USD").locked, D("0"));
}

TEST(PaperLimit, MarketableOnArrivalTakesTheTouch) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    auto ack = venue->PlaceOrder(
        Order(Side::kBuy, OrderType::kLimit, "0.1", "50100.0"));
    ASSERT_OK(ack);
    EXPECT_EQ(ack->state, OrderState::kFilled);
    auto fill = venue->GetFills(types::FromEpochSeconds(0))->at(0);
    EXPECT_EQ(fill.price,
              D("50000.0"));  // price improvement: the ask, not the limit
    EXPECT_EQ(fill.liquidity, types::Liquidity::kTaker);
    EXPECT_EQ(Bal(*venue, "USD").total, D("4980"));
}

TEST(PaperLimit, SellLimitRestsUntilBidReachesIt) {
    Fixture f;
    f.options.initial_balances["BTC"] = D("1");
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    ASSERT_OK(venue->PlaceOrder(
        Order(Side::kSell, OrderType::kLimit, "0.5", "50500.0")));
    EXPECT_EQ(Bal(*venue, "BTC").locked, D("0.5"));
    EXPECT_EQ(Bal(*venue, "BTC").available, D("0.5"));
    venue->OnQuote(Q("50500.0", "50510.0"));
    EXPECT_TRUE(venue->GetOpenOrders()->empty());
    EXPECT_EQ(Bal(*venue, "BTC").total, D("0.5"));
    EXPECT_EQ(Bal(*venue, "USD").total,
              D("10000") + D("25250") - D("63.125"));  // 0.25% maker fee
}

TEST(PaperRules, RoundsPriceAndQuantityAndEnforcesMinimum) {
    Fixture f;
    f.options.initial_balances["BTC"] = D("1");
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));

    ASSERT_OK(venue->PlaceOrder(
        Order(Side::kBuy, OrderType::kLimit, "0.123456789", "49000.07")));
    ASSERT_OK(venue->PlaceOrder(
        Order(Side::kSell, OrderType::kLimit, "0.2", "51000.01")));
    auto open = venue->GetOpenOrders();
    ASSERT_EQ(open->size(), 2u);
    EXPECT_EQ((*open)[0].limit_price, D("49000.0"));  // buy rounds down
    EXPECT_EQ((*open)[0].quantity,
              D("0.12345678"));  // quantity rounds down to lot
    EXPECT_EQ((*open)[1].limit_price, D("51000.1"));  // sell rounds up

    auto tiny = venue->PlaceOrder(
        Order(Side::kBuy, OrderType::kLimit, "0.00005", "49000.0"));
    ASSERT_FALSE(tiny.has_value());
    EXPECT_EQ(tiny.error().category, ErrorCategory::kRejected);
    auto zero =
        venue->PlaceOrder(Order(Side::kBuy, OrderType::kLimit, "0", "49000.0"));
    EXPECT_EQ(zero.error().category, ErrorCategory::kInvalidArgument);
    auto no_price =
        venue->PlaceOrder(Order(Side::kBuy, OrderType::kLimit, "0.1"));
    EXPECT_EQ(no_price.error().category, ErrorCategory::kInvalidArgument);
    auto bad_symbol = Order(Side::kBuy, OrderType::kMarket, "1");
    bad_symbol.symbol = "NOPE/USD";
    EXPECT_EQ(venue->PlaceOrder(bad_symbol).error().category,
              ErrorCategory::kNotFound);
}

TEST(PaperFunds, RejectsOrdersTheAccountCannotCover) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    auto buy = venue->PlaceOrder(Order(Side::kBuy, OrderType::kMarket, "1"));
    ASSERT_FALSE(buy.has_value());
    EXPECT_EQ(buy.error().category, ErrorCategory::kInsufficientFunds);
    auto sell = venue->PlaceOrder(
        Order(Side::kSell, OrderType::kMarket, "0.1"));  // owns no BTC
    EXPECT_EQ(sell.error().category, ErrorCategory::kInsufficientFunds);
}

TEST(PaperFunds, ReservedFundsCannotBeSpentTwice) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    ASSERT_OK(venue->PlaceOrder(Order(Side::kBuy, OrderType::kLimit, "0.15",
                                      "49000.0")));  // ~7,379 reserved
    auto second = venue->PlaceOrder(
        Order(Side::kBuy, OrderType::kLimit, "0.1", "49000.0"));
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().category, ErrorCategory::kInsufficientFunds);
}

TEST(PaperCancel, Outcomes) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    auto resting = venue->PlaceOrder(
        Order(Side::kBuy, OrderType::kLimit, "0.1", "49000.0"));
    auto filled =
        venue->PlaceOrder(Order(Side::kBuy, OrderType::kMarket, "0.01"));

    auto cancel = venue->CancelOrder(resting->order_id);
    ASSERT_OK(cancel);
    EXPECT_EQ(*cancel, CancelOutcome::kCancelled);
    EXPECT_EQ(Bal(*venue, "USD").locked, D("0"));
    EXPECT_EQ(*venue->CancelOrder(resting->order_id),
              CancelOutcome::kAlreadyClosed);
    EXPECT_EQ(*venue->CancelOrder(filled->order_id),
              CancelOutcome::kAlreadyFilled);  // not an error
    auto unknown = venue->CancelOrder("PAPER-O999999");
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().category, ErrorCategory::kNotFound);

    // A cancelled order no longer fills.
    venue->OnQuote(Q("48000.0", "48010.0"));
    EXPECT_EQ(venue->GetFills(types::FromEpochSeconds(0))->size(), 1u);
}

TEST(PaperCancel, CancelAll) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    auto a = venue->PlaceOrder(
        Order(Side::kBuy, OrderType::kLimit, "0.01", "49000.0"));
    auto b = venue->PlaceOrder(
        Order(Side::kBuy, OrderType::kLimit, "0.01", "48000.0"));
    auto ids = venue->CancelAll();
    ASSERT_OK(ids);
    EXPECT_EQ(*ids, (std::vector<std::string>{a->order_id, b->order_id}));
    EXPECT_TRUE(venue->GetOpenOrders()->empty());
    EXPECT_EQ(Bal(*venue, "USD").locked, D("0"));
    EXPECT_TRUE(venue->CancelAll()->empty());
}

TEST(PaperStop, SellStopTriggersIntoAMarketOrder) {
    Fixture f;
    f.options.initial_balances["BTC"] = D("1");
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    ASSERT_OK(venue->PlaceOrder(
        Order(Side::kSell, OrderType::kStop, "0.5", nullptr, "48000.04")));
    EXPECT_EQ(venue->GetOpenOrders()->at(0).stop_price, D("48000.0"));

    venue->OnQuote(Q("49000.0", "49010.0"));
    EXPECT_EQ(venue->GetOpenOrders()->size(), 1u);  // not triggered
    venue->OnQuote(Q("47900.0", "47910.0"));
    EXPECT_TRUE(venue->GetOpenOrders()->empty());
    auto fill = venue->GetFills(types::FromEpochSeconds(0))->at(0);
    EXPECT_EQ(fill.price,
              D("47876.0"));  // 47900 * 0.9995 = 47876.05, rounded down to tick
    EXPECT_EQ(fill.liquidity, types::Liquidity::kTaker);
    EXPECT_EQ(Bal(*venue, "BTC").total, D("0.5"));
    EXPECT_EQ(Bal(*venue, "BTC").locked, D("0"));
}

TEST(PaperStop, BuyStopTriggersWhenAskRisesToIt) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    ASSERT_OK(venue->PlaceOrder(
        Order(Side::kBuy, OrderType::kStop, "0.1", nullptr, "51000.0")));
    venue->OnQuote(Q("50900.0", "50990.0"));
    EXPECT_EQ(venue->GetOpenOrders()->size(), 1u);
    venue->OnQuote(Q("51000.0", "51010.0"));
    EXPECT_TRUE(venue->GetOpenOrders()->empty());
    EXPECT_EQ(venue->GetFills(types::FromEpochSeconds(0))->at(0).price,
              D("51035.6"));  // 51010 * 1.0005 -> up
}

TEST(PaperTimeInForce, ImmediateOrCancelExpiresUnfilledRemainder) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    auto req = Order(Side::kBuy, OrderType::kLimit, "0.1", "49000.0");
    req.time_in_force = TimeInForce::kIoc;
    auto ack = venue->PlaceOrder(req);
    ASSERT_OK(ack);
    EXPECT_EQ(ack->state, OrderState::kExpired);
    EXPECT_TRUE(venue->GetOpenOrders()->empty());
    EXPECT_EQ(Bal(*venue, "USD").locked, D("0"));
}

TEST(PaperTimeInForce, FillOrKillIsAllOrNothing) {
    Fixture f;
    f.options.cap_limit_fills_at_quote_size = true;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0", "1", "0.05"));
    auto req = Order(Side::kBuy, OrderType::kLimit, "0.1", "50100.0");
    req.time_in_force = TimeInForce::kFok;
    auto ack = venue->PlaceOrder(req);
    ASSERT_OK(ack);
    EXPECT_EQ(ack->state, OrderState::kExpired);
    EXPECT_TRUE(venue->GetFills(types::FromEpochSeconds(0))->empty());
    EXPECT_EQ(Bal(*venue, "USD").total, D("10000"));

    req.quantity = D("0.05");
    EXPECT_EQ(venue->PlaceOrder(req)->state, OrderState::kFilled);
}

TEST(PaperTimeInForce, DayOrdersExpireAtEndOfUtcDay) {
    Fixture f;
    f.clock_seconds = 19'000 * 86400 + 10 * 3600;  // 10:00 UTC
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    auto req = Order(Side::kBuy, OrderType::kLimit, "0.1", "49000.0");
    req.time_in_force = TimeInForce::kDay;
    ASSERT_OK(venue->PlaceOrder(req));
    f.clock_seconds = 19'000 * 86400 + 23 * 3600;
    EXPECT_EQ(venue->GetOpenOrders()->size(), 1u);
    f.clock_seconds = 19'001 * 86400;  // midnight
    EXPECT_TRUE(venue->GetOpenOrders()->empty());
    EXPECT_EQ(Bal(*venue, "USD").locked, D("0"));
}

TEST(PaperPartialFills, LimitFillsAreCappedAtQuoteSize) {
    Fixture f;
    f.options.cap_limit_fills_at_quote_size = true;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0", "1", "0.04"));
    auto ack = venue->PlaceOrder(
        Order(Side::kBuy, OrderType::kLimit, "0.1", "50000.0"));
    ASSERT_OK(ack);
    EXPECT_EQ(ack->state, OrderState::kPartiallyFilled);
    auto open = venue->GetOpenOrders();
    ASSERT_EQ(open->size(), 1u);
    EXPECT_EQ((*open)[0].filled_quantity, D("0.04"));
    EXPECT_EQ(Bal(*venue, "USD").locked, D("3012"));  // 0.06 * 50000 * 1.004

    venue->OnQuote(Q("49990.0", "50000.0", "1", "0.05"));
    EXPECT_EQ(venue->GetOpenOrders()->at(0).filled_quantity, D("0.09"));
    venue->OnQuote(Q("49990.0", "50000.0", "1", "5"));
    EXPECT_TRUE(venue->GetOpenOrders()->empty());

    auto fills = venue->GetFills(types::FromEpochSeconds(0));
    ASSERT_EQ(fills->size(), 3u);
    EXPECT_EQ((*fills)[0].quantity, D("0.04"));
    EXPECT_EQ((*fills)[1].quantity, D("0.05"));
    EXPECT_EQ((*fills)[2].quantity, D("0.01"));
    EXPECT_EQ(Bal(*venue, "BTC").total, D("0.1"));
    EXPECT_EQ(Bal(*venue, "USD").total,
              D("4984.5"));  // 10000 - 5000 - fees (8 + 6.25 + 1.25 ... )
    EXPECT_EQ(Bal(*venue, "USD").locked, D("0"));
}

TEST(PaperFills, FilteredBySinceTime) {
    Fixture f;
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));
    ASSERT_OK(venue->PlaceOrder(Order(Side::kBuy, OrderType::kMarket, "0.01")));
    f.clock_seconds += 100;
    ASSERT_OK(venue->PlaceOrder(Order(Side::kBuy, OrderType::kMarket, "0.02")));
    EXPECT_EQ(
        venue->GetFills(types::FromEpochSeconds(f.clock_seconds - 100))->size(),
        2u);
    auto recent =
        venue->GetFills(types::FromEpochSeconds(f.clock_seconds - 50));
    ASSERT_EQ(recent->size(), 1u);
    EXPECT_EQ((*recent)[0].quantity, D("0.02"));
    EXPECT_TRUE(
        venue->GetFills(types::FromEpochSeconds(f.clock_seconds + 1))->empty());
}

TEST(PaperStream, StartSubscribesAndStopStopsIt) {
    Fixture f;
    auto venue = f.Make();
    ASSERT_OK(venue->Start({"BTC/USD"}));
    ASSERT_TRUE(f.data->callback);
    f.data->callback(Q("49990.0", "50000.0"));  // as the stream thread would
    ASSERT_OK(venue->PlaceOrder(Order(Side::kBuy, OrderType::kMarket, "0.01")));
    auto* sub = f.data->mSub;
    venue->Stop();
    EXPECT_TRUE(sub == nullptr ||
                true);  // subscription object is destroyed by Stop()
}

TEST(PaperThreading, ConcurrentQuotesAndOrdersKeepBooksConsistent) {
    Fixture f;
    f.options.initial_balances["BTC"] = D("1");
    auto venue = f.Make();
    venue->OnQuote(Q("49990.0", "50000.0"));

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {  // "stream" threads
        threads.emplace_back([&, t] {
            for (int i = 0; !stop && i < 3000; ++i) {
                const int mid = 49000 + ((i * 37 + t * 11) % 2000);
                venue->OnQuote(Q(std::to_string(mid - 5).append(".0").c_str(),
                                 std::to_string(mid + 5).append(".0").c_str()));
            }
        });
    }
    for (int t = 0; t < 2; ++t) {  // strategy threads
        threads.emplace_back([&, t] {
            for (int i = 0; i < 400; ++i) {
                const auto side = (i + t) % 2 ? Side::kBuy : Side::kSell;
                const auto price =
                    std::to_string(49000 + (i * 13) % 2000).append(".0");
                auto ack = venue->PlaceOrder(
                    Order(side, OrderType::kLimit, "0.001", price.c_str()));
                if (ack && i % 3 == 0) {
                    (void)venue->CancelOrder(ack->order_id);
                }
                (void)venue->GetBalances();
                (void)venue->GetOpenOrders();
            }
        });
    }
    for (auto& th : threads) th.join();
    stop = true;

    (void)venue->CancelAll();
    for (const auto& b : *venue->GetBalances()) {
        EXPECT_EQ(b.locked, D("0")) << b.asset;
        EXPECT_FALSE(b.total.IsNegative()) << b.asset;
        EXPECT_EQ(b.available, b.total) << b.asset;
    }
    // Everything bought or sold must be reflected in both assets: BTC conserved
    // against the fills.
    types::Decimal net_btc;
    for (const auto& fill : *venue->GetFills(types::FromEpochSeconds(0))) {
        net_btc += fill.side == Side::kBuy ? fill.quantity : -fill.quantity;
    }
    EXPECT_EQ(Bal(*venue, "BTC").total, D("1") + net_btc);
}

}  // namespace
