#include "kestrel/trading/pnl_tracker.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace kestrel::trading {

using types::Decimal;
using types::Rounding;
using types::Side;

namespace {

constexpr int kScale = 8;
// A starting holding that differs from the stored history by less than this
// in the quote currency (exchange dust, rounding) is logged at info, not as a
// warning on every start. Only the log level depends on it.
const Decimal kDustValue = Decimal::FromInt(1);

Decimal Round(const Decimal& d) {
    return d.Rescale(kScale, Rounding::kNearest);
}

// The fee in the quote asset.
Decimal FeeInQuote(const types::Fill& fill) {
    auto parts = types::SplitSymbol(fill.symbol);
    if (parts && !fill.fee_asset.empty() && fill.fee_asset == parts->base) {
        return fill.fee * fill.price;
    }
    return fill.fee;
}

}  // namespace

void PnlTracker::Reset(const std::vector<types::Position>& positions) {
    mHoldings.clear();
    for (const auto& position : positions) {
        mHoldings[position.symbol] =
            Holding{position.quantity, position.average_price, Decimal()};
    }
}

void PnlTracker::ResetRealized() {
    for (auto& [symbol, holding] : mHoldings) {
        holding.realized = Decimal();
    }
}

void PnlTracker::Apply(const types::Fill& fill) {
    const auto parts = types::SplitSymbol(fill.symbol);
    if (parts && parts->quote && !fill.fee_asset.empty() &&
        fill.fee_asset != parts->base && fill.fee_asset != *parts->quote &&
        mOtherFeeAssets.insert(fill.fee_asset).second) {
        // Valued 1:1 with the quote asset, which is wrong (a BNB fee on
        // Binance.US): the P&L and the risk gate's loss count are off by it.
        spdlog::warn(
            "pnl: fees in {} (on {}) are counted as if in {}; P&L is off by "
            "the difference",
            fill.fee_asset, fill.symbol, *parts->quote);
    }
    Holding& holding = mHoldings[fill.symbol];
    const Decimal fee = FeeInQuote(fill);

    // Signed: what the fill adds to the position, and what of the position it
    // closes first (a buy closes a short, a sell a long). A fee paid in the
    // base asset reduces what a buy adds, and adds to what a sell takes from
    // the holding (realizing the fee coins against the average cost, or adding
    // them to a short at the fill price).
    const bool buy = fill.side == Side::kBuy;
    Decimal quantity = fill.quantity;
    if (parts && !fill.fee_asset.empty() && fill.fee_asset == parts->base &&
        fill.fee.Sign() > 0) {
        quantity = buy ? std::max(fill.quantity - fill.fee, Decimal())
                       : fill.quantity + fill.fee;
    }
    const Decimal open = buy ? -holding.quantity : holding.quantity;
    Decimal closed = std::min(quantity, std::max(open, Decimal()));
    Decimal opened = quantity - closed;
    if (!buy && !mShorts && holding.quantity.Sign() >= 0) {
        opened = Decimal();  // spot: an excess sale has no cost basis
    }
    Decimal gained = -fee;
    if (closed.Sign() > 0) {
        gained += (buy ? holding.average_price - fill.price
                       : fill.price - holding.average_price) *
                  closed;
        holding.quantity += buy ? closed : -closed;
        if (holding.quantity.IsZero()) {
            holding.average_price = Decimal();
        }
    }
    holding.realized = Round(holding.realized + gained);
    if (opened.Sign() > 0) {
        const Decimal size = holding.quantity.Abs();
        holding.average_price =
            Decimal::Divide(holding.average_price * size + fill.price * opened,
                            size + opened, kScale, Rounding::kNearest);
        holding.quantity += buy ? opened : -opened;
    }
}

Decimal PnlTracker::Realized() const {
    Decimal sum;
    for (const auto& [symbol, holding] : mHoldings) {
        sum += holding.realized;
    }
    return sum;
}

