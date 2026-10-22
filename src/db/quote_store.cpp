#include "kestrel/db/quote_store.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace kestrel::db {

using types::Result;

namespace {

constexpr std::size_t kColumns = 11;
// PostgreSQL allows 65535 bind parameters per statement.
constexpr std::size_t kRowsPerInsert = 1000;

std::string InsertSql(std::size_t rows) {
    std::string sql =
        "INSERT INTO quotes (venue, symbol, seq, received_time, exchange_time, "
        "bid, bid_size, ask, ask_size, last, last_size) VALUES ";
    for (std::size_t r = 0; r < rows; ++r) {
        sql += r ? ", (" : "(";
        for (std::size_t c = 0; c < kColumns; ++c) {
            sql += (c ? ", $" : "$") + std::to_string(r * kColumns + c + 1);
        }
        sql += ")";
    }
    return sql;
}

std::optional<std::string> Text(const std::optional<types::Decimal>& d) {
    return d ? std::optional<std::string>(d->ToString()) : std::nullopt;
}

std::optional<types::Decimal> ParseOptional(
    const std::optional<std::string>& text) {
    if (!text) {
        return std::nullopt;
    }
    auto d = types::Decimal::Parse(*text);
    return d ? std::optional<types::Decimal>(*d) : std::nullopt;
}

}  // namespace

Result<std::size_t> QuoteStore::Save(std::span<const types::Quote> quotes) {
    std::size_t written = 0;
    while (written < quotes.size()) {
        const auto batch = quotes.subspan(
            written, std::min(kRowsPerInsert, quotes.size() - written));
        std::vector<std::optional<std::string>> params;
        params.reserve(batch.size() * kColumns);
        for (const auto& q : batch) {
            params.emplace_back(q.venue);
            params.emplace_back(q.symbol);
            params.emplace_back(std::to_string(q.seq));
            params.emplace_back(types::FormatIso8601(q.received_time));
            params.emplace_back(
                q.exchange_time ? std::optional<std::string>(
                                      types::FormatIso8601(*q.exchange_time))
                                : std::nullopt);
            params.emplace_back(Text(q.bid));
            params.emplace_back(Text(q.bid_size));
            params.emplace_back(Text(q.ask));
            params.emplace_back(Text(q.ask_size));
            params.emplace_back(Text(q.last));
            params.emplace_back(Text(q.last_size));
        }
        auto result = mDatabase->Query(InsertSql(batch.size()), params);
        if (!result) {
            return std::unexpected(result.error());
        }
        written += batch.size();
    }
    return written;
}

Result<std::vector<types::Quote>> QuoteStore::Load(const std::string& venue,
                                                   const types::Symbol& symbol,
                                                   types::Timestamp start,
                                                   types::Timestamp end) {
    auto rows = mDatabase->Query(
        "SELECT seq, (extract(epoch FROM received_time) * 1000)::bigint, "
        "(extract(epoch FROM exchange_time) * 1000)::bigint, bid, bid_size, "
        "ask, ask_size, last, last_size FROM quotes WHERE venue = $1 AND "
        "symbol = $2 AND received_time >= $3 AND received_time < $4 "
        "ORDER BY seq",
        {venue, symbol, types::FormatIso8601(start),
         types::FormatIso8601(end)});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<types::Quote> quotes;
    quotes.reserve(rows->size());
    for (const auto& row : *rows) {
        types::Quote q;
        q.venue = venue;
        q.symbol = symbol;
        q.seq = std::stoll(*row[0]);
        q.received_time = types::FromEpochMillis(std::stoll(*row[1]));
        if (row[2]) {
            q.exchange_time = types::FromEpochMillis(std::stoll(*row[2]));
        }
        q.bid = ParseOptional(row[3]);
        q.bid_size = ParseOptional(row[4]);
        q.ask = ParseOptional(row[5]);
        q.ask_size = ParseOptional(row[6]);
        q.last = ParseOptional(row[7]);
        q.last_size = ParseOptional(row[8]);
        quotes.push_back(std::move(q));
    }
    return quotes;
}

Result<std::int64_t> QuoteStore::LastSeq(const std::string& venue,
                                         const types::Symbol& symbol) {
    auto rows = mDatabase->Query(
        "SELECT COALESCE(MAX(seq), 0) FROM quotes WHERE venue = $1 AND "
        "symbol = $2",
        {venue, symbol});
    if (!rows) {
        return std::unexpected(rows.error());
    }
    return std::stoll(*(*rows)[0][0]);
}

QuoteWriter::QuoteWriter(std::shared_ptr<Database> database,
                         std::chrono::milliseconds flush_interval)
    : mStore(std::move(database)),
      mInterval(flush_interval),
      mThread([this] { Run(); }) {}

QuoteWriter::~QuoteWriter() {
    {
        std::lock_guard lock(mMutex);
        mStopping = true;
    }
    mWake.notify_all();
    mThread.join();
    Flush();
}

void QuoteWriter::Push(const types::Quote& quote) {
    // Held across numbering and buffering so seq order is buffer order.
    std::lock_guard seq_lock(mSeqMutex);
    const auto key = std::make_pair(quote.venue, quote.symbol);
    auto it = mLastSeq.find(key);
    if (it == mLastSeq.end()) {
        auto last = mStore.LastSeq(quote.venue, quote.symbol);
        if (!last) {
            spdlog::error("store quote {}: cannot read last seq: {}",
                          quote.symbol, last.error().ToString());
            return;  // dropped; the next quote retries the lookup
        }
        it = mLastSeq.emplace(key, *last).first;
    }
    auto numbered = quote;
    numbered.seq = ++it->second;

    bool full = false;
    {
        std::lock_guard lock(mMutex);
        mBuffer.push_back(std::move(numbered));
        full = mBuffer.size() >= kMaxBatch;
    }
    if (full) {
        mWake.notify_one();
    }
}

data::QuoteCallback QuoteWriter::MakeStreamSink() {
    return [this](const types::Quote& q) {
        try {
            Push(q);
        } catch (const std::exception& e) {
            spdlog::error("store quote {}: {}", q.symbol, e.what());
        }
    };
}

void QuoteWriter::Flush() {
    std::lock_guard flush_lock(mFlushMutex);
    std::vector<types::Quote> batch;
    {
        std::lock_guard lock(mMutex);
        batch.swap(mBuffer);
    }
    if (batch.empty()) {
        return;
    }
    auto saved = mStore.Save(batch);
    if (saved) {
        return;
    }
    spdlog::error("store quotes: {} ({} quotes kept for retry)",
                  saved.error().ToString(), batch.size());
    std::lock_guard lock(mMutex);
    // Failed quotes are older than anything pushed since; put them back first.
    batch.insert(batch.end(), std::make_move_iterator(mBuffer.begin()),
                 std::make_move_iterator(mBuffer.end()));
    if (batch.size() > kMaxBuffered) {
        const auto drop = batch.size() - kMaxBuffered;
        spdlog::error("store quotes: dropping {} oldest quotes", drop);
        batch.erase(batch.begin(),
                    batch.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    mBuffer = std::move(batch);
}

void QuoteWriter::Run() {
    std::unique_lock lock(mMutex);
    while (!mStopping) {
        mWake.wait_for(lock, mInterval, [this] {
            return mStopping || mBuffer.size() >= kMaxBatch;
        });
        if (mStopping) {
            break;
        }
        lock.unlock();
        Flush();
        lock.lock();
    }
}

}  // namespace kestrel::db
