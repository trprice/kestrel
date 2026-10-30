#include "kestrel/util/url.hpp"

namespace kestrel::util {

namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

std::string Decode(std::string_view text, bool plus_is_space) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '+' && plus_is_space) {
            out += ' ';
        } else if (c == '%' && i + 2 < text.size() &&
                   HexValue(text[i + 1]) >= 0 && HexValue(text[i + 2]) >= 0) {
            out += static_cast<char>(HexValue(text[i + 1]) * 16 +
                                     HexValue(text[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

std::string UrlEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
            c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

std::string UrlDecode(std::string_view text) { return Decode(text, true); }

std::string PercentDecode(std::string_view text) {
    return Decode(text, false);
}

std::string FormEncode(const std::map<std::string, std::string>& fields) {
    std::string out;
    for (const auto& [key, value] : fields) {
        if (!out.empty()) {
            out += '&';
        }
        out += UrlEncode(key) + '=' + UrlEncode(value);
    }
    return out;
}

}  // namespace kestrel::util
