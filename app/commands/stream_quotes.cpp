#include <spdlog/spdlog.h>

#include <chrono>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/db/quote_store.hpp"
#include "kestrel/factory.hpp"

namespace kestrel::app {

namespace {

class StreamQuotesCommand : public Command {
   public:
    StreamQuotesCommand(int seconds, bool store)
        : mSeconds(seconds), mStore(store) {}

    int Run(const config::Configuration& config) override {
        std::unique_ptr<db::QuoteWriter> quote_writer;
        if (mStore) {
            auto database = OpenDatabase(config);
            if (!database) {
                spdlog::error("database: {}", database.error().ToString());
                return 1;
            }
            quote_writer =
                std::make_unique<db::QuoteWriter>(std::move(*database));
        }

        auto source = CreateDataSource(config);
        if (!source) {
            spdlog::error("startup: {}", source.error().ToString());
            return 1;
        }
        spdlog::info("provider={} data={}",
                     config::ToString(config.GetProvider()), (*source)->Name());

        // Quotes arrive on the stream thread, and several symbols can update
        // at once, so output is serialised.
        std::mutex out_mutex;
        auto sub = (*source)->StreamQuotes(
            config.GetSymbols(), [&](const types::Quote& q) {
                std::lock_guard lock(out_mutex);
                std::cout << types::FormatIso8601(q.received_time) << ' '
                          << q.symbol << " bid=" << Opt(q.bid)
                          << " ask=" << Opt(q.ask) << " last=" << Opt(q.last)
                          << '\n';
                if (quote_writer) {
                    quote_writer->Push(q);
                }
            });
        if (!sub) {
            spdlog::error("stream: {}", sub.error().ToString());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(mSeconds));
        (*sub)->Stop();
        if (quote_writer) {
            quote_writer->Flush();
        }
        return 0;
    }

   private:
    int mSeconds;
    bool mStore;
};

struct Args {
    int seconds = 0;
    bool store = false;
};

}  // namespace

void AddStreamCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub =
        AddSubcommand(app, "stream", "Print live quotes for <seconds> seconds");
    auto args = std::make_shared<Args>();
    sub->add_option("seconds", args->seconds, "How long to stream")
        ->required()
        ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    sub->add_flag("--store", args->store,
                  "Also save every quote to the [database] (PostgreSQL + "
                  "TimescaleDB)");
    sub->callback([&slot, args] {
        slot =
            std::make_unique<StreamQuotesCommand>(args->seconds, args->store);
    });
}

}  // namespace kestrel::app
