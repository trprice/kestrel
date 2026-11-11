#include <spdlog/spdlog.h>

#include <iostream>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/factory.hpp"
#include "kestrel/trading/binanceus_trading_venue.hpp"
#include "kestrel/trading/risk_gate.hpp"

namespace kestrel::app {

namespace {

// Passes everything to the Binance.US venue except PlaceOrder, which goes to
// its test endpoint instead, so `order --test` goes through the risk gate
// like a real order and places nothing.
class TestEndpointVenue final : public trading::TradingVenue {
   public:
    explicit TestEndpointVenue(
        std::shared_ptr<trading::BinanceUSTradingVenue> venue)
        : mVenue(std::move(venue)) {}

    std::string_view Name() const override { return mVenue->Name(); }
    types::Result<types::OrderAcknowledgement> PlaceOrder(
        const types::OrderRequest& request) override {
        if (auto tested = mVenue->TestOrder(request); !tested) {
            return std::unexpected(tested.error());
        }
        return types::OrderAcknowledgement{
            "", request.client_order_id, types::OrderState::kNew, types::Now()};
    }
    types::Result<trading::CancelOutcome> CancelOrder(
        const std::string& order_id) override {
        return mVenue->CancelOrder(order_id);
    }
    types::Result<std::vector<std::string>> CancelAll() override {
        return mVenue->CancelAll();
    }
    types::Result<std::vector<types::OrderStatus>> GetOpenOrders() override {
        return mVenue->GetOpenOrders();
    }
    types::Result<types::OrderStatus> GetOrder(
        const std::string& order_id) override {
        return mVenue->GetOrder(order_id);
    }
    types::Result<std::vector<types::Fill>> GetFills(
        types::Timestamp since) override {
        return mVenue->GetFills(since);
    }
    types::Result<std::vector<types::Position>> GetPositions() override {
        return mVenue->GetPositions();
    }
    types::Result<std::vector<types::Balance>> GetBalances() override {
        return mVenue->GetBalances();
    }
    types::Result<std::unique_ptr<data::Subscription>> StreamExecutions(
        trading::ExecutionCallback callback) override {
        return mVenue->StreamExecutions(std::move(callback));
    }

   private:
    std::shared_ptr<trading::BinanceUSTradingVenue> mVenue;
};

// The close of the newest 1-minute bar in the last 15 minutes, to value a
// market order by.
std::optional<types::Decimal> RecentPrice(data::DataSource& data,
                                          const types::Symbol& symbol) {
    const auto now = types::Now();
    auto bars = data.GetBars(symbol, types::Timeframe::kM1,
                             now - std::chrono::minutes(15), now);
    if (!bars || bars->empty()) {
        spdlog::warn(
            "order: no recent 1-minute bar for {} ({}), so a market "
            "order cannot be valued for the risk limits",
            symbol, bars ? "none" : bars.error().ToString());
        return std::nullopt;
    }
    return bars->back().close;
}

// One order by hand, for checking a venue: --test sends it to the venue's test
// endpoint (nothing is placed), --confirm places it for real. One of the two
// must be given, so a real order is never sent by accident. Either way it
// passes the [risk] limits that apply to one order (ManualOrderLimits) unless
// --no-risk is given.
struct OrderArgs {
    std::string side;
    std::string symbol;
    std::string quantity;
    std::string limit;
    std::string stop;
    std::string tif = "gtc";
    std::string client_id;
    bool post_only = false;
    bool test = false;
    bool confirm = false;
    bool no_risk = false;
};

class OrderCommand : public Command {
   public:
    explicit OrderCommand(OrderArgs args) : mArgs(std::move(args)) {}

