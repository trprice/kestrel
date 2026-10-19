#pragma once

#include <memory>
#include <span>
#include <vector>

#include "kestrel/data/data_source.hpp"
#include "kestrel/db/database.hpp"
#include "kestrel/types/market.hpp"

namespace kestrel::db {

// OHLCV bars in the `bars` TimescaleDB hypertable, keyed by
// (venue, symbol, timeframe, open_time). Writing a bar that already exists
// replaces it, so re-pulling a range or replaying a stream is harmless.
class BarStore {
   public:
    explicit BarStore(std::shared_ptr<Database> database)
        : mDatabase(std::move(database)) {}

    // Returns how many bars were written.
    types::Result<std::size_t> Save(const std::string& venue,
                                    const types::Symbol& symbol,
                                    types::Timeframe timeframe,
                                    std::span<const types::Bar> bars);

    // start <= open_time < end, oldest first.
    types::Result<std::vector<types::Bar>> Load(const std::string& venue,
                                                const types::Symbol& symbol,
                                                types::Timeframe timeframe,
                                                types::Timestamp start,
                                                types::Timestamp end);

    // Callback for DataSource::StreamBars that stores each bar as it closes.
    // Failures are logged; the callback itself never throws.
    data::BarCallback MakeStreamSink(std::string venue);

   private:
    std::shared_ptr<Database> mDatabase;
};

}  // namespace kestrel::db
