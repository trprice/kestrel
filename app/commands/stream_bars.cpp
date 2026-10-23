#include <spdlog/spdlog.h>

#include <chrono>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/db/bar_store.hpp"
#include "kestrel/factory.hpp"

namespace kestrel::app {

namespace {

class StreamBarsCommand : public Command {
   public:
    StreamBarsCommand(types::Timeframe timeframe, int seconds, bool store)
        : mTimeframe(timeframe), mSeconds(seconds), mStore(store) {}

    int Run(const config::Configuration& config) override {
        std::unique_ptr<db::BarStore> bar_store;
        if (mStore) {
            auto database = OpenDatabase(config);
            if (!database) {
                spdlog::error("database: {}", database.error().ToString());
                return 1;
            }
            bar_store = std::make_unique<db::BarStore>(std::move(*database));
        }

        auto source = CreateDataSource(config);
        if (!source) {
            spdlog::error("startup: {}", source.error().ToString());
            return 1;
        }
        spdlog::info("provider={} data={}",
                     config::ToString(config.GetProvider()), (*source)->Name());

        std::mutex out_mutex;
        data::BarCallback store_bar =
            bar_store
                ? bar_store->MakeStreamSink(std::string((*source)->Name()))
                : data::BarCallback{};
        auto sub = (*source)->StreamBars(
            config.GetSymbols(), mTimeframe,
            [&](const types::Symbol& symbol, types::Timeframe tf,
                const types::Bar& b) {
                {
                    std::lock_guard lock(out_mutex);
                    std::cout << symbol << ' ' << types::ToString(tf) << ' '
                              << FormatBar(b) << '\n';
                }
                if (store_bar) {
                    store_bar(symbol, tf, b);
                }
            });
        if (!sub) {
            spdlog::error("stream-bars: {}", sub.error().ToString());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(mSeconds));
        (*sub)->Stop();
        return 0;
    }

   private:
    types::Timeframe mTimeframe;
    int mSeconds;
    bool mStore;
};

struct Args {
    std::string timeframe;
    int seconds = 0;
    bool store = false;
};

}  // namespace

void AddStreamBarsCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "stream-bars",
        "Stream completed bars of <timeframe> for <seconds> seconds "
        "(providers that offer it)");
    auto args = std::make_shared<Args>();
    sub->add_option("timeframe", args->timeframe, "Bar length, e.g. 1m")
        ->required()
        ->check(TimeframeValidator(false));
    sub->add_option("seconds", args->seconds, "How long to stream")
        ->required()
        ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    sub->add_flag("--store", args->store,
                  "Also save the bars to the [database] (PostgreSQL + "
                  "TimescaleDB)");
    sub->callback([&slot, args] {
        slot = std::make_unique<StreamBarsCommand>(
            *types::ParseTimeframe(args->timeframe), args->seconds,
            args->store);
    });
}

}  // namespace kestrel::app
