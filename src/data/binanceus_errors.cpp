#include "kestrel/data/binanceus_errors.hpp"

#include "kestrel/util/json.hpp"

namespace kestrel::data {

using types::ErrorCategory;

namespace {

bool MentionsBalance(std::string text) {
    for (auto& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text.find("balance") != std::string::npos;
}

}  // namespace

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
    } else if (code == -2010 && MentionsBalance(err.message)) {
        // -2010 is any refused new order (it would match a LIMIT_MAKER, the
        // stop would trigger at once, the market is closed...), so only one
        // that talks about the balance is a shortage of funds. Binance's
        // wording is "Account has insufficient balance for requested
        // action."; any mention of "balance" counts, in any case, so a
        // rewording is not missed.
        err.category = ErrorCategory::kInsufficientFunds;
    }
    return err;
}

}  // namespace kestrel::data
