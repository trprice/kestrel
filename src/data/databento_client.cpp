#include "kestrel/data/databento_client.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

#include "kestrel/util/hmac.hpp"
#include "kestrel/util/json.hpp"
#include "kestrel/util/url.hpp"

namespace kestrel::data {

using types::Decimal;
using types::ErrorCategory;
using types::MakeError;
using types::Result;

namespace {

constexpr std::size_t kBatchSize = 1000;
constexpr std::size_t kMaxDailySlicesPerRequest = 365;
constexpr std::int64_t kUndefPrice = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t kUndefTimestamp =
    std::numeric_limits<std::uint64_t>::max();

struct SchemaEntry {
    DatabentoSchema schema;
    std::string_view name;
};

constexpr SchemaEntry kSchemas[] = {
    {DatabentoSchema::kOhlcv1m, "ohlcv-1m"},
    {DatabentoSchema::kOhlcv1h, "ohlcv-1h"},
    {DatabentoSchema::kOhlcv1d, "ohlcv-1d"},
    {DatabentoSchema::kBbo1s, "bbo-1s"},
    {DatabentoSchema::kBbo1m, "bbo-1m"},
    {DatabentoSchema::kTbbo, "tbbo"},
    {DatabentoSchema::kMbp1, "mbp-1"},
    {DatabentoSchema::kTrades, "trades"},
};

bool IsTickSchema(DatabentoSchema schema) {
    return schema == DatabentoSchema::kTbbo ||
           schema == DatabentoSchema::kMbp1 ||
           schema == DatabentoSchema::kTrades;
}

// The longest request for a tick schema. A response is held whole in memory
// before it is parsed (net::HttpClient does not stream), so the densest
// schema, mbp-1 (every top-of-book change: an hour of SPY can run to hundreds
// of MB), is fetched a quarter of an hour at a time.
std::chrono::minutes TickSliceLength(DatabentoSchema schema) {
    return schema == DatabentoSchema::kMbp1 ? std::chrono::minutes(15)
                                            : std::chrono::minutes(60);
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

bool AllDigits(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
}

Result<void> SplitCsvLine(std::string_view line,
                          std::vector<std::string>& scratch,
                          std::vector<std::string_view>& fields,
                          std::size_t line_no) {
    fields.clear();
    scratch.clear();
    if (line.find('"') == std::string_view::npos) {
        std::size_t start = 0;
        for (;;) {
            const std::size_t comma = line.find(',', start);
            if (comma == std::string_view::npos) {
                fields.push_back(line.substr(start));
                return {};
            }
            fields.push_back(line.substr(start, comma - start));
            start = comma + 1;
        }
    }
    // Quoted CSV: unescape into `scratch` first, then point `fields` at it so
    // reallocations of `scratch` never invalidate earlier views.
    std::size_t pos = 0;
    while (pos <= line.size()) {
        if (pos < line.size() && line[pos] == '"') {
            ++pos;
            std::string unescaped;
            bool closed = false;
            while (pos < line.size()) {
                if (line[pos] == '"') {
                    if (pos + 1 < line.size() && line[pos + 1] == '"') {
                        unescaped.push_back('"');
                        pos += 2;
                        continue;
                    }
                    ++pos;
                    closed = true;
                    break;
                }
                unescaped.push_back(line[pos++]);
            }
            if (!closed) {
                return MakeError(ErrorCategory::kParse,
                                 "databento CSV line " +
                                     std::to_string(line_no) +
                                     ": unterminated quoted field");
            }
            const auto rest_end = std::min(line.find(',', pos), line.size());
            if (!Trim(line.substr(pos, rest_end - pos)).empty()) {
                return MakeError(
                    ErrorCategory::kParse,
                    "databento CSV line " + std::to_string(line_no) +
                        ": unexpected characters after closing quote");
            }
            scratch.push_back(std::move(unescaped));
            if (rest_end == line.size()) {
                break;
            }
            pos = rest_end + 1;
            if (pos == line.size()) {
                scratch.emplace_back();
                break;
            }
        } else {
            const std::size_t comma = line.find(',', pos);
            if (comma == std::string_view::npos) {
                scratch.emplace_back(line.substr(pos));
                break;
            }
            scratch.emplace_back(line.substr(pos, comma - pos));
            pos = comma + 1;
            if (pos == line.size()) {
                scratch.emplace_back();
                break;
            }
        }
    }
    fields.reserve(scratch.size());
    for (const std::string& item : scratch) {
        fields.emplace_back(item);
    }
    return {};
}

struct ParsedTime {
    types::Timestamp time;
    std::int64_t nanos = 0;
};

Result<std::optional<ParsedTime>> ParseTime(std::string_view raw,
                                            std::size_t line_no) {
    const std::string_view text = Trim(raw);
    if (text.empty()) {
        return std::optional<ParsedTime>{};
    }
    if (AllDigits(text)) {
        std::uint64_t u = 0;
        const auto [end, ec] =
            std::from_chars(text.data(), text.data() + text.size(), u);
        if (ec != std::errc() || end != text.data() + text.size()) {
            return MakeError(ErrorCategory::kParse,
                             "databento CSV line " + std::to_string(line_no) +
                                 ": invalid timestamp '" + std::string(text) +
                                 "'");
        }
        if (u == kUndefTimestamp) {
            return std::optional<ParsedTime>{};
        }
        if (u > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
            return MakeError(ErrorCategory::kParse,
                             "databento CSV line " + std::to_string(line_no) +
                                 ": timestamp out of range");
        }
        const auto ns = static_cast<std::int64_t>(u);
        return ParsedTime{types::FromEpochMillis(ns / 1'000'000), ns};
    }
    auto ts = types::ParseIso8601(text);
    if (!ts) {
        return MakeError(ErrorCategory::kParse, "databento CSV line " +
                                                    std::to_string(line_no) +
                                                    ": " + ts.error().message);
    }
    std::int64_t subsec_ns = 0;
    if (const std::size_t dot = text.find('.'); dot != std::string_view::npos) {
        int digits = 0;
        for (std::size_t i = dot + 1;
             i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            if (digits < 9) {
                subsec_ns = subsec_ns * 10 + (text[i] - '0');
                ++digits;
            }
        }
        for (; digits < 9; ++digits) {
            subsec_ns *= 10;
        }
    }
    const std::int64_t secs = std::chrono::floor<std::chrono::seconds>(*ts)
                                  .time_since_epoch()
                                  .count();
    constexpr std::int64_t kMaxSec =
        (std::numeric_limits<std::int64_t>::max() - 999'999'999LL) /
        1'000'000'000LL;
    constexpr std::int64_t kMinSec =
        std::numeric_limits<std::int64_t>::min() / 1'000'000'000LL;
    if (secs < kMinSec || secs > kMaxSec) {
        return MakeError(ErrorCategory::kParse,
                         "databento CSV line " + std::to_string(line_no) +
                             ": timestamp out of nanosecond range");
    }
    return ParsedTime{*ts, secs * 1'000'000'000LL + subsec_ns};
}

Result<std::optional<Decimal>> ParsePrice(std::string_view raw,
                                          std::optional<bool>& pretty_mode,
                                          std::size_t line_no) {
    const std::string_view text = Trim(raw);
    if (text.empty() || text == "9223372036854775807") {
        return std::optional<Decimal>{};
    }
    const bool has_decimal =
        text.find_first_of(".eE") != std::string_view::npos;
    if (has_decimal) {
        if (pretty_mode && !*pretty_mode) {
            return MakeError(ErrorCategory::kParse,
                             "databento CSV line " + std::to_string(line_no) +
                                 ": decimal price '" + std::string(text) +
                                 "' in fixed-point stream");
        }
        pretty_mode = true;
        auto d = Decimal::Parse(text);
        if (!d) {
            return MakeError(ErrorCategory::kParse,
                             "databento CSV line " + std::to_string(line_no) +
                                 ": " + d.error().message);
        }
        return d->Normalized();
    }
    if (pretty_mode && *pretty_mode) {
        auto d = Decimal::Parse(text);
        if (!d) {
            return MakeError(ErrorCategory::kParse,
                             "databento CSV line " + std::to_string(line_no) +
                                 ": " + d.error().message);
        }
        return d->Normalized();
    }
    pretty_mode = false;
    std::string_view digits = text;
    if (!digits.empty() && digits.front() == '+') {
        digits.remove_prefix(1);
    }
    std::int64_t scaled = 0;
    const auto [end, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), scaled);
    if (ec != std::errc() || end != digits.data() + digits.size()) {
        return MakeError(ErrorCategory::kParse,
                         "databento CSV line " + std::to_string(line_no) +
                             ": invalid fixed-point price '" +
                             std::string(text) + "'");
    }
    if (scaled == kUndefPrice) {
        return std::optional<Decimal>{};
    }
    return Decimal(scaled, 9).Normalized();
}

Result<Decimal> ParseQuantity(std::string_view raw, std::size_t line_no) {
    const std::string_view text = Trim(raw);
    if (text.empty()) {
        return MakeError(ErrorCategory::kParse, "databento CSV line " +
                                                    std::to_string(line_no) +
                                                    ": empty quantity");
    }
    if (AllDigits(text)) {
        std::int64_t v = 0;
        const auto [end, ec] =
            std::from_chars(text.data(), text.data() + text.size(), v);
        if (ec != std::errc() || end != text.data() + text.size() || v < 0) {
            return MakeError(ErrorCategory::kParse,
                             "databento CSV line " + std::to_string(line_no) +
                                 ": invalid quantity '" + std::string(text) +
                                 "'");
        }
        return Decimal(v, 0);
    }
    auto d = Decimal::Parse(text);
    if (!d || d->IsNegative()) {
        return MakeError(ErrorCategory::kParse,
                         "databento CSV line " + std::to_string(line_no) +
                             ": invalid quantity '" + std::string(text) + "'");
    }
    return d->Normalized();
}

struct IdState {
    bool seen = false;
    std::int64_t millis = 0;
    std::int64_t last_id = 0;
};

// Deterministic sequence/trade ID: the record's nanosecond timestamp, moved
// up past the IDs given before it in the same millisecond (a burst in one
// nanosecond, or a timestamp stepping back). It depends only on the records
// of the same millisecond, which no request splits (every Kestrel timestamp,
// and so every slice edge, is a whole millisecond), so a record gets the same
// ID whatever slice, file or filter it came in, and a re-import is skipped by
// ON CONFLICT DO NOTHING. IDs rise with time, so backfilling an earlier day
// keeps QuoteStore::Load (ORDER BY seq) chronological; records of different
// milliseconds also differ in received time, the rest of the unique key.
std::int64_t NextId(IdState& state, std::int64_t raw_ns) {
    std::int64_t millis = raw_ns / 1'000'000;
    if (raw_ns % 1'000'000 < 0) {
        --millis;
    }
    if (!state.seen || millis != state.millis) {
        state.seen = true;
        state.millis = millis;
        state.last_id = raw_ns;
    } else if (state.last_id < std::numeric_limits<std::int64_t>::max()) {
        state.last_id = std::max(raw_ns, state.last_id + 1);
    }
    return state.last_id;
}

bool InWindow(types::Timestamp t, const DatabentoParseOptions& options) {
    if (options.from && t < *options.from) {
        return false;
    }
    if (options.to && t >= *options.to) {
        return false;
    }
    return true;
}

// Caches the active `[start, end)` session and local market date so millions of
// chronological tick rows on the same trading day check session membership and
// calendar date in two integer comparisons instead of a time-zone lookup per
// row.
class SessionCache {
   public:
    explicit SessionCache(const types::MarketHours* hours) : mHours(hours) {}

    bool AlwaysOpen() const { return !mHours || mHours->IsAlwaysOpen(); }

    std::optional<types::MarketHours::Session> SessionContaining(
        types::Timestamp t) {
        if (AlwaysOpen()) {
            return std::nullopt;
        }
        if (mOpen && t >= mOpen->start && t < mOpen->end) {
            return mOpen;
        }
        if (mClosedUntil && t >= mClosedFrom && t < *mClosedUntil) {
            return std::nullopt;
        }
        const auto next = mHours->CurrentOrNext(t);
        if (!next) {
            return std::nullopt;
        }
        if (t >= next->start && t < next->end) {
            mOpen = *next;
            mOpenDate = mHours->DateAt(next->start);
            return mOpen;
        }
        mClosedFrom = t;
        mClosedUntil = next->start;
        return std::nullopt;
    }

    bool IsOpen(types::Timestamp t) {
        return AlwaysOpen() || SessionContaining(t).has_value();
    }

    std::chrono::year_month_day DateAt(types::Timestamp t) {
        if (AlwaysOpen()) {
            return std::chrono::year_month_day{
                std::chrono::floor<std::chrono::days>(t)};
        }
        if (mOpen && t >= mOpen->start && t < mOpen->end) {
            return mOpenDate;
        }
        return mHours->DateAt(t);
    }

   private:
    const types::MarketHours* mHours = nullptr;
    std::optional<types::MarketHours::Session> mOpen;
    std::chrono::year_month_day mOpenDate{};
    types::Timestamp mClosedFrom{};
    std::optional<types::Timestamp> mClosedUntil;
};

struct RollupBucket {
    types::Timestamp open;
    types::Timestamp end;
};

RollupBucket BucketOf(
    types::Timestamp bar_open, std::chrono::seconds length,
    const std::optional<types::MarketHours::Session>& session) {
    const auto len_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(length);
    if (session) {
        const auto offset = bar_open - session->start;
        const auto open = session->start + (offset / len_ms) * len_ms;
        return RollupBucket{open, std::min(open + len_ms, session->end)};
    }
    const std::int64_t ms = types::ToEpochMillis(bar_open);
    const std::int64_t step = len_ms.count();
    std::int64_t q = ms / step;
    if (ms % step < 0) {
        --q;
    }
    const types::Timestamp open = types::FromEpochMillis(q * step);
    return RollupBucket{open, open + len_ms};
}

bool CanEmitRollup(const RollupBucket& bucket,
                   const DatabentoParseOptions& options) {
    if (bucket.end > options.now) {
        return false;
    }
    if (options.from && bucket.open < *options.from) {
        return false;
    }
    if (options.to && bucket.end > *options.to) {
        return false;
    }
    return true;
}

types::Bar FoldBars(types::Timestamp open_time,
                    std::span<const types::Bar> bars) {
    types::Bar out = bars.front();
    out.open_time = open_time;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        out.high = std::max(out.high, bars[i].high);
        out.low = std::min(out.low, bars[i].low);
        out.close = bars[i].close;
        out.volume += bars[i].volume;
    }
    return out;
}

}  // namespace

std::string_view ToString(DatabentoSchema schema) {
    for (const auto& entry : kSchemas) {
        if (entry.schema == schema) {
            return entry.name;
        }
    }
    return "unknown";
}

Result<DatabentoSchema> ParseDatabentoSchema(std::string_view text) {
    for (const auto& entry : kSchemas) {
        if (entry.name == text) {
            return entry.schema;
        }
    }
    return MakeError(ErrorCategory::kInvalidArgument,
                     "unknown Databento schema '" + std::string(text) +
                         "' (expected ohlcv-1m, ohlcv-1h, ohlcv-1d, bbo-1s, "
                         "bbo-1m, tbbo, mbp-1 or trades)");
}

std::optional<types::Timeframe> BarTimeframeOf(DatabentoSchema schema) {
    switch (schema) {
        case DatabentoSchema::kOhlcv1m:
            return types::Timeframe::kM1;
        case DatabentoSchema::kOhlcv1h:
            return types::Timeframe::kH1;
        case DatabentoSchema::kOhlcv1d:
            return types::Timeframe::kD1;
        default:
            return std::nullopt;
    }
}

bool IsQuoteSchema(DatabentoSchema schema) {
    return schema == DatabentoSchema::kBbo1s ||
           schema == DatabentoSchema::kBbo1m ||
           schema == DatabentoSchema::kTbbo || schema == DatabentoSchema::kMbp1;
}

bool IsTradeSchema(DatabentoSchema schema) {
    return schema == DatabentoSchema::kTrades;
}

Result<std::vector<types::Timeframe>> ValidateDatabentoRollups(
    DatabentoSchema schema, std::span<const types::Timeframe> rollups) {
    if (rollups.empty()) {
        return std::vector<types::Timeframe>{};
    }
    if (schema != DatabentoSchema::kOhlcv1m) {
        return MakeError(
            ErrorCategory::kInvalidArgument,
            "--rollup requires schema ohlcv-1m (ohlcv-1h bars are UTC "
            "clock-aligned rather than 09:30 session-aligned)");
    }
    const auto base_len = *types::FixedDuration(types::Timeframe::kM1);
    std::vector<types::Timeframe> sorted(rollups.begin(), rollups.end());
    for (const types::Timeframe tf : sorted) {
        const auto len = types::FixedDuration(tf);
        if (!len) {
            return MakeError(ErrorCategory::kInvalidArgument,
                             "rollup timeframe " +
                                 std::string(types::ToString(tf)) +
                                 " has no fixed length");
        }
        if (*len <= base_len || *len >= std::chrono::days(1) ||
            *len % base_len != std::chrono::seconds::zero()) {
            return MakeError(ErrorCategory::kInvalidArgument,
                             "rollup timeframe " +
                                 std::string(types::ToString(tf)) +
                                 " must be under 1d and a strictly larger "
                                 "multiple of 1m");
        }
    }
    std::ranges::sort(sorted, [](types::Timeframe a, types::Timeframe b) {
        return *types::FixedDuration(a) < *types::FixedDuration(b);
    });
    if (std::ranges::adjacent_find(sorted) != sorted.end()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "rollup timeframes must not repeat");
    }
    return sorted;
}

