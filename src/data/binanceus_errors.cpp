#include "kestrel/data/binanceus_errors.hpp"

#include "kestrel/util/json.hpp"

namespace kestrel::data {

using types::ErrorCategory;

types::Error RefineBinanceUSError(types::Error err) {
    const auto brace = err.message.find('{');
    if (brace == std::string::npos) {
        return err;
    }
    auto body = util::ParseJson(std::string_view(err.message).substr(brace));
    if (!body || !body->is_object() || !body->contains("code") ||
        !(*body)["code"].is_number_integer()) {
        return err;
    }
    const auto code = (*body)["code"].get<std::int64_t>();
    err.provider_code = std::to_string(code);
    if (code == -1121 || code == -2011 || code == -2013) {
        err.category = ErrorCategory::kNotFound;
    } else if (code == -1003) {
        err.category = ErrorCategory::kRateLimit;
    } else if (code == -2014 || code == -2015 || code == -1022) {
        err.category = ErrorCategory::kAuth;
    }
    return err;
}

}  // namespace kestrel::data
