#include <spdlog/spdlog.h>

#include <iostream>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/factory.hpp"

namespace kestrel::app {

namespace {

void PrintOrder(const types::OrderStatus& o) {
    std::cout << "order " << o.order_id << " " << o.symbol << " "
              << types::ToString(o.side) << " " << types::ToString(o.type)
              << " " << types::ToString(o.time_in_force)
              << " qty=" << o.quantity << " filled=" << o.filled_quantity;
    if (o.limit_price) {
        std::cout << " limit=" << *o.limit_price;
    }
    if (o.stop_price) {
        std::cout << " stop=" << *o.stop_price;
    }
    if (o.average_fill_price) {
        std::cout << " avg=" << *o.average_fill_price;
    }
    std::cout << " state=" << types::ToString(o.state)
              << " created=" << types::FormatIso8601(o.created_time)
              << " updated=" << types::FormatIso8601(o.updated_time);
    if (!o.client_order_id.empty()) {
        std::cout << " client_id=" << o.client_order_id;
    }
    if (o.reason) {
        std::cout << " reason=\"" << *o.reason << "\"";
    }
    std::cout << '\n';
}

// Reads the account through the configured venue and prints it. Every call is
// a read: nothing is placed, cancelled or changed.
class AccountCommand : public Command {
   public:
    AccountCommand(int days, std::string order_id)
        : mDays(days), mOrderId(std::move(order_id)) {}

    int Run(const config::Configuration& config) override {
        auto components = CreateComponents(config);
        if (!components) {
            spdlog::error("startup: {}", components.error().ToString());
            return 1;
        }
        auto& venue = *components->venue;
        spdlog::info("provider={} mode={} venue={}",
                     config::ToString(config.GetProvider()),
                     config::ToString(config.GetMode()), venue.Name());
        bool ok = true;

        if (auto balances = venue.GetBalances()) {
            for (const auto& b : *balances) {
                std::cout << "balance " << b.asset << ": total=" << b.total
                          << " available=" << b.available
                          << " locked=" << b.locked << '\n';
            }
        } else {
            spdlog::error("balances: {}", balances.error().ToString());
            ok = false;
        }

        if (auto positions = venue.GetPositions()) {
            if (positions->empty()) {
                std::cout << "positions: none reported\n";
            }
            for (const auto& p : *positions) {
                std::cout << "position " << p.symbol << ": qty=" << p.quantity
                          << " avg=" << p.average_price
                          << " realized=" << p.realized_pnl << '\n';
            }
        } else {
            spdlog::error("positions: {}", positions.error().ToString());
            ok = false;
        }

        if (auto open = venue.GetOpenOrders()) {
            std::cout << "open orders: " << open->size() << '\n';
            for (const auto& o : *open) {
                PrintOrder(o);
            }
        } else {
            spdlog::error("open orders: {}", open.error().ToString());
            ok = false;
        }

        const auto since = types::Now() - std::chrono::days(mDays);
        if (auto fills = venue.GetFills(since)) {
            std::cout << "fills since " << types::FormatIso8601(since) << ": "
                      << fills->size() << '\n';
            for (const auto& f : *fills) {
                std::cout << types::FormatIso8601(f.time) << " " << f.symbol
                          << " " << types::ToString(f.side) << " " << f.quantity
                          << " @ " << f.price << " fee=" << f.fee << " "
                          << f.fee_asset << " "
                          << (f.liquidity == types::Liquidity::kMaker ? "maker"
                                                                      : "taker")
                          << " order=" << f.order_id << " fill=" << f.fill_id
                          << '\n';
            }
        } else {
            spdlog::error("fills: {}", fills.error().ToString());
            ok = false;
        }

        if (!mOrderId.empty()) {
            if (auto order = venue.GetOrder(mOrderId)) {
                PrintOrder(*order);
            } else {
                spdlog::error("order {}: {}", mOrderId,
                              order.error().ToString());
                ok = false;
            }
        }
        return ok ? 0 : 1;
    }

   private:
    int mDays;
    std::string mOrderId;
};

struct Args {
    int days = 7;
    std::string order_id;
};

}  // namespace

void AddAccountCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "account",
        "Print the venue's balances, positions, open orders and the fills of "
        "the last --days days for the configured symbols, and one order with "
        "--order. Read-only: places and cancels nothing");
    auto args = std::make_shared<Args>();
    sub->add_option("--days", args->days, "How many days of fills (default 7)")
        ->check(CLI::Range(1, 90));
    sub->add_option("--order", args->order_id,
                    "Also look up this order ID (GetOrder)");
    sub->callback([&slot, args] {
        slot = std::make_unique<AccountCommand>(args->days, args->order_id);
    });
}

}  // namespace kestrel::app
