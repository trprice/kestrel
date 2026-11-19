#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "kestrel/backtest/backtest.hpp"
#include "kestrel/data/databento_client.hpp"
#include "kestrel/data/databento_data_source.hpp"
#include "kestrel/db/bar_store.hpp"
#include "kestrel/db/databento_import_store.hpp"
#include "kestrel/db/market_trade_store.hpp"
#include "kestrel/db/quote_store.hpp"
#include "kestrel/strategy/ma_crossover.hpp"
#include "kestrel/util/hmac.hpp"
#include "test_helpers.hpp"

namespace {

using namespace kestrel;
using kestrel::testing::D;
using types::Side;
using types::Timeframe;

class FakeHttp : public net::HttpClient {
   public:
    std::vector<net::HttpRequest> requests;
    std::vector<types::Result<net::HttpResponse>> responses;

    types::Result<net::HttpResponse> Send(
        const net::HttpRequest& request) override {
        requests.push_back(request);
        if (responses.empty()) {
            return types::MakeError(types::ErrorCategory::kInternal,
                                    "no canned HTTP response");
        }
        auto next = std::move(responses.front());
        responses.erase(responses.begin());
        return next;
    }
};

types::Timestamp T(const char* iso) { return *types::ParseTimestamp(iso); }

std::shared_ptr<const types::MarketHours> UsHours(bool extended) {
    return std::make_shared<const types::MarketHours>(
        std::make_shared<const types::UsEquityCalendar>(), extended);
}

TEST(DatabentoClientTest, GetCostGetRangeAndDatasetEndSendExpectedParameters) {
    auto http = std::make_shared<FakeHttp>();
    http->responses.push_back(net::HttpResponse{
        200,
        "{\"start\":\"2018-05-01T00:00:00Z\",\"end\":\"2025-01-03T18:00:00Z\","
        "\"schema\":{\"ohlcv-1m\":{\"end\":\"2025-01-03T17:45:00.000000000Z\"}}"
        "}",
        {}});
    http->responses.push_back(net::HttpResponse{200, "1.5e-2", {}});
    http->responses.push_back(net::HttpResponse{
        200,
        "ts_event,rtype,publisher_id,instrument_id,open,high,low,close,volume,"
        "symbol\n",
        {}});

    data::DatabentoClient::Options opts;
    opts.rest_base_url = "https://hist.databento.com/";
    opts.sleeper = [](std::chrono::milliseconds) {};
    data::DatabentoClient client("db-test-key", http, opts);

    auto avail_end =
        client.GetDatasetEnd("EQUS.MINI", data::DatabentoSchema::kOhlcv1m);
    ASSERT_OK(avail_end);
    EXPECT_EQ(*avail_end, T("2025-01-03T17:45:00Z"));
    ASSERT_EQ(http->requests.size(), 1u);
    EXPECT_EQ(http->requests[0].method, net::HttpMethod::kGet);
    EXPECT_EQ(
        http->requests[0].url,
        "https://hist.databento.com/v0/metadata.get_dataset_range?dataset="
        "EQUS.MINI");
    EXPECT_TRUE(http->requests[0].body.empty());
    EXPECT_EQ(http->requests[0].headers.at("Authorization"),
              "Basic " + util::Base64Encode("db-test-key:"));

    data::DatabentoClient::RangeRequest req;
    req.dataset = "EQUS.MINI";
    req.schema = data::DatabentoSchema::kOhlcv1m;
    req.symbols = {"SPY", "BRK.B"};
    req.start = T("2025-01-02");
    req.end = T("2025-01-03");

    auto cost = client.GetCost(req);
    ASSERT_OK(cost);
    EXPECT_EQ(*cost, D("0.015"));

    ASSERT_EQ(http->requests.size(), 2u);
    const auto& cost_req = http->requests[1];
    EXPECT_EQ(cost_req.method, net::HttpMethod::kPost);
    EXPECT_EQ(cost_req.url, "https://hist.databento.com/v0/metadata.get_cost");
    EXPECT_EQ(cost_req.headers.at("Authorization"),
              "Basic " + util::Base64Encode("db-test-key:"));
    EXPECT_NE(cost_req.body.find("dataset=EQUS.MINI"), std::string::npos);
    EXPECT_NE(cost_req.body.find("symbols=SPY%2CBRK.B"), std::string::npos);
    EXPECT_NE(cost_req.body.find("stype_in=raw_symbol"), std::string::npos);

    auto csv = client.GetRangeCsv(req);
    ASSERT_OK(csv);
    ASSERT_EQ(http->requests.size(), 3u);
    const auto& range_req = http->requests[2];
    EXPECT_EQ(range_req.url,
              "https://hist.databento.com/v0/timeseries.get_range");
    EXPECT_NE(range_req.body.find("encoding=csv"), std::string::npos);
    EXPECT_NE(range_req.body.find("pretty_px=false"), std::string::npos);
    EXPECT_NE(range_req.body.find("pretty_ts=false"), std::string::npos);
    EXPECT_NE(range_req.body.find("map_symbols=true"), std::string::npos);
    EXPECT_NE(range_req.body.find("compression=none"), std::string::npos);

    // ohlcv-1d batches up to 365 days into a single HTTP fetch (1 cost call +
    // 1 range call = 2 HTTP requests across 124 trading days) rather than one
    // request per session.
    http->requests.clear();
    http->responses.push_back(net::HttpResponse{200, "0.02", {}});
    http->responses.push_back(net::HttpResponse{
        200, "ts_event,open,high,low,close,volume,symbol\n", {}});
    data::DatabentoHttpOptions d_opts;
    d_opts.schema = data::DatabentoSchema::kOhlcv1d;
    d_opts.symbols = {"SPY"};
    d_opts.from = T("2024-01-02");
    d_opts.to = T("2024-07-01");
    d_opts.now = T("2025-01-10");
    d_opts.hours = UsHours(false);
    d_opts.max_cost = D("1.00");
    auto daily_plan = data::ImportDatabentoHttp(client, d_opts);
    ASSERT_OK(daily_plan);
    EXPECT_EQ(daily_plan->slices_planned, 124u);
    EXPECT_FALSE(daily_plan->cost_is_upper_bound);
    EXPECT_EQ(http->requests.size(), 2u);
}

TEST(DatabentoClientTest, RetriesRateLimitExplainsAvailableEndAndRefusesPairs) {
    auto http = std::make_shared<FakeHttp>();
    http->responses.push_back(std::unexpected(
        net::ErrorFromHttpStatus({429, "slow down", {{"Retry-After", "1"}}})));
    http->responses.push_back(net::HttpResponse{200, "0.25", {}});
    http->responses.push_back(std::unexpected(net::ErrorFromHttpStatus(
        {422, "{\"detail\":\"data_end_after_available_end\"}", {}})));

    data::DatabentoClient::Options opts;
    opts.sleeper = [](std::chrono::milliseconds) {};
    data::DatabentoClient client("k", http, opts);

    data::DatabentoClient::RangeRequest req;
    req.symbols = {"SPY"};
    req.start = T("2025-01-02");
    req.end = T("2025-01-03");
    auto cost = client.GetCost(req);
    ASSERT_OK(cost);
    EXPECT_EQ(*cost, D("0.25"));
    EXPECT_EQ(http->requests.size(), 2u);

    auto lag = client.GetRangeCsv(req);
    ASSERT_FALSE(lag.has_value());
    EXPECT_NE(lag.error().message.find("lags real-time"), std::string::npos);

    req.symbols = {"BTC/USD"};
    auto bad = client.GetCost(req);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().category, types::ErrorCategory::kUnsupported);
}

