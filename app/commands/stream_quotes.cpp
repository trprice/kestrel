#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>

#include "alerts.hpp"
#include "command.hpp"
#include "common.hpp"
#include "kestrel/db/quote_store.hpp"
#include "kestrel/factory.hpp"

namespace kestrel::app {

namespace {

// "6 h 31 min", "45 min", "30 s".
std::string FormatDuration(int seconds) {
    const int hours = seconds / 3600;
    const int minutes = seconds % 3600 / 60;
    if (hours > 0) {
        return minutes > 0 ? std::format("{} h {} min", hours, minutes)
                           : std::format("{} h", hours);
    }
    return minutes > 0 ? std::format("{} min", minutes)
                       : std::format("{} s", seconds);
}

class StreamQuotesCommand : public Command {
   public:
    StreamQuotesCommand(int seconds, bool store)
        : mSeconds(seconds), mStore(store) {}

    int Run(const config::Configuration& config) override {
        // Ctrl-C and SIGTERM end the stream early but cleanly, with the
        // quotes flushed, and a second one at once (a hung start-up or
        // flush). Blocked before any thread starts (the alerts' worker, the
        // stream's), so every thread inherits the mask.
        BlockStopSignals();
        // Recording (--store) is a job no one watches, so it alerts; a look
        // at the quotes does not. Before the watcher: no thread may log
        // while the alerts' sink is added.
        std::optional<AlertSession> alerts;
        if (mStore) {
            IgnoreSigpipe();
            alerts.emplace(config, "stream");
        }
        StopSignalWatcher stop;
        const auto failed = [&alerts] {
            if (alerts) {
                alerts->Stopped("could not start");
            }
            return 1;
        };
        std::unique_ptr<db::QuoteWriter> quote_writer;
        if (mStore) {
            auto database = OpenDatabase(config);
            if (!database) {
                spdlog::error("database: {}", database.error().ToString());
                return failed();
            }
            quote_writer =
                std::make_unique<db::QuoteWriter>(std::move(*database));
        }

        auto source = CreateDataSource(config);
        if (!source) {
            spdlog::error("startup: {}", source.error().ToString());
            return failed();
        }
        spdlog::info("provider={} data={}",
                     config::ToString(config.GetProvider()), (*source)->Name());
        if (quote_writer) {
            // The quotes carry the data source's name as their venue.
            if (auto primed = quote_writer->Prime(
                    std::string((*source)->Name()), config.GetSymbols());
                !primed) {
                spdlog::error("database: {}", primed.error().ToString());
                return failed();
            }
        }

        if (stop.Stopping()) {
            if (alerts) {
                alerts->Stopped("stopped early, on a signal");
            }
            return 0;
        }

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
            return failed();
        }
        if (alerts && !stop.Stopping()) {
            alerts->Started(std::format(
                "recording quotes for {} symbols for {}",
                config.GetSymbols().size(), FormatDuration(mSeconds)));
        }
        const bool signalled = stop.Wait(std::chrono::steady_clock::now() +
                                         std::chrono::seconds(mSeconds));
        (*sub)->Stop();
        const bool stored = !quote_writer || quote_writer->Close();
        if (alerts) {
            std::string how = signalled ? "stopped early, on a signal" : "done";
            if (!stored) {
                how +=
                    ", but the last quotes could not be stored (see the log)";
            }
            alerts->Stopped(how);
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
