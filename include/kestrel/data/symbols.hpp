#pragma once

#include <string>
#include <string_view>

#include "kestrel/types/symbol.hpp"

// Translation between Kestrel's normalized symbols (BTC/USD, AAPL) and each
// provider's native format.
namespace kestrel::data {

// --- Kraken -----------------------------------------------------------------
// Both the REST API and the v2 WebSocket API accept the standard BASE/QUOTE
// form ("BTC/USD", "DOGE/USD"), though responses may use Kraken's legacy
// codes (XBT, XDG, XXBTZUSD), which FromKrakenSymbol maps back.
types::Result<std::string> ToKrakenSymbol(std::string_view symbol);
// Accepts every form Kraken emits: "XBT/USD", "BTC/USD", "XBTUSD", "XXBTZUSD".
types::Result<types::Symbol> FromKrakenSymbol(std::string_view native);

// --- Binance.US
// --------------------------------------------------------------- Concatenated
// upper-case pair: "BTCUSD", "ETHUSDT".
types::Result<std::string> ToBinanceUSSymbol(std::string_view symbol);
types::Result<types::Symbol> FromBinanceUSSymbol(std::string_view native);

// --- Schwab
// ------------------------------------------------------------------- Equities
// only; native tickers match the normalized form.
// TODO: confirm how Schwab spells share classes (BRK.B vs BRK/B) against the
// live API before enabling those symbols; options/futures are not mapped.
types::Result<std::string> ToSchwabSymbol(std::string_view symbol);
types::Result<types::Symbol> FromSchwabSymbol(std::string_view native);

}  // namespace kestrel::data