TEST(DatabentoClientTest, GetRangeIsNotResentAfterATimeoutButGetCostIs) {
    auto http = std::make_shared<FakeHttp>();
    // get_cost: a timeout, then the answer.
    http->responses.push_back(
        types::MakeError(types::ErrorCategory::kTimeout, "timed out"));
    http->responses.push_back(net::HttpResponse{200, "0.10", {}});
    // get_range: a timeout, which may have delivered the data: not resent.
    http->responses.push_back(
        types::MakeError(types::ErrorCategory::kTimeout, "timed out"));
    // get_range: a server error delivered nothing: resent.
    http->responses.push_back(
        std::unexpected(net::ErrorFromHttpStatus({503, "busy", {}})));
    http->responses.push_back(net::HttpResponse{
        200, "ts_event,open,high,low,close,volume,symbol\n", {}});

    data::DatabentoClient::Options opts;
    opts.sleeper = [](std::chrono::milliseconds) {};
    data::DatabentoClient client("k", http, opts);
    data::DatabentoClient::RangeRequest req;
    req.symbols = {"SPY"};
    req.start = T("2025-01-02");
    req.end = T("2025-01-03");

    auto cost = client.GetCost(req);
    ASSERT_OK(cost);
    EXPECT_EQ(http->requests.size(), 2u);

    auto timed_out = client.GetRangeCsv(req);
    ASSERT_FALSE(timed_out.has_value());
    EXPECT_EQ(timed_out.error().category, types::ErrorCategory::kTimeout);
    EXPECT_NE(timed_out.error().message.find("not sent again"),
              std::string::npos);
    EXPECT_EQ(http->requests.size(), 3u);

    auto resent = client.GetRangeCsv(req);
    ASSERT_OK(resent);
    EXPECT_EQ(http->requests.size(), 5u);
}

TEST(DatabentoClientTest, LedgerRecordsARollupOnlyOverItsWholeBuckets) {
    // An import ending mid-bucket (11:17 New York time) emits no 11:00-11:30
    // 30m bar, so the ledger records rollup:30m only to 11:00 (16:00Z), and a
    // later import covering the rest still makes that bucket.
    auto http = std::make_shared<FakeHttp>();
    http->responses.push_back(net::HttpResponse{200, "0.10", {}});
    std::ostringstream csv;
    csv << "ts_event,open,high,low,close,volume,symbol\n";
    const types::Timestamp open = T("2025-01-02T14:30:00Z");
    for (int m = 0; m < 107; ++m) {  // 14:30 to 16:16
        csv << types::ToEpochMillis(open + std::chrono::minutes(m)) *
                   1'000'000LL
            << ",580000000000,581000000000,579000000000,580500000000,100,SPY\n";
    }
    http->responses.push_back(net::HttpResponse{200, csv.str(), {}});
    data::DatabentoClient::Options c_opts;
    c_opts.sleeper = [](std::chrono::milliseconds) {};
    data::DatabentoClient client("k", http, c_opts);

    std::vector<std::pair<std::string, data::DatabentoHttpOptions::Interval>>
        recorded;
    std::size_t rollup_bars = 0;
    data::DatabentoHttpOptions opts;
    opts.schema = data::DatabentoSchema::kOhlcv1m;
    opts.symbols = {"SPY"};
    opts.rollups = {Timeframe::kM30};
    opts.from = open;
    opts.to = T("2025-01-02T16:17:00Z");
    opts.now = T("2025-01-10");
    opts.hours = UsHours(false);
    opts.max_cost = D("1");
    opts.sinks.on_bars =
        [&](const types::Symbol&, Timeframe tf,
            std::span<const types::Bar> bars) -> types::Result<void> {
        if (tf == Timeframe::kM30) {
            rollup_bars += bars.size();
        }
        return {};
    };
    opts.record_imported =
        [&](const types::Symbol&, std::span<const std::string> targets,
            std::span<const data::DatabentoHttpOptions::Interval> slices)
        -> types::Result<void> {
        for (const auto& target : targets) {
            for (const auto& slice : slices) {
                recorded.emplace_back(target, slice);
            }
        }
        return {};
    };
    auto result = data::ImportDatabentoHttp(client, opts);
    ASSERT_OK(result);
    EXPECT_EQ(rollup_bars, 3u);  // 14:30, 15:00, 15:30
    ASSERT_EQ(recorded.size(), 2u);
    EXPECT_EQ(recorded[0].first, "ohlcv-1m");
    EXPECT_EQ(recorded[0].second.first, open);
    EXPECT_EQ(recorded[0].second.second, T("2025-01-02T16:17:00Z"));
    EXPECT_EQ(recorded[1].first, "rollup:30m");
    EXPECT_EQ(recorded[1].second.first, open);
    EXPECT_EQ(recorded[1].second.second, T("2025-01-02T16:00:00Z"));
}