std::vector<std::string> DatabentoLedgerTargets(
    DatabentoSchema schema, std::span<const types::Timeframe> rollups) {
    std::vector<std::string> targets;
    targets.reserve(1 + rollups.size());
    targets.emplace_back(ToString(schema));
    for (const types::Timeframe tf : rollups) {
        targets.push_back("rollup:" + std::string(types::ToString(tf)));
    }
    return targets;
}

Result<std::string> ToDatabentoSymbol(std::string_view symbol) {
    auto normalized = types::NormalizeSymbol(symbol);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }
    auto parts = types::SplitSymbol(*normalized);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    if (parts->quote) {
        return MakeError(
            ErrorCategory::kUnsupported,
            "Databento US equities do not trade pairs: " + *normalized);
    }
    return *normalized;
}

Result<DatabentoParseStats> ParseDatabentoCsv(
    std::istream& input, const DatabentoParseOptions& options,
    const DatabentoCallbacks& callbacks) {
    auto rollups = ValidateDatabentoRollups(options.schema, options.rollups);
    if (!rollups) {
        return std::unexpected(rollups.error());
    }
    DatabentoParseStats stats;
    std::string line;
    std::size_t line_no = 0;

    bool found_header = false;
    while (std::getline(input, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!Trim(line).empty()) {
            found_header = true;
            break;
        }
    }
    if (!found_header) {
        return stats;
    }

    std::vector<std::string> scratch;
    std::vector<std::string_view> fields;
    if (auto ok = SplitCsvLine(line, scratch, fields, line_no); !ok) {
        return std::unexpected(ok.error());
    }

    std::vector<std::string> header_names;
    header_names.reserve(fields.size());
    std::map<std::string, std::size_t, std::less<>> columns;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        std::string name(Trim(fields[i]));
        columns.try_emplace(name, i);
        header_names.push_back(std::move(name));
    }
    const auto col = [&](std::string_view name) -> std::optional<std::size_t> {
        const auto it = columns.find(name);
        return it == columns.end() ? std::nullopt
                                   : std::optional<std::size_t>(it->second);
    };
    const auto require_col = [&](std::string_view name) -> Result<std::size_t> {
        if (const auto idx = col(name)) {
            return *idx;
        }
        return MakeError(ErrorCategory::kParse,
                         "databento CSV header is missing required column '" +
                             std::string(name) + "'");
    };

    const auto sym_col = col("symbol");
    const auto pub_col = col("publisher_id");
    if (!sym_col && options.symbols.size() != 1) {
        return MakeError(ErrorCategory::kParse,
                         "databento CSV has no 'symbol' column; configure a "
                         "single symbol or export with map_symbols=true");
    }

    const auto base_tf = BarTimeframeOf(options.schema);
    std::size_t c_ts_event = 0, c_open = 0, c_high = 0, c_low = 0, c_close = 0,
                c_volume = 0;
    std::optional<std::size_t> c_ts_recv, c_opt_ts_event, c_price, c_size,
        c_side, c_action;
    std::size_t c_bid_px = 0, c_ask_px = 0, c_bid_sz = 0, c_ask_sz = 0;

    if (base_tf) {
        auto te = require_col("ts_event");
        auto o = require_col("open");
        auto h = require_col("high");
        auto l = require_col("low");
        auto c = require_col("close");
        auto v = require_col("volume");
        for (const auto* r : {&te, &o, &h, &l, &c, &v}) {
            if (!*r) {
                return std::unexpected(r->error());
            }
        }
        c_ts_event = *te;
        c_open = *o;
        c_high = *h;
        c_low = *l;
        c_close = *c;
        c_volume = *v;
    } else if (IsQuoteSchema(options.schema)) {
        c_ts_recv = col("ts_recv");
        c_opt_ts_event = col("ts_event");
        if (!c_ts_recv && !c_opt_ts_event) {
            return MakeError(
                ErrorCategory::kParse,
                "databento quote CSV needs 'ts_recv' or 'ts_event'");
        }
        auto bp = require_col("bid_px_00");
        auto ap = require_col("ask_px_00");
        auto bs = require_col("bid_sz_00");
        auto as = require_col("ask_sz_00");
        for (const auto* r : {&bp, &ap, &bs, &as}) {
            if (!*r) {
                return std::unexpected(r->error());
            }
        }
        c_bid_px = *bp;
        c_ask_px = *ap;
        c_bid_sz = *bs;
        c_ask_sz = *as;
        c_price = col("price");
        c_size = col("size");
        c_side = col("side");
        c_action = col("action");
        if (options.schema == DatabentoSchema::kTbbo &&
            (!c_price || !c_size || !c_side)) {
            return MakeError(ErrorCategory::kParse,
                             "databento tbbo CSV needs 'price', 'size' and "
                             "'side' columns");
        }
        if (options.schema == DatabentoSchema::kMbp1 && !c_action) {
            return MakeError(ErrorCategory::kParse,
                             "databento mbp-1 CSV needs an 'action' column");
        }
    } else {
        c_ts_recv = col("ts_recv");
        c_opt_ts_event = col("ts_event");
        if (!c_ts_recv && !c_opt_ts_event) {
            return MakeError(
                ErrorCategory::kParse,
                "databento trades CSV needs 'ts_event' or 'ts_recv'");
        }
        auto p = require_col("price");
        auto s = require_col("size");
        auto sd = require_col("side");
        for (const auto* r : {&p, &s, &sd}) {
            if (!*r) {
                return std::unexpected(r->error());
            }
        }
        c_price = *p;
        c_size = *s;
        c_side = *sd;
        c_action = col("action");
    }

    const std::size_t num_cols = header_names.size();
    std::optional<bool> pretty_px = options.pretty_px;

    using BarKey = std::pair<types::Symbol, types::Timeframe>;
    std::map<BarKey, std::vector<types::Bar>> bar_batches;
    struct Accumulator {
        RollupBucket bucket;
        std::vector<types::Bar> bars;
    };
    std::map<BarKey, Accumulator> accumulators;
    std::map<types::Symbol, types::Timestamp> last_bar_open;
    std::map<types::Symbol, std::string> symbol_publisher;

    std::vector<types::Quote> quote_batch;
    std::map<types::Symbol, std::vector<types::MarketTrade>> trade_batches;
    std::map<types::Symbol, IdState> ids;

    struct LastTradeState {
        std::chrono::year_month_day date{};
        std::optional<Decimal> price;
        std::optional<Decimal> size;
        std::optional<types::Side> side;
    };
    std::map<types::Symbol, LastTradeState> mbp_last_trade;

    struct TickRuleState {
        std::optional<Decimal> last_price;
        types::Side last_side = types::Side::kBuy;
    };
    std::map<types::Symbol, TickRuleState> tick_rule;

    const auto flush_bars = [&](const BarKey& key) -> Result<void> {
        auto& batch = bar_batches[key];
        if (batch.empty()) {
            return {};
        }
        if (callbacks.on_bars) {
            if (auto ok = callbacks.on_bars(key.first, key.second, batch);
                !ok) {
                return ok;
            }
        }
        batch.clear();
        return {};
    };
    // Deduplicates `open_time` inside `batch` in O(1) for chronological streams
    // (reverse-scanning only when `open_time` steps back) so PostgreSQL's
    // multi-row `ON CONFLICT DO UPDATE` never sees two rows for the same key.
    const auto push_bar = [&](const types::Symbol& sym, types::Timeframe tf,
                              const types::Bar& bar,
                              bool is_rollup) -> Result<void> {
        const BarKey key{sym, tf};
        auto& batch = bar_batches[key];
        if (!batch.empty()) {
            if (bar.open_time == batch.back().open_time) {
                batch.back() = bar;
                return {};
            }
            if (bar.open_time < batch.back().open_time) {
                for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
                    if (it->open_time == bar.open_time) {
                        *it = bar;
                        return {};
                    }
                }
            }
        }
        batch.push_back(bar);
        if (is_rollup) {
            ++stats.rollup_bars;
        } else {
            ++stats.bars;
        }
        if (batch.size() >= kBatchSize) {
            return flush_bars(key);
        }
        return {};
    };
    const auto flush_quotes = [&]() -> Result<void> {
        if (quote_batch.empty()) {
            return {};
        }
        if (callbacks.on_quotes) {
            if (auto ok = callbacks.on_quotes(quote_batch); !ok) {
                return ok;
            }
        }
        quote_batch.clear();
        return {};
    };
    const auto flush_trades = [&](const types::Symbol& sym) -> Result<void> {
        auto& batch = trade_batches[sym];
        if (batch.empty()) {
            return {};
        }
        if (callbacks.on_trades) {
            if (auto ok = callbacks.on_trades(sym, batch); !ok) {
                return ok;
            }
        }
        batch.clear();
        return {};
    };

    const auto* hours = options.hours.get();
    SessionCache sessions(hours);
    const auto base_len = base_tf ? *types::FixedDuration(*base_tf)
                                  : std::chrono::seconds::zero();

    const auto is_repeated_header = [&]() {
        if (fields.size() != num_cols) {
            return false;
        }
        for (std::size_t i = 0; i < num_cols; ++i) {
            if (Trim(fields[i]) != header_names[i]) {
                return false;
            }
        }
        return true;
    };

    while (std::getline(input, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (Trim(line).empty()) {
            continue;
        }
        if (auto ok = SplitCsvLine(line, scratch, fields, line_no); !ok) {
            return std::unexpected(ok.error());
        }
        if (is_repeated_header()) {
            continue;
        }
        if (fields.size() != num_cols) {
            return MakeError(ErrorCategory::kParse,
                             "databento CSV line " + std::to_string(line_no) +
                                 ": expected " + std::to_string(num_cols) +
                                 " columns, got " +
                                 std::to_string(fields.size()));
        }
        ++stats.rows_read;

        types::Symbol symbol;
        if (sym_col) {
            const std::string_view raw_sym = Trim(fields[*sym_col]);
            if (raw_sym.empty()) {
                return MakeError(ErrorCategory::kParse,
                                 "databento CSV line " +
                                     std::to_string(line_no) +
                                     ": empty symbol");
            }
            auto norm = types::NormalizeSymbol(raw_sym);
            if (!norm) {
                return MakeError(ErrorCategory::kParse,
                                 "databento CSV line " +
                                     std::to_string(line_no) + ": " +
                                     norm.error().message);
            }
            if (!options.symbols.empty() && !options.symbols.contains(*norm)) {
                ++stats.skipped_symbol;
                continue;
            }
            symbol = std::move(*norm);
        } else {
            symbol = *options.symbols.begin();
        }

        if (pub_col) {
            const std::string_view pub = Trim(fields[*pub_col]);
            if (!pub.empty()) {
                auto [it, inserted] = symbol_publisher.try_emplace(symbol, pub);
                if (!inserted && it->second != pub) {
                    return MakeError(
                        ErrorCategory::kParse,
                        "databento CSV line " + std::to_string(line_no) +
                            ": multiple publisher_id values for " + symbol +
                            " (" + it->second + " and " + std::string(pub) +
                            "); use a single-publisher dataset such as "
                            "EQUS.MINI or XNAS.ITCH");
                }
            }
        }

        if (base_tf) {
            auto ts = ParseTime(fields[c_ts_event], line_no);
            if (!ts) {
                return std::unexpected(ts.error());
            }
            if (!*ts) {
                return MakeError(ErrorCategory::kParse,
                                 "databento CSV line " +
                                     std::to_string(line_no) +
                                     ": undefined bar timestamp");
            }
            types::Bar bar;
            bar.open_time =
                options.schema == DatabentoSchema::kOhlcv1d
                    ? std::chrono::floor<std::chrono::days>((*ts)->time)
                    : (*ts)->time;

            std::optional<types::MarketHours::Session> active_session;
            if (options.schema == DatabentoSchema::kOhlcv1d) {
                if (!InWindow(bar.open_time, options) ||
                    bar.open_time + std::chrono::days(1) > options.now ||
                    (!sessions.AlwaysOpen() &&
                     !hours->Regular().AnyOpen(
                         bar.open_time,
                         bar.open_time + std::chrono::days(1)))) {
                    ++stats.skipped_outside;
                    continue;
                }
            } else if (options.schema == DatabentoSchema::kOhlcv1h) {
                const auto bar_end = bar.open_time + std::chrono::hours(1);
                if (bar_end > options.now ||
                    (options.from && bar_end <= *options.from) ||
                    (options.to && bar.open_time >= *options.to) ||
                    (!sessions.AlwaysOpen() &&
                     !hours->AnyOpen(bar.open_time, bar_end))) {
                    ++stats.skipped_outside;
                    continue;
                }
            } else {
                if (!InWindow(bar.open_time, options)) {
                    ++stats.skipped_outside;
                    continue;
                }
                if (!sessions.AlwaysOpen()) {
                    active_session = sessions.SessionContaining(bar.open_time);
                    if (!active_session) {
                        ++stats.skipped_outside;
                        continue;
                    }
                }
                if (BucketOf(bar.open_time, base_len, active_session).end >
                    options.now) {
                    ++stats.skipped_outside;
                    continue;
                }
            }

            auto o = ParsePrice(fields[c_open], pretty_px, line_no);
            auto h = ParsePrice(fields[c_high], pretty_px, line_no);
            auto l = ParsePrice(fields[c_low], pretty_px, line_no);
            auto c = ParsePrice(fields[c_close], pretty_px, line_no);
            auto v = ParseQuantity(fields[c_volume], line_no);
            if (!o) return std::unexpected(o.error());
            if (!h) return std::unexpected(h.error());
            if (!l) return std::unexpected(l.error());
            if (!c) return std::unexpected(c.error());
            if (!v) return std::unexpected(v.error());
            if (!*o || !*h || !*l || !*c || (*o)->Sign() <= 0 ||
                (*h)->Sign() <= 0 || (*l)->Sign() <= 0 || (*c)->Sign() <= 0 ||
                **h < std::max({**o, **l, **c}) ||
                **l > std::min({**o, **h, **c})) {
                return MakeError(ErrorCategory::kParse,
                                 "databento CSV line " +
                                     std::to_string(line_no) +
                                     ": invalid OHLC values");
            }
            bar.open = **o;
            bar.high = **h;
            bar.low = **l;
            bar.close = **c;
            bar.volume = *v;

            if (!rollups->empty()) {
                const auto prev = last_bar_open.find(symbol);
                if (prev != last_bar_open.end() &&
                    bar.open_time < prev->second) {
                    return MakeError(
                        ErrorCategory::kParse,
                        "databento CSV line " + std::to_string(line_no) +
                            ": bars for " + symbol +
                            " are out of chronological order; sort "
                            "oldest-first before --rollup");
                }
            }

            if (auto ok = push_bar(symbol, *base_tf, bar, false); !ok) {
                return std::unexpected(ok.error());
            }

            if (!rollups->empty()) {
                const auto prev = last_bar_open.find(symbol);
                const bool repeated = prev != last_bar_open.end() &&
                                      bar.open_time == prev->second;
                last_bar_open[symbol] = bar.open_time;

                for (const types::Timeframe dst_tf : *rollups) {
                    const auto dst_len = *types::FixedDuration(dst_tf);
                    const RollupBucket bucket =
                        BucketOf(bar.open_time, dst_len, active_session);
                    const BarKey key{symbol, dst_tf};
                    const auto acc_it = accumulators.find(key);
                    if (acc_it == accumulators.end()) {
                        accumulators.emplace(key, Accumulator{bucket, {bar}});
                        continue;
                    }
                    if (bucket.open > acc_it->second.bucket.open) {
                        if (CanEmitRollup(acc_it->second.bucket, options)) {
                            const types::Bar rolled =
                                FoldBars(acc_it->second.bucket.open,
                                         acc_it->second.bars);
                            if (auto ok =
                                    push_bar(symbol, dst_tf, rolled, true);
                                !ok) {
                                return std::unexpected(ok.error());
                            }
                        }
                        acc_it->second = Accumulator{bucket, {bar}};
                    } else if (repeated && !acc_it->second.bars.empty()) {
                        acc_it->second.bars.back() = bar;
                    } else {
                        acc_it->second.bars.push_back(bar);
                    }
                }
            }
        } else if (IsQuoteSchema(options.schema)) {
            std::optional<ParsedTime> recv;
            if (c_ts_recv) {
                auto parsed = ParseTime(fields[*c_ts_recv], line_no);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                recv = *parsed;
            }
            std::optional<ParsedTime> event;
            if (c_opt_ts_event) {
                auto parsed = ParseTime(fields[*c_opt_ts_event], line_no);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                event = *parsed;
            }
            const std::optional<ParsedTime>& primary = recv ? recv : event;
            if (!primary) {
                return MakeError(ErrorCategory::kParse,
                                 "databento CSV line " +
                                     std::to_string(line_no) +
                                     ": undefined quote timestamp");
            }
            const std::int64_t seq = NextId(ids[symbol], primary->nanos);
            if (!InWindow(primary->time, options) ||
                !sessions.IsOpen(primary->time)) {
                ++stats.skipped_outside;
                continue;
            }

            auto bid_px = ParsePrice(fields[c_bid_px], pretty_px, line_no);
            auto ask_px = ParsePrice(fields[c_ask_px], pretty_px, line_no);
            auto bid_sz = ParseQuantity(fields[c_bid_sz], line_no);
            auto ask_sz = ParseQuantity(fields[c_ask_sz], line_no);
            if (!bid_px) return std::unexpected(bid_px.error());
            if (!ask_px) return std::unexpected(ask_px.error());
            if (!bid_sz) return std::unexpected(bid_sz.error());
            if (!ask_sz) return std::unexpected(ask_sz.error());

            types::Quote q;
            q.venue = options.venue;
            q.symbol = symbol;
            q.seq = seq;
            q.received_time = primary->time;
            if (event) {
                q.exchange_time = event->time;
            }
            if (*bid_px && (*bid_px)->Sign() > 0) {
                q.bid = **bid_px;
                q.bid_size = *bid_sz;
            }
            if (*ask_px && (*ask_px)->Sign() > 0) {
                q.ask = **ask_px;
                q.ask_size = *ask_sz;
            }

            const std::string_view action =
                c_action ? Trim(fields[*c_action]) : std::string_view{};
            const bool is_mbp = options.schema == DatabentoSchema::kMbp1;
            const bool has_trade_fields =
                c_price.has_value() && (!is_mbp || action == "T");

            if (is_mbp) {
                auto& lt = mbp_last_trade[symbol];
                const auto date = sessions.DateAt(primary->time);
                if (lt.date != date || action == "R") {
                    lt = LastTradeState{date, {}, {}, {}};
                }
            }

            if (has_trade_fields) {
                auto px = ParsePrice(fields[*c_price], pretty_px, line_no);
                if (!px) {
                    return std::unexpected(px.error());
                }
                if (*px && (*px)->Sign() > 0) {
                    q.last = **px;
                    if (c_size) {
                        auto sz = ParseQuantity(fields[*c_size], line_no);
                        if (!sz) {
                            return std::unexpected(sz.error());
                        }
                        q.last_size = *sz;
                    }
                    if (c_side) {
                        const std::string_view sd = Trim(fields[*c_side]);
                        if (sd == "B") {
                            q.last_taker_side = types::Side::kBuy;
                        } else if (sd == "A") {
                            q.last_taker_side = types::Side::kSell;
                        } else if (!sd.empty() && sd != "N") {
                            return MakeError(ErrorCategory::kParse,
                                             "databento CSV line " +
                                                 std::to_string(line_no) +
                                                 ": unknown side '" +
                                                 std::string(sd) + "'");
                        }
                    }
                    if (is_mbp) {
                        auto& lt = mbp_last_trade[symbol];
                        lt.price = q.last;
                        lt.size = q.last_size;
                        lt.side = q.last_taker_side;
                    }
                }
            } else if (is_mbp) {
                const auto& lt = mbp_last_trade[symbol];
                q.last = lt.price;
                q.last_size = lt.size;
                q.last_taker_side = lt.side;
            }

            quote_batch.push_back(std::move(q));
            ++stats.quotes;
            if (quote_batch.size() >= kBatchSize) {
                if (auto ok = flush_quotes(); !ok) {
                    return std::unexpected(ok.error());
                }
            }
        } else {
            if (c_action) {
                const std::string_view act = Trim(fields[*c_action]);
                if (!act.empty() && act != "T") {
                    continue;
                }
            }
            std::optional<ParsedTime> event;
            if (c_opt_ts_event) {
                auto parsed = ParseTime(fields[*c_opt_ts_event], line_no);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                event = *parsed;
            }
            std::optional<ParsedTime> recv;
            if (c_ts_recv) {
                auto parsed = ParseTime(fields[*c_ts_recv], line_no);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                recv = *parsed;
            }
            const std::optional<ParsedTime>& primary = event ? event : recv;
            if (!primary) {
                return MakeError(ErrorCategory::kParse,
                                 "databento CSV line " +
                                     std::to_string(line_no) +
                                     ": undefined trade timestamp");
            }
            // The ID and the window follow ts_recv, the order the records come
            // in and the time a request's range selects on (ts_event can step
            // back between records, and would repeat an ID or, near a slice's
            // edge, fall outside both slices); the session is the trade's own.
            const std::optional<ParsedTime>& key = recv ? recv : event;
            const std::int64_t trade_id = NextId(ids[symbol], key->nanos);
            if (!InWindow(key->time, options) ||
                !sessions.IsOpen(primary->time)) {
                ++stats.skipped_outside;
                continue;
            }
            auto px = ParsePrice(fields[*c_price], pretty_px, line_no);
            auto sz = ParseQuantity(fields[*c_size], line_no);
            if (!px) return std::unexpected(px.error());
            if (!sz) return std::unexpected(sz.error());
            if (!*px || (*px)->Sign() <= 0 || sz->Sign() <= 0) {
                return MakeError(ErrorCategory::kParse,
                                 "databento CSV line " +
                                     std::to_string(line_no) +
                                     ": non-positive trade price or size");
            }

            auto& tr = tick_rule[symbol];
            const std::string_view sd = Trim(fields[*c_side]);
            types::Side side = types::Side::kBuy;
            if (sd == "B") {
                side = types::Side::kBuy;
            } else if (sd == "A") {
                side = types::Side::kSell;
            } else if (sd == "N" || sd.empty()) {
                if (tr.last_price) {
                    if (**px > *tr.last_price) {
                        side = types::Side::kBuy;
                    } else if (**px < *tr.last_price) {
                        side = types::Side::kSell;
                    } else {
                        side = tr.last_side;
                    }
                }
            } else {
                return MakeError(
                    ErrorCategory::kParse,
                    "databento CSV line " + std::to_string(line_no) +
                        ": unknown side '" + std::string(sd) + "'");
            }
            tr.last_price = **px;
            tr.last_side = side;

            types::MarketTrade trade;
            trade.trade_id = trade_id;
            trade.time = primary->time;
            trade.price = **px;
            trade.quantity = *sz;
            trade.taker_side = side;

            auto& batch = trade_batches[symbol];
            batch.push_back(trade);
            ++stats.trades;
            if (batch.size() >= kBatchSize) {
                if (auto ok = flush_trades(symbol); !ok) {
                    return std::unexpected(ok.error());
                }
            }
        }
    }

    if (input.bad()) {
        return MakeError(ErrorCategory::kNetwork,
                         "error reading Databento CSV stream");
    }

    for (const auto& [key, acc] : accumulators) {
        if (!acc.bars.empty() && CanEmitRollup(acc.bucket, options)) {
            const types::Bar rolled = FoldBars(acc.bucket.open, acc.bars);
            if (auto ok = push_bar(key.first, key.second, rolled, true); !ok) {
                return std::unexpected(ok.error());
            }
        }
    }
    for (auto& [key, batch] : bar_batches) {
        if (auto ok = flush_bars(key); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (auto ok = flush_quotes(); !ok) {
        return std::unexpected(ok.error());
    }
    for (auto& [sym, batch] : trade_batches) {
        if (auto ok = flush_trades(sym); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return stats;
}

DatabentoClient::DatabentoClient(std::string api_key,
                                 std::shared_ptr<net::HttpClient> http)
    : DatabentoClient(std::move(api_key), std::move(http), Options{}) {}

DatabentoClient::DatabentoClient(std::string api_key,
                                 std::shared_ptr<net::HttpClient> http,
                                 Options options)
    : mAuthHeader("Basic " + util::Base64Encode(api_key + ":")),
      mHttp(std::move(http)),
      mOptions(std::move(options)) {
    while (!mOptions.rest_base_url.empty() &&
           mOptions.rest_base_url.back() == '/') {
        mOptions.rest_base_url.pop_back();
    }
}

namespace {

Result<std::map<std::string, std::string>> BaseForm(
    const DatabentoClient::RangeRequest& request) {
    if (request.dataset.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "Databento dataset must not be empty");
    }
    if (request.symbols.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "Databento request needs at least one symbol");
    }
    if (request.end <= request.start) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "Databento request end must be after start");
    }
    std::string joined;
    for (const auto& sym : request.symbols) {
        auto native = ToDatabentoSymbol(sym);
        if (!native) {
            return std::unexpected(native.error());
        }
        if (!joined.empty()) {
            joined.push_back(',');
        }
        joined += *native;
    }
    return std::map<std::string, std::string>{
        {"dataset", request.dataset},
        {"end", types::FormatIso8601(request.end)},
        {"schema", std::string(ToString(request.schema))},
        {"start", types::FormatIso8601(request.start)},
        {"stype_in", "raw_symbol"},
        {"symbols", std::move(joined)},
    };
}

struct PlannedSlice {
    types::Timestamp query_start;
    types::Timestamp clip_from;
    types::Timestamp end;

    bool operator==(const PlannedSlice&) const = default;
    auto operator<=>(const PlannedSlice&) const = default;
};

std::vector<PlannedSlice> BuildCandidateSlices(const DatabentoHttpOptions& opts,
                                               types::Timestamp effective_to) {
    std::vector<PlannedSlice> out;
    const auto* hours = opts.hours.get();
    if (opts.schema == DatabentoSchema::kOhlcv1d) {
        for (auto day = std::chrono::floor<std::chrono::days>(opts.from);
             day < effective_to; day += std::chrono::days(1)) {
            const types::Timestamp start{day};
            const types::Timestamp day_end = start + std::chrono::days(1);
            if (start < opts.from || day_end > effective_to) {
                continue;
            }
            if (hours && !hours->IsAlwaysOpen() &&
                !hours->Regular().AnyOpen(start, day_end)) {
                continue;
            }
            out.push_back(PlannedSlice{start, start, day_end});
        }
        return out;
    }

    const std::chrono::minutes slice_len = IsTickSchema(opts.schema)
                                               ? TickSliceLength(opts.schema)
                                               : std::chrono::hours(24);
    if (!hours || hours->IsAlwaysOpen()) {
        for (auto t = opts.from; t < effective_to;) {
            const auto next = std::min(t + slice_len, effective_to);
            out.push_back(PlannedSlice{t, t, next});
            t = next;
        }
        return out;
    }

    for (auto session = hours->CurrentOrNext(opts.from);
         session && session->start < effective_to;
         session = hours->CurrentOrNext(session->end)) {
        const auto s_start = std::max(session->start, opts.from);
        const auto s_end = std::min(session->end, effective_to);
        if (s_end <= s_start) {
            continue;
        }
        if (IsTickSchema(opts.schema)) {
            for (auto t = s_start; t < s_end;) {
                const auto next = std::min(t + slice_len, s_end);
                out.push_back(PlannedSlice{t, t, next});
                t = next;
            }
        } else {
            auto q_start = s_start;
            if (opts.schema == DatabentoSchema::kOhlcv1h &&
                s_start == session->start) {
                q_start = std::chrono::floor<std::chrono::hours>(s_start);
            }
            out.push_back(PlannedSlice{q_start, s_start, s_end});
        }
    }
    return out;
}

bool IntervalCovers(std::span<const DatabentoHttpOptions::Interval> coalesced,
                    types::Timestamp start, types::Timestamp end) {
    const auto it = std::ranges::upper_bound(
        coalesced, start, {}, &DatabentoHttpOptions::Interval::first);
    if (it == coalesced.begin()) {
        return false;
    }
    const auto& prev = *(it - 1);
    return prev.first <= start && prev.second >= end;
}

// The part of `span` that its `tf` rollup buckets fill whole. The buckets cut
// by its edges are not emitted (CanEmitRollup), so the ledger must not record
// them as done for that rollup; nullopt when no bucket is whole.
std::optional<DatabentoHttpOptions::Interval> RollupSpan(
    const DatabentoHttpOptions::Interval& span, types::Timeframe tf,
    const types::MarketHours* hours) {
    const auto length = types::FixedDuration(tf);
    if (!length || span.second <= span.first) {
        return std::nullopt;
    }
    std::optional<types::MarketHours::Session> session;
    if (hours && !hours->IsAlwaysOpen()) {
        session = hours->CurrentOrNext(span.first);
        if (session &&
            (span.first < session->start || span.first >= session->end)) {
            session.reset();
        }
    }
    const RollupBucket first = BucketOf(span.first, *length, session);
    const types::Timestamp begin =
        first.open == span.first ? span.first : first.end;
    const RollupBucket last =
        BucketOf(span.second - std::chrono::milliseconds(1), *length, session);
    const types::Timestamp finish =
        last.end <= span.second ? last.end : last.open;
    if (finish <= begin) {
        return std::nullopt;
    }
    return DatabentoHttpOptions::Interval{begin, finish};
}

}  // namespace

