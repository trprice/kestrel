// kestrel_app: small command-line driver that exercises the data and trading
// interfaces without knowing which provider or mode is configured.

#include <spdlog/spdlog.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "kestrel/config/configuration.hpp"
#include "kestrel/factory.hpp"

namespace {

using namespace kestrel;

void Usage() {
  std::cout << "usage: kestrel_app --config <file.toml> [--bars <timeframe> <count>] [--stream <seconds>]\n"
               "                   [--verbose]\n"
               "\n"
               "  --bars 1h 24    print the last 24 completed 1h bars for each configured symbol\n"
               "  --stream 30     print live quotes for 30 seconds (default 10)\n"
               "Always prints symbol info and the venue's balances.\n";
}

std::string Opt(const std::optional<types::Decimal>& d) { return d ? d->ToString() : "-"; }

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::optional<std::pair<types::Timeframe, int>> bars;
  int stream_seconds = 10;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--bars" && i + 2 < argc) {
      auto tf = types::ParseTimeframe(argv[++i]);
      if (!tf) {
        std::cerr << tf.error().ToString() << '\n';
        return 2;
      }
      bars = {*tf, std::stoi(argv[++i])};
    } else if (arg == "--stream" && i + 1 < argc) {
      stream_seconds = std::stoi(argv[++i]);
    } else if (arg == "--verbose") {
      spdlog::set_level(spdlog::level::debug);
    } else {
      Usage();
      return arg == "--help" ? 0 : 2;
    }
  }
  if (config_path.empty()) {
    Usage();
    return 2;
  }

  auto cfg = config::LoadConfiguration(config_path);
  if (!cfg) {
    spdlog::error("config: {}", cfg.error().ToString());
    return 1;
  }
  auto components = CreateComponents(**cfg);
  if (!components) {
    spdlog::error("startup: {}", components.error().ToString());
    return 1;
  }
  auto& [data_source, venue] = *components;
  spdlog::info("provider={} mode={} data={} venue={}", config::ToString((*cfg)->GetProvider()),
               config::ToString((*cfg)->GetMode()), data_source->Name(), venue->Name());

  for (const auto& symbol : (*cfg)->GetSymbols()) {
    if (auto info = data_source->GetSymbolInfo(symbol)) {
      std::cout << symbol << ": tick=" << info->tick_size << " lot=" << info->lot_size
                << " min=" << info->min_order_size << '\n';
    } else {
      spdlog::error("{}: {}", symbol, info.error().ToString());
    }
    if (bars) {
      const auto length = *types::FixedDuration(bars->first);
      const auto end = types::Now();
      const auto start = end - length * bars->second;
      auto result = data_source->GetBars(symbol, bars->first, start, end);
      if (!result) {
        spdlog::error("{}: {}", symbol, result.error().ToString());
        continue;
      }
      for (const auto& b : *result) {
        std::cout << symbol << ' ' << types::FormatIso8601(b.open_time) << " O=" << b.open << " H=" << b.high
                  << " L=" << b.low << " C=" << b.close << " V=" << b.volume << '\n';
      }
    }
  }

  if (auto balances = venue->GetBalances()) {
    for (const auto& b : *balances) {
      std::cout << "balance " << b.asset << ": total=" << b.total << " available=" << b.available
                << " locked=" << b.locked << '\n';
    }
  } else {
    spdlog::warn("balances: {}", balances.error().ToString());
  }

  // Quotes are printed from the stream thread; std::cout writes are serialised
  // here by a lock because several symbols can update at once.
  std::mutex out_mutex;
  auto sub = data_source->StreamQuotes((*cfg)->GetSymbols(), [&](const types::Quote& q) {
    std::lock_guard lock(out_mutex);
    std::cout << types::FormatIso8601(q.received_time) << ' ' << q.symbol << " bid=" << Opt(q.bid)
              << " ask=" << Opt(q.ask) << " last=" << Opt(q.last) << '\n';
  });
  if (!sub) {
    spdlog::error("stream: {}", sub.error().ToString());
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(stream_seconds));
  (*sub)->Stop();
  return 0;
}
