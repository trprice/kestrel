#pragma once

#include <cstddef>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kestrel/net/http_client.hpp"
#include "kestrel/types/decimal.hpp"
#include "kestrel/types/error.hpp"
#include "kestrel/types/market.hpp"
#include "kestrel/types/market_hours.hpp"
#include "kestrel/types/timeframe.hpp"
#include "kestrel/util/retry.hpp"

namespace kestrel::data {

// Databento historical schemas Kestrel imports into `bars`, `quotes`, or
// `market_trades`.
enum class DatabentoSchema {
    kOhlcv1m,
    kOhlcv1h,
    kOhlcv1d,
    kBbo1s,
    kBbo1m,
    kTbbo,
    kMbp1,
    kTrades,
};

std::string_view ToString(DatabentoSchema schema);
types::Result<DatabentoSchema> ParseDatabentoSchema(std::string_view text);

// The bar timeframe for an OHLCV schema; nullopt for quote and trade schemas.
std::optional<types::Timeframe> BarTimeframeOf(DatabentoSchema schema);
bool IsQuoteSchema(DatabentoSchema schema);
bool IsTradeSchema(DatabentoSchema schema);

// Checks `--rollup` targets against `schema`: supported on `ohlcv-1m` only,
// with each target strictly longer than 1m, under 1d, an exact multiple of 1m,
// and without duplicates. Returns the targets sorted shortest first.
types::Result<std::vector<types::Timeframe>> ValidateDatabentoRollups(
    DatabentoSchema schema, std::span<const types::Timeframe> rollups);

// Ledger target keys for `schema` plus validated `rollups` (e.g. "ohlcv-1m",
// "rollup:5m").
std::vector<std::string> DatabentoLedgerTargets(
    DatabentoSchema schema, std::span<const types::Timeframe> rollups);

// Translates a Kestrel US equity symbol ("AAPL", "BRK.B") to Databento's
// `raw_symbol` form, refusing crypto/FX pairs ("BTC/USD").
types::Result<std::string> ToDatabentoSymbol(std::string_view symbol);

// CSV parser options and batch callbacks. `--file` streams line-by-line in
// bounded memory; HTTP pulls buffer one request slice at a time before
// streaming it through the same parser.
struct DatabentoParseOptions {
    DatabentoSchema schema = DatabentoSchema::kOhlcv1m;
    std::string venue = "databento";
    // Symbols to keep (normalized, e.g. "SPY", "BRK.B"). Rows for other
    // symbols in a multi-symbol file are skipped; when the CSV has no `symbol`
    // column, a single-symbol set supplies it.
    std::set<types::Symbol> symbols;
    // Validated intraday rollup timeframes (see ValidateDatabentoRollups).
    std::vector<types::Timeframe> rollups;
    // Session filter: null means always open. Intraday 1m bars, quotes and
    // trades outside open sessions are skipped; 1h bars check session overlap;
    // daily bars check whether their UTC day had a regular session.
    std::shared_ptr<const types::MarketHours> hours;
    // Bars whose period has not finished by `now` are omitted.
    types::Timestamp now;
    // Optional [from, to) clip window. Rollup buckets extending outside
    // [from, to) are dropped rather than emitted as partial bars.
    std::optional<types::Timestamp> from;
    std::optional<types::Timestamp> to;
    // Price encoding: false = int64 scaled by 1e-9 (`pretty_px=false`), true =
    // decimal text (`pretty_px=true`), nullopt = auto-detect from the CSV.
    std::optional<bool> pretty_px;
};

struct DatabentoCallbacks {
    std::function<types::Result<void>(const types::Symbol&, types::Timeframe,
                                      std::span<const types::Bar>)>
        on_bars;
    std::function<types::Result<void>(std::span<const types::Quote>)> on_quotes;
    std::function<types::Result<void>(const types::Symbol&,
                                      std::span<const types::MarketTrade>)>
        on_trades;
};

struct DatabentoParseStats {
    std::size_t rows_read = 0;
    std::size_t bars = 0;
    std::size_t rollup_bars = 0;
    std::size_t quotes = 0;
    std::size_t trades = 0;
    std::size_t skipped_symbol = 0;
    // Rows skipped because they fell outside `hours`, outside `[from, to)`, or
    // in a bar period still forming at `now`.
    std::size_t skipped_outside = 0;
};

// Streams a Databento CSV payload from `input`, flushing records in batches of
// up to 1,000 rows to `callbacks`. Repeated header rows (from concatenated
// daily batch files) are skipped, and an empty stream emits zero records.
types::Result<DatabentoParseStats> ParseDatabentoCsv(
    std::istream& input, const DatabentoParseOptions& options,
    const DatabentoCallbacks& callbacks);

// HTTP Historical API client (`https://hist.databento.com/v0`) built over
// `net::HttpClient`.
class DatabentoClient {
   public:
    struct Options {
        std::string rest_base_url = "https://hist.databento.com";
        util::RetryPolicy retry;
        util::Sleeper sleeper = util::DefaultSleep;
    };