Result<net::HttpResponse> DatabentoClient::GetQuery(
    std::string_view endpoint,
    const std::map<std::string, std::string>& query) {
    net::HttpRequest request;
    request.method = net::HttpMethod::kGet;
    request.url = mOptions.rest_base_url + std::string(endpoint);
    if (!query.empty()) {
        request.url += "?" + util::FormEncode(query);
    }
    request.headers = {
        {"Authorization", mAuthHeader},
    };
    auto response = util::Retry(
        mOptions.retry, [&] { return mHttp->Send(request); }, mOptions.sleeper);
    if (!response) {
        auto err = response.error();
        err.message = "databento " + std::string(endpoint) + ": " + err.message;
        return std::unexpected(std::move(err));
    }
    return response;
}

Result<net::HttpResponse> DatabentoClient::PostForm(
    std::string_view endpoint, const std::map<std::string, std::string>& form,
    bool resend_uncertain) {
    net::HttpRequest request;
    request.method = net::HttpMethod::kPost;
    request.url = mOptions.rest_base_url + std::string(endpoint);
    request.headers = {
        {"Authorization", mAuthHeader},
        {"Content-Type", "application/x-www-form-urlencoded"},
    };
    request.body = util::FormEncode(form);
    // A timeout or a connection lost mid-way may have delivered (and billed)
    // the data: unless resend_uncertain, such an error is returned, not sent
    // again. A rate limit or a server error delivered nothing and is retried.
    std::optional<types::Error> uncertain;
    auto response = util::Retry(
        mOptions.retry,
        [&]() -> Result<net::HttpResponse> {
            auto sent = mHttp->Send(request);
            if (!sent && !resend_uncertain &&
                (sent.error().category == ErrorCategory::kTimeout ||
                 sent.error().category == ErrorCategory::kNetwork)) {
                uncertain = sent.error();
                return MakeError(ErrorCategory::kInternal, "not resent");
            }
            return sent;
        },
        mOptions.sleeper);
    if (uncertain) {
        uncertain->message +=
            " (not sent again, as the data may have been delivered and billed; "
            "run the import again to resume from the last saved slice)";
        response = std::unexpected(std::move(*uncertain));
    }
    if (!response) {
        auto err = response.error();
        err.message = "databento " + std::string(endpoint) + ": " + err.message;
        if (err.http_status == 422 &&
            err.message.find("available_end") != std::string::npos) {
            err.message +=
                " (Databento's historical feed lags real-time by a few "
                "minutes; pass an earlier --to)";
        }
        return std::unexpected(std::move(err));
    }
    return response;
}

