#include "kestrel/util/hmac.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <array>
#include <stdexcept>
#include <vector>

namespace kestrel::util {
namespace {

std::string Hmac(const EVP_MD* md, std::string_view key,
                 std::string_view message) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!HMAC(md, key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(message.data()),
              message.size(), out, &len)) {
        throw std::runtime_error("OpenSSL HMAC failed");
    }
    return std::string(reinterpret_cast<char*>(out), len);
}

}  // namespace

std::string HmacSha256(std::string_view key, std::string_view message) {
    return Hmac(EVP_sha256(), key, message);
}

std::string HmacSha512(std::string_view key, std::string_view message) {
    return Hmac(EVP_sha512(), key, message);
}

std::string Sha256(std::string_view message) {
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(message.data()),
           message.size(), out);
    return std::string(reinterpret_cast<char*>(out), sizeof out);
}

std::string HexEncode(std::string_view bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0xF]);
    }
    return out;
}

std::string Base64Encode(std::string_view bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    const int n =
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                        reinterpret_cast<const unsigned char*>(bytes.data()),
                        static_cast<int>(bytes.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

types::Result<std::string> Base64Decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        return types::MakeError(types::ErrorCategory::kParse,
                                "invalid base64 length");
    }
    std::string out(text.size() / 4 * 3, '\0');
    const int n =
        EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                        reinterpret_cast<const unsigned char*>(text.data()),
                        static_cast<int>(text.size()));
    if (n < 0) {
        return types::MakeError(types::ErrorCategory::kParse, "invalid base64");
    }
    std::size_t size = static_cast<std::size_t>(n);
    // EVP_DecodeBlock counts padding bytes as output; trim them.
    for (std::size_t i = 0;
         i < 2 && i < text.size() && text[text.size() - 1 - i] == '='; ++i)
        --size;
    out.resize(size);
    return out;
}

}  // namespace kestrel::util
