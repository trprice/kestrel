#pragma once

#include <memory>

#include "kestrel/config/configuration.hpp"
#include "kestrel/data/data_source.hpp"
#include "kestrel/trading/trading_venue.hpp"

namespace kestrel {

// The two halves of a provider connection. Market data and order execution are
// separate objects: only the venue differs between paper and live mode.
struct Components {
    std::shared_ptr<data::DataSource> data_source;
    std::unique_ptr<trading::TradingVenue> venue;
};

// Builds the provider's DataSource and a TradingVenue chosen by
// configuration.GetMode(): a PaperTradingVenue (already streaming quotes for
// the configured symbols) in paper mode, the provider's live venue in live
// mode. Callers use only the abstract interfaces.
//
// Live mode fails with kAuth if the provider's credentials are not configured.
types::Result<Components> CreateComponents(
    const config::Configuration& configuration);

}  // namespace kestrel
