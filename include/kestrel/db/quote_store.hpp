#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "kestrel/data/data_source.hpp"
#include "kestrel/db/database.hpp"
#include "kestrel/types/market.hpp"

namespace kestrel::db {

// Quotes in the `quotes` TimescaleDB hypertable, one row per delivered Quote
// (each already carries the latest known value of every field). Rows are
// keyed by (venue, symbol, seq); QuoteWriter assigns seq, so Save callers that
// bypass it must supply unique values.
class QuoteStore {
   public:
    explicit QuoteStore(std::shared_ptr<Database> database)
        : mDatabase(std::move(database)) {}

    // Returns how many quotes were written.
    types::Result<std::size_t> Save(std::span<const types::Quote> quotes);

    // Highest stored seq for the symbol, 0 if there are no quotes yet.
    types::Result<std::int64_t> LastSeq(const std::string& venue,
                                        const types::Symbol& symbol);

    // start <= received_time < end, in arrival (seq) order.
    types::Result<std::vector<types::Quote>> Load(const std::string& venue,
                                                  const types::Symbol& symbol,
                                                  types::Timestamp start,
                                                  types::Timestamp end);

   private:
    std::shared_ptr<Database> mDatabase;
};

// Decouples the WebSocket thread from the database: Push() only appends to a
// buffer, and a background thread writes it in batches. Quotes arrive far
// faster than one INSERT per quote could keep up with. A failed batch is kept
// and retried, up to kMaxBuffered quotes, after which the oldest are dropped.
class QuoteWriter {
   public:
    static constexpr std::size_t kMaxBatch = 1000;
    static constexpr std::size_t kMaxBuffered = 100'000;

    QuoteWriter(std::shared_ptr<Database> database,
                std::chrono::milliseconds flush_interval =
                    std::chrono::milliseconds(500));
    ~QuoteWriter();  // flushes what is left
    QuoteWriter(const QuoteWriter&) = delete;
    QuoteWriter& operator=(const QuoteWriter&) = delete;

    // Assigns the quote's seq (continuing from the database after a restart),
    // overwriting whatever the caller set.
    void Push(const types::Quote& quote);
    // Callback for DataSource::StreamQuotes.
    data::QuoteCallback MakeStreamSink();

    // Writes everything buffered now, on the calling thread.
    void Flush();

   private:
    void Run();

    QuoteStore mStore;
    std::chrono::milliseconds mInterval;
    std::mutex mMutex;
    std::condition_variable mWake;
    std::mutex mSeqMutex;
    std::map<std::pair<std::string, types::Symbol>, std::int64_t> mLastSeq;
    std::vector<types::Quote> mBuffer;
    bool mStopping = false;
    std::mutex mFlushMutex;  // one writer at a time keeps batches in order
    std::thread mThread;
};

}  // namespace kestrel::db