Decimal PnlTracker::Realized(const types::Symbol& symbol) const {
    auto it = mHoldings.find(symbol);
    return it == mHoldings.end() ? Decimal() : it->second.realized;
}

std::vector<types::Position> PnlTracker::Positions() const {
    std::vector<types::Position> out;
    out.reserve(mHoldings.size());
    for (const auto& [symbol, holding] : mHoldings) {
        out.push_back({symbol, holding.quantity, holding.average_price,
                       holding.realized});
    }
    return out;
}

Decimal PnlTracker::Held(const types::Symbol& symbol) const {
    auto it = mHoldings.find(symbol);
    return it == mHoldings.end() ? Decimal() : it->second.quantity;
}

std::vector<types::Position> ReconcileStartingPositions(
    std::span<const types::Symbol> symbols, std::span<const types::Fill> fills,
    std::span<const types::Balance> balances,
    const std::function<std::optional<Decimal>(const types::Symbol&)>&
        reference_price) {
    const auto by_time_and_id = [](const types::Fill& a, const types::Fill& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        return a.fill_id.size() != b.fill_id.size()
                   ? a.fill_id.size() < b.fill_id.size()
                   : a.fill_id < b.fill_id;
    };

    // The base assets of the run's symbols.
    std::set<std::string> run_bases;
    for (const auto& raw : symbols) {
        if (const auto parts =
                types::SplitSymbol(types::NormalizeSymbol(raw).value_or(raw))) {
            run_bases.insert(parts->base);
        }
    }

    PnlTracker tracker(/*shorts=*/false);
    // A fill quoted in one of the run's base assets (a ZEC/BTC buy spends
    // BTC) changes that asset's holding as well. Coins spent come off its
    // holdings at their average cost (on the run's pairs first, so that
    // replay matches the wallet); coins received have no cost basis in the
    // run's quote currency, so they are left for the wallet's excess, valued
    // at the reference price below.
    const auto spend_quote = [&](const std::string& asset, Decimal amount,
                                 types::Timestamp time) {
        std::vector<types::Symbol> holders;
        for (const auto& raw : symbols) {
            const types::Symbol sym = types::NormalizeSymbol(raw).value_or(raw);
            const auto parts = types::SplitSymbol(sym);
            if (parts && parts->base == asset &&
                std::ranges::find(holders, sym) == holders.end()) {
                holders.push_back(sym);
            }
        }
        for (const auto& pos : tracker.Positions()) {
            const auto parts = types::SplitSymbol(pos.symbol);
            if (parts && parts->base == asset &&
                std::ranges::find(holders, pos.symbol) == holders.end()) {
                holders.push_back(pos.symbol);
            }
        }
        for (const auto& sym : holders) {
            if (amount.Sign() <= 0) {
                break;
            }
            const Decimal held = tracker.Held(sym);
            if (held.Sign() <= 0) {
                continue;
            }
            types::Fill spent;
            spent.symbol = sym;
            spent.side = types::Side::kSell;
            spent.quantity = std::min(held, amount);
            // Any price: what it realizes is zeroed below, and a sale leaves
            // the average cost as it is.
            spent.price = Decimal();
            spent.time = time;
            tracker.Apply(spent);
            amount -= spent.quantity;
        }
    };
    const auto apply_fill = [&](const types::Fill& fill) {
        const types::Symbol norm =
            types::NormalizeSymbol(fill.symbol).value_or(fill.symbol);
        types::Fill normalized = fill;
        normalized.symbol = norm;
        tracker.Apply(normalized);
        const auto parts = types::SplitSymbol(norm);
        if (!parts || !parts->quote || *parts->quote == parts->base ||
            !run_bases.contains(*parts->quote)) {
            return;
        }
        // Fees with no asset are in the quote asset, as in FeeInQuote.
        const bool fee_in_quote =
            fill.fee_asset.empty() || fill.fee_asset == *parts->quote;
        const Decimal fee = fee_in_quote ? fill.fee : Decimal();
        const Decimal notional = fill.quantity * fill.price;
        const Decimal spent = fill.side == types::Side::kBuy
                                  ? notional + fee
                                  : fee - notional;  // negative: received
        if (spent.Sign() > 0) {
            spend_quote(*parts->quote, spent, fill.time);
        }
    };
    if (std::ranges::is_sorted(fills, by_time_and_id)) {
        for (const auto& fill : fills) {
            apply_fill(fill);
        }
    } else {
        std::vector<types::Fill> sorted(fills.begin(), fills.end());
        std::ranges::sort(sorted, by_time_and_id);
        for (const auto& fill : sorted) {
            apply_fill(fill);
        }
    }
    tracker.ResetRealized();

    std::map<types::Symbol, types::Position> replayed_by_symbol;
    std::map<std::string, Decimal> replayed_total_by_base;
    for (const auto& pos : tracker.Positions()) {
        if (pos.quantity.Sign() <= 0) {
            continue;
        }
        replayed_by_symbol[pos.symbol] = {pos.symbol, pos.quantity,
                                          pos.average_price, Decimal()};
        if (const auto parts = types::SplitSymbol(pos.symbol)) {
            replayed_total_by_base[parts->base] += pos.quantity;
        }
    }

    std::map<std::string, Decimal> wallet_by_base;
    for (const auto& balance : balances) {
        if (balance.total.Sign() > 0) {
            wallet_by_base[balance.asset] += balance.total;
        }
    }

    std::vector<std::string> base_order;
    std::map<std::string, std::vector<types::Symbol>> our_symbols_by_base;
    std::set<types::Symbol> seen_symbols;
    for (const auto& raw : symbols) {
        const types::Symbol sym = types::NormalizeSymbol(raw).value_or(raw);
        if (!seen_symbols.insert(sym).second) {
            continue;
        }
        const auto parts = types::SplitSymbol(sym);
        if (!parts) {
            continue;
        }
        auto [it, inserted] = our_symbols_by_base.try_emplace(parts->base);
        if (inserted) {
            base_order.push_back(parts->base);
        }
        it->second.push_back(sym);
    }

    std::vector<types::Position> out;
    for (const auto& base : base_order) {
        const auto& our_syms = our_symbols_by_base[base];
        const Decimal wallet = wallet_by_base[base];
        Decimal replayed_ours;
        for (const auto& sym : our_syms) {
            if (const auto it = replayed_by_symbol.find(sym);
                it != replayed_by_symbol.end()) {
                replayed_ours += it->second.quantity;
            }
        }
        const Decimal replayed_other =
            replayed_total_by_base[base] - replayed_ours;

        if (wallet == replayed_ours) {
            for (const auto& sym : our_syms) {
                if (const auto it = replayed_by_symbol.find(sym);
                    it != replayed_by_symbol.end() &&
                    it->second.quantity.Sign() > 0) {
                    out.push_back(it->second);
                }
            }
            continue;
        }

        if (wallet > replayed_ours) {
            const Decimal excess = wallet - replayed_ours;
            const std::string other_note =
                replayed_other.Sign() > 0
                    ? "; " + replayed_other.Normalized().ToString() + " " +
                          base + " replayed on other pairs"
                    : "";
            types::Symbol target;
            std::optional<Decimal> mark;
            const auto try_price = [&](const types::Symbol& sym) {
                if (!reference_price) {
                    return false;
                }
                const auto p = reference_price(sym);
                if (p && p->Sign() > 0) {
                    target = sym;
                    mark = *p;
                    return true;
                }
                return false;
            };
            for (const auto& sym : our_syms) {
                if (replayed_by_symbol.contains(sym) && try_price(sym)) {
                    break;
                }
            }
            if (!mark) {
                for (const auto& sym : our_syms) {
                    if (!replayed_by_symbol.contains(sym) && try_price(sym)) {
                        break;
                    }
                }
            }
            if (target.empty()) {
                for (const auto& sym : our_syms) {
                    if (replayed_by_symbol.contains(sym)) {
                        target = sym;
                        break;
                    }
                }
                if (target.empty()) {
                    target = our_syms.front();
                }
            }

            for (const auto& sym : our_syms) {
                Decimal qty_0;
                Decimal avg_0;
                if (const auto it = replayed_by_symbol.find(sym);
                    it != replayed_by_symbol.end()) {
                    qty_0 = it->second.quantity;
                    avg_0 = it->second.average_price;
                }
                if (sym == target) {
                    if (mark) {
                        const Decimal new_qty = qty_0 + excess;
                        const Decimal cost = avg_0 * qty_0 + *mark * excess;
                        const Decimal new_avg = Decimal::Divide(
                            cost, new_qty, kScale, Rounding::kNearest);
                        const auto level = excess * *mark < kDustValue
                                               ? spdlog::level::info
                                               : spdlog::level::warn;
                        spdlog::log(
                            level,
                            "starting cost basis: wallet holds {} {} ({} more "
                            "than stored account_fills replay {}{}); valuing "
                            "{} {} on {} at reference price {} (starting "
                            "position {} @ {})",
                            wallet.Normalized().ToString(), base,
                            excess.Normalized().ToString(),
                            replayed_ours.Normalized().ToString(), other_note,
                            excess.Normalized().ToString(), base, target,
                            mark->Normalized().ToString(),
                            new_qty.Normalized().ToString(),
                            new_avg.Normalized().ToString());
                        out.push_back({sym, new_qty, new_avg, Decimal()});
                    } else {
                        spdlog::warn(
                            "starting cost basis: wallet holds {} {} ({} more "
                            "than stored account_fills replay {}{}), and {} "
                            "has no reference price; leaving unpriced {} {} "
                            "out of the starting cost basis (kept {} @ {})",
                            wallet.Normalized().ToString(), base,
                            excess.Normalized().ToString(),
                            replayed_ours.Normalized().ToString(), other_note,
                            target, excess.Normalized().ToString(), base,
                            qty_0.Normalized().ToString(),
                            avg_0.Normalized().ToString());
                        if (qty_0.Sign() > 0) {
                            out.push_back({sym, qty_0, avg_0, Decimal()});
                        }
                    }
                } else if (qty_0.Sign() > 0) {
                    out.push_back({sym, qty_0, avg_0, Decimal()});
                }
            }
            continue;
        }

        // wallet < replayed_ours: external withdrawal or unsynced sale.
        Decimal remaining = wallet;
        for (const auto& sym : our_syms) {
            const auto it = replayed_by_symbol.find(sym);
            if (it == replayed_by_symbol.end() ||
                it->second.quantity.Sign() <= 0) {
                continue;
            }
            const Decimal qty_0 = it->second.quantity;
            const Decimal avg_0 = it->second.average_price;
            const Decimal new_qty = std::min(qty_0, remaining);
            remaining -= new_qty;
            const Decimal new_avg = new_qty.Sign() > 0 ? avg_0 : Decimal();
            if (new_qty < qty_0) {
                const auto level = (qty_0 - new_qty) * avg_0 < kDustValue
                                       ? spdlog::level::info
                                       : spdlog::level::warn;
                spdlog::log(
                    level,
                    "starting cost basis: wallet holds {} {} (less than stored "
                    "account_fills replay {}); clamping {} from {} down to "
                    "{} @ {}",
                    wallet.Normalized().ToString(), base,
                    replayed_ours.Normalized().ToString(), sym,
                    qty_0.Normalized().ToString(),
                    new_qty.Normalized().ToString(),
                    new_avg.Normalized().ToString());
            }
            out.push_back({sym, new_qty, new_avg, Decimal()});
        }
    }
    return out;
}

}  // namespace kestrel::trading
