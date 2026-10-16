#pragma once

#include <string>
#include <string_view>

#include "kestrel/types/error.hpp"

namespace kestrel::util {

// Raw (binary) digests via OpenSSL. Kraken signs with HMAC-SHA512 and
// Binance.US with HMAC-SHA256.
std::string HmacSha256(std::string_view key, std::string_view message);
std::string HmacSha512(std::string_view key, std::string_view message);
std::string Sha256(std::string_view message);

std::string HexEncode(std::string_view bytes);  // lower-case
std::string Base64Encode(std::string_view bytes);
types::Result<std::string> Base64Decode(std::string_view text);

}  // namespace kestrel::util