Result<types::Timestamp> DatabentoClient::GetDatasetEnd(
    std::string_view dataset, DatabentoSchema schema) {
    if (dataset.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "Databento dataset must not be empty");
    }
    auto response = GetQuery("/v0/metadata.get_dataset_range",
                             {{"dataset", std::string(dataset)}});
    if (!response) {
        return std::unexpected(response.error());
    }
    auto json = util::ParseJson(response->body);
    if (!json || !json->is_object()) {
        return MakeError(ErrorCategory::kParse,
                         "databento metadata.get_dataset_range: expected JSON "
                         "object");
    }
    const std::string schema_name(ToString(schema));
    if (const auto schemas = json->find("schema");
        schemas != json->end() && schemas->is_object()) {
        if (const auto s_it = schemas->find(schema_name);
            s_it != schemas->end() && s_it->is_object()) {
            if (const auto end_it = s_it->find("end");
                end_it != s_it->end() && end_it->is_string()) {
                return types::ParseIso8601(
                    end_it->get_ref<const std::string&>());
            }
        }
    }
    if (const auto end_it = json->find("end");
        end_it != json->end() && end_it->is_string()) {
        return types::ParseIso8601(end_it->get_ref<const std::string&>());
    }
    return MakeError(ErrorCategory::kParse,
                     "databento metadata.get_dataset_range: missing 'end'");
}

