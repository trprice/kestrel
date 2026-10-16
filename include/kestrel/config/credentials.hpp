#pragma once

#include <string>

namespace kestrel::config {

// Secrets never live in the main config file: they come from environment
// variables or a separate secrets file that is excluded from version control.
// Deliberately no stream/format operators, so they are hard to log by accident.

struct KrakenCredentials {
  std::string api_key;
  std::string api_secret;  // base64, as shown in Kraken's key management page
};

struct BinanceUSCredentials {
  std::string api_key;
  std::string api_secret;
};

struct SchwabCredentials {
  std::string app_key;     // OAuth client id
  std::string app_secret;  // OAuth client secret
};

}  // namespace kestrel::config