TEST(DatabentoCsvTest, ParsesFixedPointOhlcvFiltersHoursAndRollsUpMultiSymbol) {
    // 2025-01-02 is Thursday (EST = UTC-5): regular open 14:30Z, close 21:00Z.
    const std::string csv =
        "ts_event,rtype,publisher_id,instrument_id,open,high,low,close,volume,"
        "symbol\r\n"
        // Pre-market bar at 13:00Z (08:00 ET): skipped when regular-only.
        "1735822800000000000,33,1,10,588000000000,588500000000,587900000000,"
        "588100000000,500,SPY\r\n"
        // Unconfigured symbol MSFT: skipped.
        "1735828200000000000,33,1,11,420000000000,421000000000,419500000000,"
        "420500000000,300,MSFT\r\n"
        // Duplicate 14:30Z bar (first stale row, then updated row): both base
        // 1m and 5m rollup must reflect the second row, not the first.
        "1735828200000000000,33,1,10,570000000000,571000000000,570000000000,"
        "570500000000,9999,SPY\r\n"
        "1735828200000000000,33,1,10,589000000000,589500000000,588800000000,"
        "589250000000,1000,SPY\r\n"
        "1735828200000000000,33,1,12,510000000000,510500000000,509750000000,"
        "510250000000,800,QQQ\r\n"
        "ts_event,rtype,publisher_id,instrument_id,open,high,low,close,volume,"
        "symbol\r\n"
        "1735828260000000000,33,1,10,589250000000,590000000000,589100000000,"
        "589800000000,1200,SPY\r\n"
        "1735828380000000000,33,1,10,589800000000,589900000000,588500000000,"
        "588700000000,900,SPY\r\n"
        "1735828500000000000,33,1,10,588700000000,589100000000,588600000000,"
        "589000000000,600,SPY\r\n";

    data::DatabentoParseOptions opts;
    opts.schema = data::DatabentoSchema::kOhlcv1m;
    opts.symbols = {"SPY", "QQQ"};
    opts.rollups = {Timeframe::kM5};
    opts.hours = UsHours(/*extended=*/false);
    opts.now = T("2025-01-03");

    std::map<std::pair<types::Symbol, Timeframe>, std::vector<types::Bar>> out;
    data::DatabentoCallbacks cb;
    cb.on_bars = [&](const types::Symbol& s, Timeframe tf,
                     std::span<const types::Bar> batch) -> types::Result<void> {
        auto& dst = out[{s, tf}];
        dst.insert(dst.end(), batch.begin(), batch.end());
        return {};
    };

    std::istringstream stream(csv);
    auto stats = data::ParseDatabentoCsv(stream, opts, cb);
    ASSERT_OK(stats);
    EXPECT_EQ(stats->rows_read, 8u);
    EXPECT_EQ(stats->skipped_symbol, 1u);
    EXPECT_EQ(stats->skipped_outside, 1u);
    EXPECT_EQ(stats->bars, 5u);
    EXPECT_EQ(stats->rollup_bars, 3u);

    const auto& spy_1m = out[{"SPY", Timeframe::kM1}];
    ASSERT_EQ(spy_1m.size(), 4u);
    EXPECT_EQ(spy_1m[0].open, D("589"));
    EXPECT_EQ(spy_1m[0].volume, D("1000"));

    const auto& spy_5m = out[{"SPY", Timeframe::kM5}];
    ASSERT_EQ(spy_5m.size(), 2u);
    EXPECT_EQ(spy_5m[0].open_time, T("2025-01-02T14:30:00Z"));
    EXPECT_EQ(spy_5m[0].open, D("589"));
    EXPECT_EQ(spy_5m[0].high, D("590"));
    EXPECT_EQ(spy_5m[0].low, D("588.5"));
    EXPECT_EQ(spy_5m[0].close, D("588.7"));
    EXPECT_EQ(spy_5m[0].volume, D("3100"));  // 1000 + 1200 + 900, not 9999

    // Partial rollup buckets cut by `[from, to)` are dropped rather than
    // emitted as incomplete 5m bars.
    out.clear();
    opts.from = T("2025-01-02T14:31:00Z");  // cuts [14:30, 14:35)
    opts.to = T("2025-01-02T14:38:00Z");    // cuts [14:35, 14:40)
    std::istringstream clipped(csv);
    auto clipped_stats = data::ParseDatabentoCsv(clipped, opts, cb);
    ASSERT_OK(clipped_stats);
    EXPECT_EQ(clipped_stats->bars, 3u);
    EXPECT_EQ(clipped_stats->rollup_bars, 0u);
    EXPECT_TRUE((out[{"SPY", Timeframe::kM5}].empty()));
}

