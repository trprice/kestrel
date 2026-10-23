#include <spdlog/spdlog.h>

#include <iostream>
#include <limits>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/db/bar_store.hpp"
#include "kestrel/factory.hpp"

namespace kestrel::app {

namespace {

class BarsCommand : public Command {
   public:
    BarsCommand(types::Timeframe timeframe, int count, bool store)
        : mTimeframe(timeframe), mCount(count), mStore(store) {}

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

        auto components = CreateComponents(config);
        if (!components) {
            spdlog::error("startup: {}", components.error().ToString());
            return 1;
        }
        auto& [data_source, venue] = *components;
        spdlog::info("provider={} mode={} data={} venue={}",
                     config::ToString(config.GetProvider()),
                     config::ToString(config.GetMode()), data_source->Name(),
                     venue->Name());

        for (const auto& symbol : config.GetSymbols()) {
            PrintBars(*data_source, bar_store.get(), symbol);
        }
        if (auto balances = venue->GetBalances()) {
            for (const auto& b : *balances) {
                std::cout << "balance " << b.asset << ": total=" << b.total
                          << " available=" << b.available
                          << " locked=" << b.locked << '\n';
            }
        } else {
            spdlog::warn("balances: {}", balances.error().ToString());
        }
        return 0;
    }

   private:
    void PrintBars(data::DataSource& data_source, db::BarStore* bar_store,
                   const types::Symbol& symbol) const {
        if (auto info = data_source.GetSymbolInfo(symbol)) {
            std::cout << symbol << ": tick=" << info->tick_size
                      << " lot=" << info->lot_size
                      << " min=" << info->min_order_size << '\n';
        } else {
            spdlog::error("{}: {}", symbol, info.error().ToString());
        }
        const auto length = *types::FixedDuration(mTimeframe);
        const auto end = types::Now();
        const auto start = end - length * mCount;
        auto result = data_source.GetBars(symbol, mTimeframe, start, end);
        if (!result) {
            spdlog::error("{}: {}", symbol, result.error().ToString());
            return;
        }
        if (bar_store) {
            auto saved = bar_store->Save(std::string(data_source.Name()),
                                         symbol, mTimeframe, *result);
            if (saved) {
                spdlog::info("{}: stored {} {} bars", symbol, *saved,
                             types::ToString(mTimeframe));
            } else {
                spdlog::error("{}: store: {}", symbol,
                              saved.error().ToString());
            }
        }
        for (const auto& b : *result) {
            std::cout << symbol << ' ' << FormatBar(b) << '\n';
        }
    }

    types::Timeframe mTimeframe;
    int mCount;
    bool mStore;
};

struct Args {
    std::string timeframe;
    int count = 0;
    bool store = false;
};

}  // namespace

void AddBarsCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "bars",
        "Print the last <count> completed bars of <timeframe> (e.g. 1h 24) "
        "for each symbol, with symbol info and balances");
    auto args = std::make_shared<Args>();
    sub->add_option("timeframe", args->timeframe, "Bar length, e.g. 1h")
        ->required()
        ->check(TimeframeValidator(true));
    sub->add_option("count", args->count, "How many bars")
        ->required()
        ->check(CLI::Range(1, std::numeric_limits<int>::max()));
    sub->add_flag("--store", args->store,
                  "Also save the bars to the [database] (PostgreSQL + "
                  "TimescaleDB)");
    sub->callback([&slot, args] {
        slot = std::make_unique<BarsCommand>(
            *types::ParseTimeframe(args->timeframe), args->count, args->store);
    });
}

}  // namespace kestrel::app
