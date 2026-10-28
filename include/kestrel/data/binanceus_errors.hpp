#pragma once

#include "kestrel/types/error.hpp"

namespace kestrel::data {

// Binance error bodies look like {"code":-1121,"msg":"Invalid symbol."}.
// Refines an HTTP-status error using that body when present: the code goes in
// `provider_code` and these set the category:
//   -1003 rate limited                       kRateLimit
//   -1121 invalid symbol                     kNotFound
//   -2011, -2013 unknown order (cancel/query) kNotFound
//   -1022, -2014, -2015 bad signature, key,
//         or key/IP/permission refused       kAuth
// Everything else keeps the category the HTTP status gave it (a refused order,
// -2010, stays kRejected).
types::Error RefineBinanceUSError(types::Error error);

}  // namespace kestrel::data
