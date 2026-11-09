#include "kestrel/trading/session_gated_venue.hpp"

#include <gtest/gtest.h>

#include "strategy_fakes.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;

types::OcoRequest Pair() {
    types::OcoRequest pair;
    pair.symbol = "SPY";
    pair.side = types::Side::kSell;
    pair.quantity = D("1");
    pair.limit_price = D("600");
    pair.stop_price = D("580");
    return pair;
}

TEST(SessionGatedVenue, RefusesOrdersAndPairsWhileClosedAndPassesThemOpen) {
    auto venue = std::make_shared<kestrel::testing::RecordingVenue>();
    venue->oco = true;
    auto hours = std::make_shared<types::MarketHours>(
        std::make_shared<types::UsEquityCalendar>(), false);
    // Saturday 2026-11-07, then Monday 2026-11-09 at 10:00 New York time.
    types::Timestamp now = types::FromEpochSeconds(1794063600);
    trading::SessionGatedVenue gate(venue, hours, [&] { return now; });
    types::OrderRequest order;
    order.symbol = "SPY";
    order.side = types::Side::kBuy;
    order.quantity = D("1");
    auto closed = gate.PlaceOrder(order);
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error().category, types::ErrorCategory::kRejected);
    auto closed_pair = gate.PlaceOco(Pair());
    ASSERT_FALSE(closed_pair.has_value());
    EXPECT_NE(closed_pair.error().message.find("market closed"),
              std::string::npos);
    EXPECT_TRUE(venue->orders.empty());

    now = types::FromEpochSeconds(1794236400);
    ASSERT_TRUE(gate.PlaceOco(Pair()).has_value());
    EXPECT_EQ(venue->ocos.size(), 1u);
}

}  // namespace
