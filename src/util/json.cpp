#include "kestrel/util/json.hpp"

#include <vector>

namespace kestrel::util {
namespace {

using nlohmann::json;

// Builds a json tree like nlohmann's default parser, except number_float
// receives the literal text, which we keep as a string.
class ExactNumberSax : public nlohmann::json_sax<json> {
   public:
    json root;

    bool null() override { return Add(json(nullptr)); }
    bool boolean(bool v) override { return Add(json(v)); }
    bool number_integer(number_integer_t v) override { return Add(json(v)); }
    bool number_unsigned(number_unsigned_t v) override { return Add(json(v)); }
    bool number_float(number_float_t, const string_t& literal) override {
        return Add(json(literal));
    }
    bool string(string_t& v) override { return Add(json(v)); }
    bool binary(binary_t&) override { return false; }

    bool start_object(std::size_t) override { return Open(json::object()); }
    bool key(string_t& k) override {
        mKey = k;
        return true;
    }
    bool end_object() override { return Close(); }
    bool start_array(std::size_t) override { return Open(json::array()); }
    bool end_array() override { return Close(); }
    bool parse_error(std::size_t, const std::string&,
                     const nlohmann::detail::exception&) override {
        return false;
    }

   private:
    json* Slot(json&& value) {
        if (mStack.empty()) {
            root = std::move(value);
            return &root;
        }
        json* top = mStack.back();
        if (top->is_array()) {
            top->push_back(std::move(value));
            return &top->back();
        }
        return &((*top)[mKey] = std::move(value));
    }
    bool Add(json&& v) {
        Slot(std::move(v));
        return true;
    }
    bool Open(json&& container) {
        mStack.push_back(Slot(std::move(container)));
        return true;
    }
    bool Close() {
        mStack.pop_back();
        return true;
    }

    std::vector<json*> mStack;
    std::string mKey;
};

}  // namespace

types::Result<nlohmann::json> ParseJson(std::string_view text) {
    ExactNumberSax sax;
    if (!json::sax_parse(text, &sax)) {
        return types::MakeError(types::ErrorCategory::kParse, "malformed JSON");
    }
    return std::move(sax.root);
}

types::Result<types::Decimal> JsonDecimal(const nlohmann::json& value) {
    if (value.is_string()) {
        return types::Decimal::Parse(value.get_ref<const std::string&>());
    }
    if (value.is_number_integer()) {
        return types::Decimal(value.get<std::int64_t>(), 0);
    }
    return types::MakeError(types::ErrorCategory::kParse,
                            "expected a numeric JSON value");
}

}  // namespace kestrel::util