    int Run(const config::Configuration& config) override {
        if (mArgs.test == mArgs.confirm) {
            spdlog::error(
                "order: give exactly one of --test (the venue checks it, "
                "nothing is placed) or --confirm (a real order)");
            return 2;
        }
        if (config.GetMode() != config::Mode::kLive) {
            spdlog::error(
                "order: needs mode = \"live\"; a paper venue would forget the "
                "order when this command ends");
            return 2;
        }
        auto request = Request();
        if (!request) {
            spdlog::error("order: {}", request.error().ToString());
            return 2;
        }

        auto components = CreateComponents(config);
        if (!components) {
            spdlog::error("startup: {}", components.error().ToString());
            return 1;
        }
        std::shared_ptr<trading::TradingVenue> venue =
            std::move(components->venue);
        if (mArgs.test) {
            auto binance =
                std::dynamic_pointer_cast<trading::BinanceUSTradingVenue>(
                    venue);
            if (!binance) {
                spdlog::error("order --test: {} has no test endpoint",
                              venue->Name());
                return 2;
            }
            venue = std::make_shared<TestEndpointVenue>(std::move(binance));
        }

        const auto limits = ManualOrderLimits(config.GetRiskLimits());
        if (mArgs.no_risk) {
            spdlog::warn("order: --no-risk: not checked against [risk]");
        } else if (!limits) {
            spdlog::warn(
                "order: no [risk] limit applies to one order (the allowlist, "
                "max_order_notional, max_position_notional or "
                "max_open_orders), so it is not checked");
        } else {
            auto gate =
                std::make_shared<trading::RiskGatedVenue>(venue, *limits);
            auto data = components->data_source;
            gate->SetSymbolInfo([data](const types::Symbol& symbol) {
                return data->GetSymbolInfo(symbol);
            });
            const bool need_order_price =
                request->type == types::OrderType::kMarket ||
                limits->max_position_notional || limits->max_total_notional ||
                limits->max_symbol_share;
            if (need_order_price) {
                if (auto price = RecentPrice(*data, request->symbol)) {
                    gate->NoteReferencePrice(request->symbol, *price);
                }
            }
            if (limits->max_total_notional || limits->max_symbol_share) {
                for (const auto& symbol : config.GetSymbols()) {
                    if (symbol == request->symbol) {
                        continue;
                    }
                    if (auto price = RecentPrice(*data, symbol)) {
                        gate->NoteReferencePrice(symbol, *price);
                    }
                }
            }
            venue = std::move(gate);
        }

        auto ack = venue->PlaceOrder(*request);
        if (mArgs.test) {
            if (!ack) {
                spdlog::error("order --test: refused: {}",
                              ack.error().ToString());
                return 1;
            }
            std::cout << "test order accepted by " << venue->Name()
                      << " (nothing was placed)\n";
            return 0;
        }
        if (!ack) {
            spdlog::error("order: {}", ack.error().ToString());
            return 1;
        }
        std::cout << "placed order " << ack->order_id
                  << " client_id=" << ack->client_order_id
                  << " state=" << types::ToString(ack->state)
                  << " at=" << types::FormatIso8601(ack->time) << '\n';
        return 0;
    }

   private:
    types::Result<types::OrderRequest> Request() const {
        types::OrderRequest r;
        // As the config's symbols are, so the risk gate's allowlist matches.
        auto symbol = types::NormalizeSymbol(mArgs.symbol);
        if (!symbol) {
            return std::unexpected(symbol.error());
        }
        r.symbol = *symbol;
        r.client_order_id = mArgs.client_id;
        r.side = mArgs.side == "buy" ? types::Side::kBuy : types::Side::kSell;
        auto quantity = types::Decimal::Parse(mArgs.quantity);
        if (!quantity) {
            return std::unexpected(quantity.error());
        }
        r.quantity = *quantity;
        if (!mArgs.limit.empty() && !mArgs.stop.empty()) {
            return types::MakeError(types::ErrorCategory::kInvalidArgument,
                                    "give --limit or --stop, not both");
        }
        r.type = types::OrderType::kMarket;
        if (!mArgs.limit.empty()) {
            auto price = types::Decimal::Parse(mArgs.limit);
            if (!price) {
                return std::unexpected(price.error());
            }
            r.type = types::OrderType::kLimit;
            r.limit_price = *price;
        }
        if (!mArgs.stop.empty()) {
            auto price = types::Decimal::Parse(mArgs.stop);
            if (!price) {
                return std::unexpected(price.error());
            }
            r.type = types::OrderType::kStop;
            r.stop_price = *price;
        }
        r.time_in_force = mArgs.tif == "ioc"   ? types::TimeInForce::kIoc
                          : mArgs.tif == "fok" ? types::TimeInForce::kFok
                          : mArgs.tif == "day" ? types::TimeInForce::kDay
                                               : types::TimeInForce::kGtc;
        r.post_only = mArgs.post_only;
        return r;
    }

