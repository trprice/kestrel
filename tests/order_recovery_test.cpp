#include "kestrel/trading/order_recovery.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace kestrel::trading {
namespace {

using types::ErrorCategory;
using types::MakeError;
using types::OrderStatus;
using types::Result;

OrderStatus Status(const std::string& id) {
    OrderStatus status;
    status.order_id = "venue-" + id;
    status.client_order_id = id;
    return status;
}

// A venue whose orders show up after `shows_after` lookups each.
struct Venue {
    std::map<std::string, int> shows_after;
    std::map<std::string, ErrorCategory> fails;
    std::vector<std::string> asked;
    int pauses = 0;

    Result<OrderStatus> Lookup(const std::string& id) {
        asked.push_back(id);
        if (auto f = fails.find(id); f != fails.end()) {
            return MakeError(f->second, "lookup failed");
        }
        auto it = shows_after.find(id);
        if (it == shows_after.end() || it->second-- > 0) {
            return MakeError(ErrorCategory::kNotFound, "no such order");
        }
        return Status(id);
    }
    PlacedLookup Look(const std::vector<std::string>& ids, int looks = 2) {
        return LookUpPlaced(
            ids, [this](const std::string& id) { return Lookup(id); }, looks,
            [this] { ++pauses; });
    }
};

TEST(LookUpPlaced, FindsAnOrderAtOnceWithoutPausing) {
    Venue venue;
    venue.shows_after["a"] = 0;
    auto looked = venue.Look({"a"});
    ASSERT_TRUE(looked.AllFound());
    EXPECT_EQ(looked.found[0]->order_id, "venue-a");
    EXPECT_FALSE(looked.failure);
    EXPECT_EQ(venue.pauses, 0);
    EXPECT_EQ(venue.asked.size(), 1u);
}

TEST(LookUpPlaced, PausesAndAsksAgainForOneThatShowsLate) {
    Venue venue;
    venue.shows_after["a"] = 1;
    auto looked = venue.Look({"a"});
    ASSERT_TRUE(looked.AllFound());
    EXPECT_EQ(venue.pauses, 1);
    EXPECT_EQ(venue.asked.size(), 2u);
}

TEST(LookUpPlaced, GivesUpAfterTheLastRoundWhenNotFound) {
    Venue venue;
    auto looked = venue.Look({"a"});
    EXPECT_FALSE(looked.found[0]);
    EXPECT_FALSE(looked.failure);  // not found, not failed
    EXPECT_EQ(venue.asked.size(), 2u);
    EXPECT_EQ(venue.pauses, 1);
}

TEST(LookUpPlaced, AsksAgainOnlyForTheLegsStillMissing) {
    Venue venue;
    venue.shows_after["limit"] = 0;
    venue.shows_after["stop"] = 1;
    auto looked = venue.Look({"limit", "stop"});
    ASSERT_TRUE(looked.AllFound());
    EXPECT_EQ(venue.asked, (std::vector<std::string>{"limit", "stop", "stop"}));
}

TEST(LookUpPlaced, StopsAtALookupThatFails) {
    Venue venue;
    venue.fails["limit"] = ErrorCategory::kTimeout;
    venue.shows_after["stop"] = 0;
    auto looked = venue.Look({"limit", "stop"});
    ASSERT_TRUE(looked.failure);
    EXPECT_EQ(looked.failure->category, ErrorCategory::kTimeout);
    EXPECT_FALSE(looked.found[0]);
    EXPECT_FALSE(looked.found[1]);  // not asked after the failure
    EXPECT_EQ(venue.asked, std::vector<std::string>{"limit"});
    EXPECT_EQ(venue.pauses, 0);
}

TEST(LookUpPlaced, KeepsALegFoundBeforeAFailure) {
    Venue venue;
    venue.shows_after["limit"] = 0;
    venue.fails["stop"] = ErrorCategory::kServer;
    auto looked = venue.Look({"limit", "stop"});
    EXPECT_TRUE(looked.found[0]);
    EXPECT_FALSE(looked.found[1]);
    EXPECT_TRUE(looked.failure);
    EXPECT_FALSE(looked.AllFound());
}

TEST(NoteLookup, SaysWhetherTheOrderMayExistOrMayStillAppear) {
    types::Error failed =
        MakeError(ErrorCategory::kTimeout, "timed out").error();
    NoteLookup(failed, "order kst1",
               MakeError(ErrorCategory::kNetwork, "down").error());
    EXPECT_NE(failed.message.find("order kst1 could not be looked up either"),
              std::string::npos);
    EXPECT_NE(failed.message.find("it may exist"), std::string::npos);

    types::Error missing =
        MakeError(ErrorCategory::kTimeout, "timed out").error();
    NoteLookup(missing, "OCO abc", std::nullopt);
    EXPECT_NE(missing.message.find(
                  "OCO abc was not found afterwards, but may still appear"),
              std::string::npos);
}

}  // namespace
}  // namespace kestrel::trading