Result<Decimal> DatabentoClient::GetCost(const RangeRequest& request) {
    auto form = BaseForm(request);
    if (!form) {
        return std::unexpected(form.error());
    }
    auto response = PostForm("/v0/metadata.get_cost", *form);
    if (!response) {
        return std::unexpected(response.error());
    }
    auto json = util::ParseJson(response->body);
    if (!json) {
        return MakeError(
            ErrorCategory::kParse,
            "databento metadata.get_cost: " + json.error().message);
    }
    auto cost = util::JsonDecimal(*json);
    if (!cost || cost->IsNegative()) {
        return MakeError(ErrorCategory::kParse,
                         "databento metadata.get_cost: expected a non-negative "
                         "USD amount");
    }
    return cost->Normalized();
}

Result<std::string> DatabentoClient::GetRangeCsv(const RangeRequest& request) {
    auto form = BaseForm(request);
    if (!form) {
        return std::unexpected(form.error());
    }
    (*form)["compression"] = "none";
    (*form)["encoding"] = "csv";
    (*form)["map_symbols"] = "true";
    (*form)["pretty_px"] = "false";
    (*form)["pretty_ts"] = "false";
    auto response = PostForm("/v0/timeseries.get_range", *form,
                             /*resend_uncertain=*/false);
    if (!response) {
        return std::unexpected(response.error());
    }
    return std::move(response->body);
}