    OrderArgs mArgs;
};

struct CancelArgs {
    std::string order_id;
    bool all = false;
    bool confirm = false;
};

class CancelCommand : public Command {
   public:
    explicit CancelCommand(CancelArgs args) : mArgs(std::move(args)) {}

    int Run(const config::Configuration& config) override {
        if (!mArgs.confirm) {
            spdlog::error("cancel: add --confirm to cancel for real");
            return 2;
        }
        if (mArgs.all == !mArgs.order_id.empty()) {
            spdlog::error("cancel: give an order ID or --all, not both");
            return 2;
        }
        if (config.GetMode() != config::Mode::kLive) {
            spdlog::error("cancel: needs mode = \"live\"");
            return 2;
        }
        auto components = CreateComponents(config);
        if (!components) {
            spdlog::error("startup: {}", components.error().ToString());
            return 1;
        }
        auto& venue = *components->venue;

        if (mArgs.all) {
            auto cancelled = venue.CancelAll();
            if (!cancelled) {
                spdlog::error("cancel --all: {}", cancelled.error().ToString());
                return 1;
            }
            std::cout << "cancelled " << cancelled->size() << " order(s)";
            for (const auto& id : *cancelled) {
                std::cout << ' ' << id;
            }
            std::cout << '\n';
            return 0;
        }
        auto outcome = venue.CancelOrder(mArgs.order_id);
        if (!outcome) {
            spdlog::error("cancel {}: {}", mArgs.order_id,
                          outcome.error().ToString());
            return 1;
        }
        const char* what = *outcome == trading::CancelOutcome::kCancelled
                               ? "cancelled"
                           : *outcome == trading::CancelOutcome::kAlreadyFilled
                               ? "already filled"
                               : "already closed";
        std::cout << "order " << mArgs.order_id << ": " << what << '\n';
        return 0;
    }

   private:
    CancelArgs mArgs;
};

}  // namespace

void AddOrderCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "order",
        "Send one order to the live venue: --test has the venue check it "
        "without placing it, --confirm places it. A market order unless "
        "--limit or --stop is given");
    auto args = std::make_shared<OrderArgs>();
    sub->add_option("side", args->side, "buy or sell")
        ->required()
        ->check(CLI::IsMember({"buy", "sell"}));
    sub->add_option("symbol", args->symbol, "e.g. ZEC/USDT")->required();
    sub->add_option("quantity", args->quantity, "Base-asset units")->required();
    sub->add_option("--limit", args->limit, "Limit price");
    sub->add_option("--stop", args->stop, "Stop price (a stop-market order)");
    sub->add_option("--tif", args->tif, "gtc (default), ioc, fok or day")
        ->check(CLI::IsMember({"gtc", "ioc", "fok", "day"}));
    sub->add_flag("--post-only", args->post_only,
                  "With --limit: refused instead of taking liquidity");
    sub->add_option("--client-id", args->client_id,
                    "Client order ID (default: a generated kst-... ID)");
    sub->add_flag("--test", args->test,
                  "Send to the venue's test endpoint: checked, not placed");
    sub->add_flag("--confirm", args->confirm, "Place the order for real");
    sub->add_flag("--no-risk", args->no_risk,
                  "Skip the [risk] limits (e.g. to exit a position larger "
                  "than max_order_notional)");
    sub->callback(
        [&slot, args] { slot = std::make_unique<OrderCommand>(*args); });
}

void AddCancelCommand(CLI::App& app, CommandSlot& slot) {
    auto* sub = AddSubcommand(
        app, "cancel",
        "Cancel one order on the live venue by its ID, or --all open orders "
        "on the configured symbols. Needs --confirm");
    auto args = std::make_shared<CancelArgs>();
    sub->add_option("order_id", args->order_id, "The venue's order ID");
    sub->add_flag("--all", args->all, "Every open order on [general] symbols");
    sub->add_flag("--confirm", args->confirm, "Cancel for real");
    sub->callback(
        [&slot, args] { slot = std::make_unique<CancelCommand>(*args); });
}

}  // namespace kestrel::app
