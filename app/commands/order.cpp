#include <spdlog/spdlog.h>

#include <iostream>

#include "command.hpp"
#include "common.hpp"
#include "kestrel/factory.hpp"
#include "kestrel/trading/binanceus_trading_venue.hpp"

namespace kestrel::app {

namespace {

// One order by hand, for checking a venue: --test sends it to the venue's test
// endpoint (nothing is placed), --confirm places it for real. One of the two
// must be given, so a real order is never sent by accident.
struct OrderArgs {
    std::string side;
    std::string symbol;
    std::string quantity;
    std::string limit;
    std::string stop;
    std::string tif = "gtc";
    std::string client_id;
    bool test = false;
    bool confirm = false;
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
        auto& venue = *components->venue;

        if (mArgs.test) {
            auto* binance =
                dynamic_cast<trading::BinanceUSTradingVenue*>(&venue);
            if (!binance) {
                spdlog::error("order --test: {} has no test endpoint",
                              venue.Name());
                return 2;
            }
            if (auto tested = binance->TestOrder(*request); !tested) {
                spdlog::error("order --test: refused: {}",
                              tested.error().ToString());
                return 1;
            }
            std::cout << "test order accepted by " << venue.Name()
                      << " (nothing was placed)\n";
            return 0;
        }

        auto ack = venue.PlaceOrder(*request);
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
        r.symbol = mArgs.symbol;
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
    sub->add_option("--client-id", args->client_id,
                    "Client order ID (default: a generated kst-... ID)");
    sub->add_flag("--test", args->test,
                  "Send to the venue's test endpoint: checked, not placed");
    sub->add_flag("--confirm", args->confirm, "Place the order for real");
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