TEST(DatabentoCsvTest, HourlyBarsKeepOpeningHalfHourAndDailyBarsSkipHolidays) {
    // 2025-01-02 EST: RTH is 14:30Z..21:00Z. The 14:00Z (09:00 ET) hourly bar
    // overlaps 14:30Z..15:00Z and must be kept; 13:00Z and 21:00Z must not.
    const std::string h1_csv =
        "ts_event,open,high,low,close,volume,symbol\n"
        "2025-01-02T13:00:00Z,588.0,589.0,587.0,588.5,100,SPY\n"
        "2025-01-02T14:00:00Z,589.0,591.0,588.0,590.5,5000,SPY\n"
        "2025-01-02T20:00:00Z,590.0,592.0,589.0,591.0,4000,SPY\n"
        "2025-01-02T21:00:00Z,591.0,592.0,590.0,591.2,200,SPY\n";

    data::DatabentoParseOptions opts;
    opts.schema = data::DatabentoSchema::kOhlcv1h;
    opts.symbols = {"SPY"};
    opts.hours = UsHours(false);
    opts.from = T("2025-01-02T14:30:00Z");
    opts.to = T("2025-01-02T21:00:00Z");
    opts.now = T("2025-01-03");

    Timeframe expected_tf = Timeframe::kH1;
    std::vector<types::Bar> bars;
    data::DatabentoCallbacks cb;
    cb.on_bars = [&](const types::Symbol&, Timeframe tf,
                     std::span<const types::Bar> batch) -> types::Result<void> {
        EXPECT_EQ(tf, expected_tf);
        bars.insert(bars.end(), batch.begin(), batch.end());
        return {};
    };
    {
        std::istringstream s(h1_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    ASSERT_EQ(bars.size(), 2u);
    EXPECT_EQ(bars[0].open_time, T("2025-01-02T14:00:00Z"));
    EXPECT_EQ(bars[1].open_time, T("2025-01-02T20:00:00Z"));

    // Daily: 2025-01-01 is closed, 2025-01-02 is open, 2025-01-03 is forming.
    bars.clear();
    expected_tf = Timeframe::kD1;
    opts.schema = data::DatabentoSchema::kOhlcv1d;
    opts.symbols = {"SPY", "BRK.B"};
    opts.from.reset();
    opts.to.reset();
    opts.now = T("2025-01-03T18:00:00Z");
    const std::string d1_csv =
        "ts_event,open,high,low,close,volume,symbol\n"
        "2025-01-01T00:00:00Z,585.0,586.0,584.0,585.5,1000,SPY\n"
        "2025-01-02T00:00:00Z,589.0,591.25,588.0,590.5,500000,\"BRK.B\"\n"
        "2025-01-03T00:00:00Z,590.5,592.0,590.0,591.0,200000,BRK.B\n";
    {
        std::istringstream s(d1_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    ASSERT_EQ(bars.size(), 1u);
    EXPECT_EQ(bars[0].open_time, T("2025-01-02"));
}

TEST(DatabentoCsvTest, ParsesQuotesAndTradesPreservingSeqAndCalendarDate) {
    const std::string bbo_csv =
        "ts_recv,ts_event,price,size,side,bid_px_00,ask_px_00,bid_sz_00,"
        "ask_sz_00,symbol\n"
        "1735828201000000000,18446744073709551615,9223372036854775807,0,N,"
        "589100000000,9223372036854775807,200,0,SPY\n"
        "1735828201000000000,1735828200999000000,589150000000,50,B,"
        "589100000000,589200000000,200,150,SPY\n"
        // Concatenated earlier day (>1h jump backward) resets NextId cleanly.
        "1735741801000000000,1735741801000000000,585000000000,10,A,"
        "584900000000,585100000000,100,100,SPY\n";

    data::DatabentoParseOptions opts;
    opts.schema = data::DatabentoSchema::kBbo1s;
    opts.symbols = {"SPY"};
    opts.hours = types::MarketHours::AlwaysOpen();
    opts.now = T("2025-01-10");

    std::vector<types::Quote> quotes;
    data::DatabentoCallbacks cb;
    cb.on_quotes =
        [&](std::span<const types::Quote> batch) -> types::Result<void> {
        quotes.insert(quotes.end(), batch.begin(), batch.end());
        return {};
    };
    {
        std::istringstream s(bbo_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    ASSERT_EQ(quotes.size(), 3u);
    EXPECT_EQ(quotes[0].seq, 1735828201000000000LL);
    EXPECT_EQ(quotes[1].seq, 1735828201000000001LL);
    EXPECT_EQ(quotes[2].seq, 1735741801000000000LL);

    // A burst straddling the session open: two pre-open rows in the
    // nanosecond before it, then a row at the open. The open row's seq is its
    // own timestamp whether the pre-open rows are filtered out, kept, or (in
    // an HTTP slice starting at the open) never sent: they are in another
    // millisecond.
    const std::string burst_csv =
        "ts_recv,bid_px_00,ask_px_00,bid_sz_00,ask_sz_00,symbol\n"
        "1735828199999999999,589000000000,589100000000,10,10,SPY\n"
        "1735828199999999999,589000000000,589100000000,10,10,SPY\n"
        "1735828200000000000,589000000000,589100000000,10,10,SPY\n";
    quotes.clear();
    opts.hours = UsHours(false);
    {
        std::istringstream s(burst_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    ASSERT_EQ(quotes.size(), 1u);
    EXPECT_EQ(quotes[0].seq, 1735828200000000000LL);

    std::vector<types::Quote> unfiltered;
    cb.on_quotes =
        [&](std::span<const types::Quote> batch) -> types::Result<void> {
        unfiltered.insert(unfiltered.end(), batch.begin(), batch.end());
        return {};
    };
    opts.hours = types::MarketHours::AlwaysOpen();
    {
        std::istringstream s(burst_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    ASSERT_EQ(unfiltered.size(), 3u);
    EXPECT_EQ(unfiltered[0].seq, 1735828199999999999LL);
    EXPECT_EQ(unfiltered[1].seq, 1735828200000000000LL);  // moved up
    EXPECT_EQ(unfiltered[2].seq, quotes[0].seq);

    // Only the open row, as an HTTP slice from the open sends it: the same seq.
    std::vector<types::Quote> sliced;
    cb.on_quotes =
        [&](std::span<const types::Quote> batch) -> types::Result<void> {
        sliced.insert(sliced.end(), batch.begin(), batch.end());
        return {};
    };
    {
        std::istringstream s(
            "ts_recv,bid_px_00,ask_px_00,bid_sz_00,ask_sz_00,symbol\n"
            "1735828200000000000,589000000000,589100000000,10,10,SPY\n");
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    ASSERT_EQ(sliced.size(), 1u);
    EXPECT_EQ(sliced[0].seq, quotes[0].seq);

    // mbp-1 with extended hours in winter: 19:30 ET is 00:30Z the next UTC day,
    // which is still the same New York trading date, so `last` carries across
    // 00:00Z.
    quotes.clear();
    cb.on_quotes =
        [&](std::span<const types::Quote> batch) -> types::Result<void> {
        quotes.insert(quotes.end(), batch.begin(), batch.end());
        return {};
    };
    opts.schema = data::DatabentoSchema::kMbp1;
    opts.hours = UsHours(/*extended=*/true);
    const std::string mbp_csv =
        "ts_recv,ts_event,action,side,price,size,bid_px_00,ask_px_00,"
        "bid_sz_00,ask_sz_00,symbol\n"
        "2025-01-02T23:55:00.000000000Z,2025-01-02T23:55:00.000000000Z,T,A,"
        "589.100000000,25,589.100000000,589.200000000,100,100,SPY\n"
        "2025-01-03T00:30:00.000000000Z,2025-01-03T00:30:00.000000000Z,A,B,"
        "585.000000000,500,589.120000000,589.200000000,150,100,SPY\n";
    {
        std::istringstream s(mbp_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    ASSERT_EQ(quotes.size(), 2u);
    EXPECT_EQ(quotes[1].last, D("589.1"));
    EXPECT_EQ(quotes[1].last_taker_side, Side::kSell);

    // trades: non-'T' action rows are ignored; 'N' uses the tick rule.
    opts.schema = data::DatabentoSchema::kTrades;
    opts.hours = UsHours(false);
    std::vector<types::MarketTrade> trades;
    cb.on_trades =
        [&](const types::Symbol&,
            std::span<const types::MarketTrade> batch) -> types::Result<void> {
        trades.insert(trades.end(), batch.begin(), batch.end());
        return {};
    };
    const std::string trades_csv =
        "ts_recv,ts_event,action,price,size,side,symbol\n"
        "1735828201000000000,1735828201000000000,T,589100000000,100,B,SPY\n"
        "1735828201500000000,1735828201500000000,C,0,0,N,SPY\n"
        "1735828202000000000,1735828202000000000,T,589050000000,40,N,SPY\n";
    {
        std::istringstream s(trades_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[1].taker_side, Side::kSell);

    // Records come in ts_recv order; ts_event can step back between them. The
    // IDs follow ts_recv, so the three trades get their own receive times.
    trades.clear();
    const std::string unordered_csv =
        "ts_recv,ts_event,action,price,size,side,symbol\n"
        "1735828203000001000,1735828203000000000,T,589100000000,10,B,SPY\n"
        "1735828203000002000,1735828203000001500,T,589100000000,20,B,SPY\n"
        "1735828203000003000,1735828203000000500,T,589100000000,30,B,SPY\n";
    {
        std::istringstream s(unordered_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].trade_id, 1735828203000001000LL);
    EXPECT_EQ(trades[1].trade_id, 1735828203000002000LL);
    EXPECT_EQ(trades[2].trade_id, 1735828203000003000LL);

    // Without ts_recv, ts_event steps back within a millisecond: the third
    // trade, at the first one's time, is moved up past the IDs before it.
    trades.clear();
    const std::string event_only_csv =
        "ts_event,action,price,size,side,symbol\n"
        "1735828204000000000,T,589100000000,10,B,SPY\n"
        "1735828204000001000,T,589100000000,20,B,SPY\n"
        "1735828204000000000,T,589100000000,30,B,SPY\n";
    {
        std::istringstream s(event_only_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].trade_id, 1735828204000000000LL);
    EXPECT_EQ(trades[1].trade_id, 1735828204000001000LL);
    EXPECT_EQ(trades[2].trade_id, 1735828204000001001LL);

    // A trade whose ts_event is before a slice's edge and ts_recv after it
    // belongs to the later slice, as the request's range selects on ts_recv:
    // kept there, and not in the slice before.
    const std::string edge_csv =
        "ts_recv,ts_event,action,price,size,side,symbol\n"
        "1735830000000000500,1735829999999999900,T,589100000000,10,B,SPY\n";
    trades.clear();
    opts.from = T("2025-01-02T15:00:00Z");
    opts.to = T("2025-01-02T16:00:00Z");
    {
        std::istringstream s(edge_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    EXPECT_EQ(trades.size(), 1u);
    trades.clear();
    opts.from = T("2025-01-02T14:30:00Z");
    opts.to = T("2025-01-02T15:00:00Z");
    {
        std::istringstream s(edge_csv);
        ASSERT_OK(data::ParseDatabentoCsv(s, opts, cb));
    }
    EXPECT_TRUE(trades.empty());
}

TEST(DatabentoCsvTest, RejectsMalformedAndMultiPublisherInputs) {
    auto parse = [](data::DatabentoSchema schema, const std::string& csv,
                    std::vector<Timeframe> rollups = {}) {
        data::DatabentoParseOptions opts;
        opts.schema = schema;
        opts.symbols = {"SPY"};
        opts.rollups = std::move(rollups);
        opts.now = T("2025-01-10");
        std::istringstream s(csv);
        return data::ParseDatabentoCsv(s, opts, {});
    };

    auto multi_pub =
        parse(data::DatabentoSchema::kOhlcv1m,
              "ts_event,publisher_id,open,high,low,close,volume,symbol\n"
              "1735828200000000000,1,589000000000,590000000000,588000000000,"
              "589000000000,100,SPY\n"
              "1735828200000000000,2,589000000000,590000000000,588000000000,"
              "589000000000,100,SPY\n");
    ASSERT_FALSE(multi_pub.has_value());
    EXPECT_NE(multi_pub.error().message.find("publisher_id"),
              std::string::npos);

    auto out_of_order =
        parse(data::DatabentoSchema::kOhlcv1m,
              "ts_event,open,high,low,close,volume,symbol\n"
              "1735828260000000000,589000000000,590000000000,588000000000,"
              "589000000000,100,SPY\n"
              "1735828200000000000,589000000000,590000000000,588000000000,"
              "589000000000,100,SPY\n",
              {Timeframe::kM5});
    ASSERT_FALSE(out_of_order.has_value());
    EXPECT_NE(out_of_order.error().message.find("chronological"),
              std::string::npos);

    auto mixed = parse(
        data::DatabentoSchema::kOhlcv1m,
        "ts_event,open,high,low,close,volume,symbol\n"
        "1735828200000000000,589000000000,590.0,588000000000,589000000000,100,"
        "SPY\n");
    ASSERT_FALSE(mixed.has_value());
    EXPECT_NE(mixed.error().message.find("fixed-point"), std::string::npos);

    EXPECT_FALSE(parse(data::DatabentoSchema::kOhlcv1m,
                       "ts_event,open,high,low,close,volume,\"symbol\n"));
    EXPECT_FALSE(
        parse(data::DatabentoSchema::kOhlcv1m,
              "ts_event,open,high,low,close,volume,symbol\n"
              "1735828200000000000,589.0,590.0,588.0,589.0,100,\"SPY\"bad\n"));
    EXPECT_FALSE(parse(data::DatabentoSchema::kMbp1,
                       "ts_recv,bid_px_00,ask_px_00,bid_sz_00,ask_sz_00\n"));
    EXPECT_FALSE(
        data::ValidateDatabentoRollups(data::DatabentoSchema::kOhlcv1h,
                                       std::vector<Timeframe>{Timeframe::kH2}));
}

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

class DatabentoDbTest : public ::testing::Test {
   protected:
    static constexpr const char* kTestVenue = "databento_test_venue";

    void SetUp() override {
        if (Env("KESTREL_TEST_DBNAME").empty()) {
            GTEST_SKIP() << "KESTREL_TEST_DBNAME not set";
        }
        db::ConnectionOptions options;
        options.dbname = Env("KESTREL_TEST_DBNAME");
        options.host = Env("KESTREL_TEST_DBHOST");
        options.user = Env("KESTREL_TEST_DBUSER");
        options.password = Env("KESTREL_TEST_DBPASSWORD");
        if (const auto port = Env("KESTREL_TEST_DBPORT"); !port.empty()) {
            options.port = std::stoi(port);
        }
        auto database = db::Database::Connect(options);
        ASSERT_OK(database);
        mDatabase = std::move(*database);
        ASSERT_OK(mDatabase->Migrate());
        Clean();
    }

    void TearDown() override {
        if (mDatabase) {
            Clean();
        }
    }

    void Clean() {
        for (const char* table :
             {"bars", "quotes", "market_trades", "databento_imports"}) {
            (void)mDatabase->Query(
                std::string("DELETE FROM ") + table + " WHERE venue = $1",
                {kTestVenue});
        }
    }

    std::shared_ptr<db::Database> mDatabase;
};

TEST_F(DatabentoDbTest, FindsBarsOffAnotherGrid) {
    db::BarStore bar_store(mDatabase);
    db::DatabentoImportStore ledger(mDatabase);
    const auto bar = [](const char* iso) {
        types::Bar b;
        b.open_time = T(iso);
        b.open = b.high = b.low = b.close = D("100");
        b.volume = D("1");
        return b;
    };
    // Rollup 1h bars from the 09:30 open, in winter and in summer time.
    const std::vector<types::Bar> rolled{bar("2025-01-02T14:30:00Z"),
                                         bar("2025-07-01T13:30:00Z")};
    ASSERT_OK(bar_store.Save(kTestVenue, "SPY", Timeframe::kH1, rolled));

    // On the 09:30 grid, in New York time either side of the clock change.
    auto same = ledger.FindOffGridBar(kTestVenue, "SPY", Timeframe::kH1,
                                      9 * 60 + 30, 60);
    ASSERT_OK(same);
    EXPECT_FALSE(same->has_value());
    // Off the hour grid of native ohlcv-1h bars and of extended-hours rollups
    // (from 07:00).
    auto hourly =
        ledger.FindOffGridBar(kTestVenue, "SPY", Timeframe::kH1, 0, 60);
    ASSERT_OK(hourly);
    ASSERT_TRUE(hourly->has_value());
    EXPECT_EQ(**hourly, T("2025-01-02T14:30:00Z"));
    auto extended =
        ledger.FindOffGridBar(kTestVenue, "SPY", Timeframe::kH1, 7 * 60, 60);
    ASSERT_OK(extended);
    EXPECT_TRUE(extended->has_value());
    // Another symbol or timeframe has nothing stored.
    auto other =
        ledger.FindOffGridBar(kTestVenue, "QQQ", Timeframe::kH1, 0, 60);
    ASSERT_OK(other);
    EXPECT_FALSE(other->has_value());
    // Before the anchor: an extended-hours 07:00 bar is off the 09:30 grid.
    ASSERT_OK(
        bar_store.Save(kTestVenue, "IWM", Timeframe::kH1,
                       std::vector<types::Bar>{bar("2025-01-02T12:00:00Z")}));
    auto early = ledger.FindOffGridBar(kTestVenue, "IWM", Timeframe::kH1,
                                       9 * 60 + 30, 60);
    ASSERT_OK(early);
    EXPECT_TRUE(early->has_value());
    auto early_extended =
        ledger.FindOffGridBar(kTestVenue, "IWM", Timeframe::kH1, 7 * 60, 60);
    ASSERT_OK(early_extended);
    EXPECT_FALSE(early_extended->has_value());
    auto two_hours =
        ledger.FindOffGridBar(kTestVenue, "SPY", Timeframe::kH2, 0, 120);
    ASSERT_OK(two_hours);
    EXPECT_FALSE(two_hours->has_value());
}

TEST_F(DatabentoDbTest,
       LedgerTracksSparseAndInterruptedSlicesAndBacktestReplaysData) {
    db::BarStore bar_store(mDatabase);
    db::QuoteStore quote_store(mDatabase);
    db::DatabentoImportStore ledger(mDatabase);

    // 1. Out-of-order day quote imports + re-import idempotency.
    const std::string fri_quotes =
        "ts_recv,ts_event,bid_px_00,ask_px_00,bid_sz_00,ask_sz_00,symbol\n"
        "2025-01-03T14:30:01.000000000Z,2025-01-03T14:30:01.000000000Z,"
        "591.000000000,591.020000000,100,100,SPY\n";
    const std::string thu_quotes =
        "ts_recv,ts_event,bid_px_00,ask_px_00,bid_sz_00,ask_sz_00,symbol\n"
        "2025-01-02T14:30:01.000000000Z,2025-01-02T14:30:01.000000000Z,"
        "589.000000000,589.020000000,100,100,SPY\n"
        "2025-01-02T15:00:01.000000000Z,2025-01-02T15:00:01.000000000Z,"
        "590.000000000,590.020000000,100,100,SPY\n";

    data::DatabentoParseOptions q_opts;
    q_opts.schema = data::DatabentoSchema::kBbo1s;
    q_opts.venue = kTestVenue;
    q_opts.symbols = {"SPY"};
    q_opts.hours = UsHours(false);
    q_opts.now = T("2025-01-10");

    std::size_t saved_quotes = 0;
    data::DatabentoCallbacks cb;
    cb.on_quotes =
        [&](std::span<const types::Quote> batch) -> types::Result<void> {
        auto n = quote_store.Save(batch);
        if (!n) {
            return std::unexpected(n.error());
        }
        saved_quotes += *n;
        return {};
    };
    {
        std::istringstream s(fri_quotes);
        ASSERT_OK(data::ParseDatabentoCsv(s, q_opts, cb));
    }
    {
        std::istringstream s(thu_quotes);
        ASSERT_OK(data::ParseDatabentoCsv(s, q_opts, cb));
    }
    {
        std::istringstream s(thu_quotes);
        ASSERT_OK(data::ParseDatabentoCsv(s, q_opts, cb));
    }
    EXPECT_EQ(saved_quotes, 3u);

    auto loaded =
        quote_store.Load(kTestVenue, "SPY", T("2025-01-02"), T("2025-01-04"));
    ASSERT_OK(loaded);
    ASSERT_EQ(loaded->size(), 3u);
    EXPECT_EQ((*loaded)[0].bid, D("589"));
    EXPECT_EQ((*loaded)[1].bid, D("590"));
    EXPECT_EQ((*loaded)[2].bid, D("591"));

    // 2. Import Thursday 2025-01-02 1m + 30m bars (with a duplicate first row)
    // and record Thursday in `databento_imports`.
    std::ostringstream thu_bars;
    thu_bars << "ts_event,open,high,low,close,volume,symbol\n"
             << "1735828200000000000,570000000000,571000000000,570000000000,"
                "570500000000,10,SPY\n";
    const types::Timestamp thu_open = T("2025-01-02T14:30:00Z");
    for (int m = 0; m < 390; ++m) {
        const auto t = thu_open + std::chrono::minutes(m);
        const std::int64_t px = (580 + (m / 30)) * 1'000'000'000LL;
        thu_bars << (types::ToEpochMillis(t) * 1'000'000LL) << ',' << px << ','
                 << (px + 1'000'000'000LL) << ',' << px << ','
                 << (px + 500'000'000LL) << ",100,SPY\n";
    }
    data::DatabentoParseOptions b_opts;
    b_opts.schema = data::DatabentoSchema::kOhlcv1m;
    b_opts.venue = kTestVenue;
    b_opts.symbols = {"SPY"};
    b_opts.rollups = {Timeframe::kM30};
    b_opts.hours = UsHours(false);
    b_opts.now = T("2025-01-10");
    cb.on_bars = [&](const types::Symbol& s, Timeframe tf,
                     std::span<const types::Bar> batch) -> types::Result<void> {
        auto n = bar_store.Save(kTestVenue, s, tf, batch);
        if (!n) {
            return std::unexpected(n.error());
        }
        return {};
    };
    {
        std::istringstream s(thu_bars.str());
        ASSERT_OK(data::ParseDatabentoCsv(s, b_opts, cb));
    }
    const auto targets =
        data::DatabentoLedgerTargets(data::DatabentoSchema::kOhlcv1m,
                                     std::vector<Timeframe>{Timeframe::kM30});
    const std::vector<db::DatabentoImportStore::Interval> thu_slice{
        {T("2025-01-02T14:30:00Z"), T("2025-01-02T21:00:00Z")}};
    ASSERT_OK(
        ledger.Record(kTestVenue, "EQUS.MINI", "SPY", targets, thu_slice));

    // 3. HTTP planner with ledger:
    // - Thursday is recorded in the ledger -> skipped.
    // - Friday has only ONE sparse bar (no bar at close): once imported, the
    //   ledger records Friday as complete, so a second resume skips Friday too
    //   with 0 HTTP calls!
    // - Adding a new `--rollup 15m` later sees `rollup:15m` missing from the
    //   ledger and re-queues both days!
    auto http = std::make_shared<FakeHttp>();
    http->responses.push_back(net::HttpResponse{200, "2.50", {}});  // > 1.00
    http->responses.push_back(net::HttpResponse{200, "0.40", {}});  // <= 1.00
    http->responses.push_back(net::HttpResponse{
        200,
        "ts_event,open,high,low,close,volume,symbol\n"
        "1735914600000000000,592000000000,593000000000,591000000000,"
        "592500000000,200,SPY\n",
        {}});
    data::DatabentoClient::Options c_opts;
    c_opts.sleeper = [](std::chrono::milliseconds) {};
    data::DatabentoClient client("k", http, c_opts);

    data::DatabentoHttpOptions h_opts;
    h_opts.dataset = "EQUS.MINI";
    h_opts.venue = kTestVenue;
    h_opts.schema = data::DatabentoSchema::kOhlcv1m;
    h_opts.symbols = {"SPY"};
    h_opts.rollups = {Timeframe::kM30};
    h_opts.from = T("2025-01-02");
    h_opts.to = T("2025-01-06");
    h_opts.now = T("2025-01-10");
    h_opts.hours = UsHours(false);
    h_opts.max_cost = D("1.00");
    h_opts.sinks.on_bars = cb.on_bars;
    h_opts.load_imported = [&](const types::Symbol& sym,
                               const std::string& target) {
        return ledger.Load(kTestVenue, h_opts.dataset, sym, target, h_opts.from,
                           h_opts.to);
    };
    h_opts.record_imported =
        [&](const types::Symbol& sym, std::span<const std::string> tgs,
            std::span<const data::DatabentoHttpOptions::Interval> slices) {
            return ledger.Record(kTestVenue, h_opts.dataset, sym, tgs, slices);
        };

    auto refused = data::ImportDatabentoHttp(client, h_opts);
    ASSERT_OK(refused);
    EXPECT_TRUE(refused->cost_exceeded);
    EXPECT_EQ(refused->slices_skipped, 1u);
    EXPECT_EQ(refused->slices_planned, 1u);
    ASSERT_EQ(http->requests.size(), 1u);

    auto imported = data::ImportDatabentoHttp(client, h_opts);
    ASSERT_OK(imported);
    EXPECT_FALSE(imported->cost_exceeded);
    EXPECT_EQ(imported->parsed.bars, 1u);

    // Sparse Friday is now recorded in the ledger: re-running makes 0 HTTP
    // calls and plans 0 slices!
    http->requests.clear();
    auto resumed = data::ImportDatabentoHttp(client, h_opts);
    ASSERT_OK(resumed);
    EXPECT_EQ(resumed->slices_skipped, 2u);
    EXPECT_EQ(resumed->slices_planned, 0u);
    EXPECT_TRUE(http->requests.empty());

    // Asking for an additional rollup (`15m`) not yet in the ledger queues
    // both days again (2 session cost queries).
    http->responses.push_back(net::HttpResponse{200, "0.20", {}});
    http->responses.push_back(net::HttpResponse{200, "0.25", {}});
    h_opts.rollups = {Timeframe::kM15, Timeframe::kM30};
    h_opts.dry_run = true;
    auto new_rollup = data::ImportDatabentoHttp(client, h_opts);
    ASSERT_OK(new_rollup);
    EXPECT_EQ(new_rollup->slices_planned, 2u);
    EXPECT_EQ(new_rollup->estimated_cost, D("0.45"));

    // Given the stored 1m bars, the new rollup is made from them instead: no
    // download, no cost, and the ledger then records it.
    h_opts.dry_run = false;
    h_opts.load_stored_bars = [&](const types::Symbol& sym,
                                  types::Timestamp start,
                                  types::Timestamp end) {
        return bar_store.Load(kTestVenue, sym, Timeframe::kM1, start, end);
    };
    http->requests.clear();
    auto rolled = data::ImportDatabentoHttp(client, h_opts);
    ASSERT_OK(rolled);
    EXPECT_EQ(rolled->slices_from_stored, 2u);
    EXPECT_EQ(rolled->slices_planned, 0u);
    EXPECT_EQ(rolled->estimated_cost, D("0"));
    EXPECT_EQ(rolled->parsed.bars, 0u);
    EXPECT_TRUE(http->requests.empty());
    auto m15 = bar_store.Load(kTestVenue, "SPY", Timeframe::kM15,
                              T("2025-01-02"), T("2025-01-03"));
    ASSERT_OK(m15);
    ASSERT_EQ(m15->size(), 26u);  // 09:30 to 16:00 in quarter hours
    EXPECT_EQ(m15->front().open_time, T("2025-01-02T14:30:00Z"));
    EXPECT_EQ(m15->front().open, D("580"));     // the stored 14:30 bar's open
    EXPECT_EQ(m15->front().volume, D("1500"));  // 15 bars of 100
    auto again = data::ImportDatabentoHttp(client, h_opts);
    ASSERT_OK(again);
    EXPECT_EQ(again->slices_skipped, 2u);
    EXPECT_EQ(again->slices_from_stored, 0u);
    h_opts.load_stored_bars = nullptr;
    h_opts.dry_run = true;

    // Near-present `to` (within 24h of `now`) queries
    // `metadata.get_dataset_range` and clamps `to` cleanly (contiguous
    // 14:30..15:30 + 15:30..16:00 coalesce into 1 exact cost query).
    http->requests.clear();
    http->responses.push_back(net::HttpResponse{
        200,
        "{\"end\":\"2025-01-03T16:00:00Z\",\"schema\":{\"tbbo\":{\"end\":"
        "\"2025-01-03T16:00:00Z\"}}}",
        {}});
    http->responses.push_back(net::HttpResponse{200, "0.05", {}});
    data::DatabentoHttpOptions recent_opts;
    recent_opts.venue = kTestVenue;
    recent_opts.schema = data::DatabentoSchema::kTbbo;
    recent_opts.symbols = {"SPY"};
    recent_opts.from = T("2025-01-03T14:30:00Z");
    recent_opts.to = T("2025-01-03T20:00:00Z");
    recent_opts.now = T("2025-01-03T20:05:00Z");  // within 24h!
    recent_opts.hours = UsHours(false);
    recent_opts.dry_run = true;
    auto clamped = data::ImportDatabentoHttp(client, recent_opts);
    ASSERT_OK(clamped);
    ASSERT_TRUE(clamped->clamped_to.has_value());
    EXPECT_EQ(*clamped->clamped_to, T("2025-01-03T16:00:00Z"));
    // 14:30..15:30 and 15:30..16:00 = 2 slices (not 6), 1 exact cost query.
    EXPECT_EQ(clamped->slices_planned, 2u);
    EXPECT_EQ(clamped->estimated_cost, D("0.05"));
    EXPECT_EQ(http->requests.size(), 2u);
    EXPECT_EQ(http->requests[0].method, net::HttpMethod::kGet);

    // If `metadata.get_dataset_range` fails, `ImportDatabentoHttp` logs a
    // warning and falls back to `min(to, now)` rather than aborting.
    http->requests.clear();
    http->responses.push_back(std::unexpected(
        net::ErrorFromHttpStatus({405, "Method Not Allowed", {}})));
    http->responses.push_back(net::HttpResponse{200, "0.12", {}});
    auto fallback = data::ImportDatabentoHttp(client, recent_opts);
    ASSERT_OK(fallback);
    EXPECT_FALSE(fallback->clamped_to.has_value());
    // Unclamped 14:30..20:00 = 6 slices (5h + 30m), 1 contiguous cost query.
    EXPECT_EQ(fallback->slices_planned, 6u);
    EXPECT_EQ(fallback->estimated_cost, D("0.12"));

    // mbp-1, the densest schema, is fetched a quarter of an hour at a time:
    // 14:30..20:00 is 22 slices.
    http->responses.push_back(std::unexpected(
        net::ErrorFromHttpStatus({405, "Method Not Allowed", {}})));
    http->responses.push_back(net::HttpResponse{200, "0.30", {}});
    data::DatabentoHttpOptions mbp_opts = recent_opts;
    mbp_opts.schema = data::DatabentoSchema::kMbp1;
    auto mbp = data::ImportDatabentoHttp(client, mbp_opts);
    ASSERT_OK(mbp);
    EXPECT_EQ(mbp->slices_planned, 22u);

    // 4. End-to-end backtest replay using DatabentoDataSource on `kTestVenue`.
    data::DatabentoDataSource source(
        data::DatabentoDataSource::Options{false, kTestVenue});
    strategy::MaCrossoverParams params;
    params.symbol = "SPY";
    params.timeframe = Timeframe::kM30;
    params.fast_period = 2;
    params.slow_period = 4;
    params.quantity = D("2");
    auto strat = std::make_unique<strategy::MaCrossover>(params);

    backtest::StoredData stored;
    stored.bars = &bar_store;
    stored.quotes = &quote_store;
    auto replay = backtest::LoadReplayData(source, strat->Needs(),
                                           T("2025-01-02T17:00:00Z"),
                                           T("2025-01-02T21:00:00Z"), stored);
    ASSERT_OK(replay);

    backtest::BacktestOptions bt_opts;
    bt_opts.start = T("2025-01-02T17:00:00Z");
    bt_opts.end = T("2025-01-02T21:00:00Z");
    bt_opts.paper.initial_balances = {{"USD", D("10000")}};
    auto result = backtest::RunBacktest(*replay, std::move(strat), bt_opts);
    ASSERT_OK(result);
    EXPECT_GT(result->bars_replayed, 0u);
}

}  // namespace