    struct RangeRequest {
        std::string dataset = "EQUS.MINI";
        DatabentoSchema schema = DatabentoSchema::kOhlcv1m;
        std::vector<types::Symbol> symbols;
        types::Timestamp start;
        types::Timestamp end;
    };

    DatabentoClient(std::string api_key, std::shared_ptr<net::HttpClient> http);
    DatabentoClient(std::string api_key, std::shared_ptr<net::HttpClient> http,
                    Options options);

    // GET /v0/metadata.get_dataset_range: exclusive end of available data for
    // `(dataset, schema)`.
    types::Result<types::Timestamp> GetDatasetEnd(std::string_view dataset,
                                                  DatabentoSchema schema);

    // POST /v0/metadata.get_cost: estimated cost in USD for `request`.
    types::Result<types::Decimal> GetCost(const RangeRequest& request);

    // POST /v0/timeseries.get_range with encoding=csv, pretty_px=false,
    // pretty_ts=false, map_symbols=true, compression=none. Retried on a rate
    // limit or a server error, but not on a timeout or a lost connection,
    // which may have delivered (and billed) the data.
    types::Result<std::string> GetRangeCsv(const RangeRequest& request);

   private:
    types::Result<net::HttpResponse> GetQuery(
        std::string_view endpoint,
        const std::map<std::string, std::string>& query);
    types::Result<net::HttpResponse> PostForm(
        std::string_view endpoint,
        const std::map<std::string, std::string>& form,
        bool resend_uncertain = true);

    std::string mAuthHeader;
    std::shared_ptr<net::HttpClient> mHttp;
    Options mOptions;
};

struct DatabentoHttpOptions {
    using Interval = std::pair<types::Timestamp, types::Timestamp>;

    std::string dataset = "EQUS.MINI";
    std::string venue = "databento";
    DatabentoSchema schema = DatabentoSchema::kOhlcv1m;
    std::vector<types::Symbol> symbols;
    std::vector<types::Timeframe> rollups;
    types::Timestamp from;
    types::Timestamp to;
    types::Timestamp now;
    std::shared_ptr<const types::MarketHours> hours;
    bool dry_run = false;
    std::optional<types::Decimal> max_cost;
    bool force = false;
    DatabentoCallbacks sinks;
    // Optional ledger callbacks: `load_imported` returns sorted, coalesced
    // `[start, end)` intervals already completed for `(symbol, target)` in
    // `[from, to)`; `record_imported` marks finished slices after all rows in
    // the request have been flushed to `sinks`.
    std::function<types::Result<std::vector<Interval>>(
        const types::Symbol& symbol, const std::string& target)>
        load_imported;
    std::function<types::Result<void>(const types::Symbol& symbol,
                                      std::span<const std::string> targets,
                                      std::span<const Interval> slices)>
        record_imported;
    // Optional: the stored `ohlcv-1m` bars of a symbol in `[start, end)`.
    // With it, a slice whose `ohlcv-1m` bars the ledger records but whose
    // requested rollups it does not is rolled up from the stored bars rather
    // than downloaded (and paid for) again.
    std::function<types::Result<std::vector<types::Bar>>(
        const types::Symbol& symbol, types::Timestamp start,
        types::Timestamp end)>
        load_stored_bars;
    std::function<void(const types::Symbol&, types::Timestamp, types::Timestamp,
                       const DatabentoParseStats&)>
        on_slice;
};

struct DatabentoHttpResult {
    types::Decimal estimated_cost;
    // True when >10 session spans were coalesced into a broader cost query
    // covering closed hours between sessions.
    bool cost_is_upper_bound = false;
    // Set when `to` was within 24h of `now` and clamped to Databento's
    // `metadata.get_dataset_range` end.
    std::optional<types::Timestamp> clamped_to;
    std::size_t slices_planned = 0;
    std::size_t slices_skipped = 0;
    // Slices rolled up from stored `ohlcv-1m` bars (see `load_stored_bars`),
    // not downloaded and not in the estimated cost.
    std::size_t slices_from_stored = 0;
    bool cost_exceeded = false;
    DatabentoParseStats parsed;
};

// Clamps recent `to` against `metadata.get_dataset_range`, plans session or
// 1-hour tick slices over `[from, to)`, skips slices recorded in the ledger
// (unless `force`), queries `metadata.get_cost`, enforces `max_cost`, and --
// when not `dry_run` -- downloads, streams to `sinks`, and records each
// completed request in the ledger.
types::Result<DatabentoHttpResult> ImportDatabentoHttp(
    DatabentoClient& client, const DatabentoHttpOptions& options);

}  // namespace kestrel::data
