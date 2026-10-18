#include "kestrel/types/symbol.hpp"

#include <algorithm>
#include <cctype>

namespace kestrel::types {
namespace {

bool ValidAsset(std::string_view s) {
    if (s.empty() || s.size() > 16) {
        return false;
    }
    return std::ranges::all_of(s, [](unsigned char c) {
        return std::isupper(c) || std::isdigit(c) || c == '.';
    });
}

}  // namespace

Result<Symbol> NormalizeSymbol(std::string_view text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    Symbol out;
    out.reserve(text.size());
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        out.push_back(raw == '-' || raw == '_'
                          ? '/'
                          : static_cast<char>(std::toupper(c)));
    }
    if (auto parts = SplitSymbol(out); !parts) {
        return std::unexpected(parts.error());
    }
    return out;
}

Result<SymbolParts> SplitSymbol(std::string_view symbol) {
    const auto slash = symbol.find('/');
    if (slash == std::string_view::npos) {
        if (!ValidAsset(symbol)) {
            return MakeError(ErrorCategory::kInvalidArgument,
                             "invalid symbol '" + std::string(symbol) + "'");
        }
        return SymbolParts{std::string(symbol), std::nullopt};
    }
    const auto base = symbol.substr(0, slash);
    const auto quote = symbol.substr(slash + 1);
    if (!ValidAsset(base) || !ValidAsset(quote) ||
        quote.find('/') != std::string_view::npos) {
        return MakeError(ErrorCategory::kInvalidArgument,
                         "invalid symbol '" + std::string(symbol) + "'");
    }
    return SymbolParts{std::string(base), std::string(quote)};
}

std::string JoinSymbol(std::string_view base, std::string_view quote) {
    return std::string(base) + "/" + std::string(quote);
}

}  // namespace kestrel::types