Result<DatabentoHttpResult> ImportDatabentoHttp(
    DatabentoClient& client, const DatabentoHttpOptions& opts) {
    if (opts.dataset.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "Databento dataset must not be empty");
    }
    if (opts.symbols.empty()) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "Databento import needs at least one symbol");
    }
    for (const auto& sym : opts.symbols) {
        if (auto native = ToDatabentoSymbol(sym); !native) {
            return std::unexpected(native.error());
        }
    }
    if (opts.to <= opts.from) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "--to must be after --from");
    }
    auto rollups = ValidateDatabentoRollups(opts.schema, opts.rollups);
    if (!rollups) {
        return std::unexpected(rollups.error());
    }
    const std::vector<std::string> targets =
        DatabentoLedgerTargets(opts.schema, *rollups);

    DatabentoHttpResult result;
    types::Timestamp effective_to = std::min(opts.to, opts.now);
    // Databento's historical feed lags real-time by several minutes: when `to`
    // is within the last 24h of `now`, ask `metadata.get_dataset_range` so a
    // near-present query clamps cleanly instead of failing with HTTP 422.
    if (effective_to > opts.now - std::chrono::hours(24)) {
        if (auto avail_end = client.GetDatasetEnd(opts.dataset, opts.schema)) {
            if (*avail_end < effective_to) {
                effective_to = *avail_end;
                result.clamped_to = *avail_end;
            }
        } else {
            spdlog::warn(
                "databento: could not query dataset range ({}); proceeding "
                "with --to {}",
                avail_end.error().ToString(),
                types::FormatIso8601(effective_to));
        }
    }
    if (effective_to <= opts.from) {
        return result;
    }

    const std::vector<PlannedSlice> candidates =
        BuildCandidateSlices(opts, effective_to);
    if (candidates.empty()) {
        return result;
    }

    using Interval = DatabentoHttpOptions::Interval;
    struct FetchBatch {
        std::vector<PlannedSlice> slices;
        types::Timestamp query_start;
        types::Timestamp clip_from;
        types::Timestamp end;
    };
    std::map<types::Symbol, std::vector<FetchBatch>> batches_by_symbol;

    struct SymbolCostPlan {
        std::vector<Interval> exact_spans;
        std::vector<Interval> coalesced_spans;
        bool operator==(const SymbolCostPlan&) const = default;
        auto operator<=>(const SymbolCostPlan&) const = default;
    };
    std::map<types::Symbol, SymbolCostPlan> cost_plans;
    // Slices to roll up from stored ohlcv-1m bars instead of downloading.
    std::map<types::Symbol, std::vector<PlannedSlice>> stored_by_symbol;

    const std::size_t max_slices_per_batch =
        opts.schema == DatabentoSchema::kOhlcv1d ? kMaxDailySlicesPerRequest
                                                 : 1u;

    for (const auto& symbol : opts.symbols) {
        std::vector<std::vector<Interval>> covered_by_target;
        if (!opts.force && opts.load_imported) {
            covered_by_target.reserve(targets.size());
            for (const std::string& target : targets) {
                auto covered = opts.load_imported(symbol, target);
                if (!covered) {
                    return std::unexpected(covered.error());
                }
                covered_by_target.push_back(std::move(*covered));
            }
        }

        SymbolCostPlan plan;
        std::optional<Interval> active_run;
        std::vector<FetchBatch>& sym_batches = batches_by_symbol[symbol];
        bool prev_was_stored = false;
        // The ledger's first target is the schema's own (ohlcv-1m); the rest
        // are its rollups.
        const bool can_roll_stored = !rollups->empty() &&
                                     opts.load_stored_bars &&
                                     !covered_by_target.empty();

        for (const PlannedSlice& slice : candidates) {
            const Interval ledger_span{
                slice.query_start,
                opts.schema == DatabentoSchema::kOhlcv1d
                    ? slice.query_start + std::chrono::days(1)
                    : slice.end};
            const bool stored =
                !covered_by_target.empty() &&
                ledger_span.second <= effective_to &&
                std::ranges::all_of(
                    covered_by_target, [&](const std::vector<Interval>& cov) {
                        return IntervalCovers(cov, ledger_span.first,
                                              ledger_span.second);
                    });
            const bool from_stored =
                !stored && can_roll_stored &&
                ledger_span.second <= effective_to &&
                IntervalCovers(covered_by_target.front(), ledger_span.first,
                               ledger_span.second);
            if (stored || from_stored) {
                if (stored) {
                    ++result.slices_skipped;
                } else {
                    ++result.slices_from_stored;
                    stored_by_symbol[symbol].push_back(slice);
                }
                prev_was_stored = true;
                if (active_run) {
                    plan.coalesced_spans.push_back(*active_run);
                    active_run.reset();
                }
                continue;
            }

            ++result.slices_planned;
            if (!sym_batches.empty() && !prev_was_stored &&
                sym_batches.back().slices.size() < max_slices_per_batch) {
                sym_batches.back().slices.push_back(slice);
                sym_batches.back().end = slice.end;
            } else {
                sym_batches.push_back(FetchBatch{
                    {slice}, slice.query_start, slice.clip_from, slice.end});
            }
            prev_was_stored = false;

            if (!plan.exact_spans.empty() &&
                plan.exact_spans.back().second >= slice.query_start) {
                plan.exact_spans.back().second =
                    std::max(plan.exact_spans.back().second, slice.end);
            } else {
                plan.exact_spans.emplace_back(slice.query_start, slice.end);
            }

            if (!active_run) {
                active_run = Interval{slice.query_start, slice.end};
            } else {
                active_run->second = slice.end;
            }
        }
        if (active_run) {
            plan.coalesced_spans.push_back(*active_run);
        }
        if (!plan.exact_spans.empty()) {
            cost_plans.emplace(symbol, std::move(plan));
        }
    }

    if (result.slices_planned == 0 && result.slices_from_stored == 0) {
        return result;
    }

    std::map<SymbolCostPlan, std::vector<types::Symbol>> groups;
    for (const auto& sym : opts.symbols) {
        if (const auto it = cost_plans.find(sym); it != cost_plans.end()) {
            groups[it->second].push_back(sym);
        }
    }

    std::size_t total_exact_calls = 0;
    for (const auto& [plan, syms] : groups) {
        total_exact_calls += plan.exact_spans.size();
    }
    constexpr std::size_t kMaxExactCostCalls = 10;
    const bool use_coalesced = total_exact_calls > kMaxExactCostCalls;

    for (const auto& [plan, syms] : groups) {
        const auto& spans =
            (use_coalesced || opts.schema == DatabentoSchema::kOhlcv1d)
                ? plan.coalesced_spans
                : plan.exact_spans;
        if (use_coalesced && spans.size() < plan.exact_spans.size() &&
            opts.hours && !opts.hours->IsAlwaysOpen() &&
            opts.schema != DatabentoSchema::kOhlcv1d) {
            result.cost_is_upper_bound = true;
        }
        for (const auto& [s_start, s_end] : spans) {
            DatabentoClient::RangeRequest req;
            req.dataset = opts.dataset;
            req.schema = opts.schema;
            req.symbols = syms;
            req.start = s_start;
            req.end = s_end;
            auto cost = client.GetCost(req);
            if (!cost) {
                return std::unexpected(cost.error());
            }
            result.estimated_cost += *cost;
        }
    }

    if (opts.dry_run) {
        return result;
    }
    if (opts.max_cost && result.estimated_cost > *opts.max_cost) {
        result.cost_exceeded = true;
        return result;
    }

    // Records finished spans in the ledger: whole for the schema's own target,
    // and for each rollup only the part its emitted buckets cover.
    const auto record = [&](const types::Symbol& symbol,
                            std::span<const Interval> spans) -> Result<void> {
        if (!opts.record_imported || spans.empty()) {
            return {};
        }
        if (auto ok = opts.record_imported(
                symbol, std::span<const std::string>(&targets.front(), 1),
                spans);
            !ok) {
            return ok;
        }
        for (std::size_t i = 0; i < rollups->size(); ++i) {
            std::vector<Interval> clipped;
            for (const Interval& span : spans) {
                if (auto part =
                        RollupSpan(span, (*rollups)[i], opts.hours.get())) {
                    clipped.push_back(*part);
                }
            }
            if (clipped.empty()) {
                continue;
            }
            if (auto ok = opts.record_imported(
                    symbol, std::span<const std::string>(&targets[i + 1], 1),
                    clipped);
                !ok) {
                return ok;
            }
        }
        return {};
    };

    const auto add_stats = [&](const DatabentoParseStats& stats) {
        result.parsed.rows_read += stats.rows_read;
        result.parsed.bars += stats.bars;
        result.parsed.rollup_bars += stats.rollup_bars;
        result.parsed.quotes += stats.quotes;
        result.parsed.trades += stats.trades;
        result.parsed.skipped_symbol += stats.skipped_symbol;
        result.parsed.skipped_outside += stats.skipped_outside;
    };

    // Rollups of stored ohlcv-1m bars: the stored bars go through the parser
    // as a decimal CSV, and only the rollups reach the sinks (the 1m bars are
    // already stored).
    DatabentoCallbacks rollup_sinks = opts.sinks;
    if (opts.sinks.on_bars) {
        rollup_sinks.on_bars =
            [&](const types::Symbol& sym, types::Timeframe tf,
                std::span<const types::Bar> bars) -> Result<void> {
            if (tf == types::Timeframe::kM1) {
                return {};
            }
            return opts.sinks.on_bars(sym, tf, bars);
        };
    }
    for (const auto& [symbol, slices] : stored_by_symbol) {
        for (const PlannedSlice& slice : slices) {
            auto bars =
                opts.load_stored_bars(symbol, slice.clip_from, slice.end);
            if (!bars) {
                return std::unexpected(bars.error());
            }
            std::string csv = "ts_event,open,high,low,close,volume,symbol\n";
            for (const types::Bar& bar : *bars) {
                csv += std::to_string(types::ToEpochMillis(bar.open_time) *
                                      1'000'000LL) +
                       ',' + bar.open.ToString() + ',' + bar.high.ToString() +
                       ',' + bar.low.ToString() + ',' + bar.close.ToString() +
                       ',' + bar.volume.ToString() + ',' + symbol + '\n';
            }
            DatabentoParseOptions parse_opts;
            parse_opts.schema = DatabentoSchema::kOhlcv1m;
            parse_opts.venue = opts.venue;
            parse_opts.symbols = {symbol};
            parse_opts.rollups = *rollups;
            parse_opts.hours = opts.hours;
            parse_opts.now = effective_to;
            parse_opts.from = slice.clip_from;
            parse_opts.to = slice.end;
            parse_opts.pretty_px = true;
            std::istringstream stream(std::move(csv));
            auto stats = ParseDatabentoCsv(stream, parse_opts, rollup_sinks);
            if (!stats) {
                return std::unexpected(stats.error());
            }
            const Interval span{slice.query_start, slice.end};
            if (auto ok = record(symbol, std::span<const Interval>(&span, 1));
                !ok) {
                return std::unexpected(ok.error());
            }
            DatabentoParseStats rolled = *stats;
            rolled.bars = 0;  // already stored; not imported again
            add_stats(rolled);
            if (opts.on_slice) {
                opts.on_slice(symbol, slice.clip_from, slice.end, rolled);
            }
        }
    }

    for (const auto& symbol : opts.symbols) {
        const auto it = batches_by_symbol.find(symbol);
        if (it == batches_by_symbol.end()) {
            continue;
        }
        for (const FetchBatch& batch : it->second) {
            DatabentoClient::RangeRequest req;
            req.dataset = opts.dataset;
            req.schema = opts.schema;
            req.symbols = {symbol};
            req.start = batch.query_start;
            req.end = batch.end;
            auto csv = client.GetRangeCsv(req);
            if (!csv) {
                return std::unexpected(csv.error());
            }

            DatabentoParseOptions parse_opts;
            parse_opts.schema = opts.schema;
            parse_opts.venue = opts.venue;
            parse_opts.symbols = {symbol};
            parse_opts.rollups = *rollups;
            parse_opts.hours = opts.hours;
            parse_opts.now = effective_to;
            parse_opts.from = batch.clip_from;
            parse_opts.to = batch.end;
            parse_opts.pretty_px = false;

            std::istringstream stream(std::move(*csv));
            auto stats = ParseDatabentoCsv(stream, parse_opts, opts.sinks);
            if (!stats) {
                return std::unexpected(stats.error());
            }

            if (opts.record_imported) {
                std::vector<Interval> completed;
                completed.reserve(batch.slices.size());
                for (const PlannedSlice& slice : batch.slices) {
                    const Interval span{
                        slice.query_start,
                        opts.schema == DatabentoSchema::kOhlcv1d
                            ? slice.query_start + std::chrono::days(1)
                            : slice.end};
                    if (span.second <= effective_to) {
                        completed.push_back(span);
                    }
                }
                if (auto ok = record(symbol, completed); !ok) {
                    return std::unexpected(ok.error());
                }
            }

            add_stats(*stats);

            if (opts.on_slice) {
                opts.on_slice(symbol, batch.clip_from, batch.end, *stats);
            }
        }
    }

    return result;
}

}  // namespace kestrel::data
