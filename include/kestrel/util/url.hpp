#pragma once

#include <map>
#include <string>
#include <string_view>

namespace kestrel::util {

// Percent-encodes everything but the RFC 3986 unreserved characters.
std::string UrlEncode(std::string_view text);

// Reverses UrlEncode, and also reads '+' as a space (form encoding). A '%'
// not followed by two hex digits is kept as it is.
std::string UrlDecode(std::string_view text);

// Like UrlDecode, but keeps '+' as it is: for values that never hold a space,
// such as an OAuth code, where a '+' is part of the value.
std::string PercentDecode(std::string_view text);

// "k1=v1&k2=v2", each key and value encoded with UrlEncode, in key order: an
// application/x-www-form-urlencoded body.
std::string FormEncode(const std::map<std::string, std::string>& fields);

}  // namespace kestrel::util
