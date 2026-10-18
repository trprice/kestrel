#include "kestrel/types/decimal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ostream>
#include <stdexcept>

// GCC and Clang provide __int128, which lets every scale alignment and
// multiplication below be computed exactly before narrowing back to int64.
// TODO: MSVC has no __int128; it needs a small wide-integer fallback.
namespace kestrel::types {
namespace {

using i128 = __int128;
constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();

constexpr std::array<i128, 39> MakePow10() {
    std::array<i128, 39> t{};
    t[0] = 1;
    for (std::size_t i = 1; i < t.size(); ++i) t[i] = t[i - 1] * 10;
    return t;
}
constexpr auto kPow10 = MakePow10();

i128 Pow10(int n) { return kPow10.at(static_cast<std::size_t>(n)); }

bool FitsI64(i128 v) { return v >= kI64Min && v <= kI64Max; }

i128 Abs128(i128 v) { return v < 0 ? -v : v; }

// Divides v by d (> 0) rounding in the requested direction.
i128 DivRound(i128 v, i128 d, Rounding mode) {
    i128 q = v / d;
    i128 r = v % d;
    if (r < 0)  // C++ truncates toward zero; convert to floor
    {
        r += d;
        --q;
    }
    // Now v == q*d + r with 0 <= r < d.
    if (r == 0) {
        return q;
    }
    switch (mode) {
        case Rounding::kDown:
            return q;
        case Rounding::kUp:
            return q + 1;
        case Rounding::kNearest: {
            const i128 twice = r * 2;
            if (twice > d) {
                return q + 1;
            }
            if (twice < d) {
                return q;
            }
            return v >= 0 ? q + 1 : q;  // tie: away from zero
        }
    }
    return q;
}

// Builds a Decimal from a wide value, shrinking the scale (rounding to
// nearest, once) until the scale is <= kMaxScale and the mantissa fits.
Decimal FromWide(i128 v, int scale) {
    int drop = std::max(0, scale - Decimal::kMaxScale);
    while (drop < scale &&
           !FitsI64(DivRound(v, Pow10(drop), Rounding::kNearest)))
        ++drop;
    const i128 m = drop > 0 ? DivRound(v, Pow10(drop), Rounding::kNearest) : v;
    if (!FitsI64(m)) {
        throw std::overflow_error("Decimal overflow");
    }
    return Decimal(static_cast<std::int64_t>(m), scale - drop);
}

// Mantissa widened to `scale` (>= d.Scale()).
i128 Widen(const Decimal& d, int scale) {
    return static_cast<i128>(d.Mantissa()) * Pow10(scale - d.Scale());
}

}  // namespace

Decimal::Decimal(std::int64_t mantissa, int scale)
    : mMantissa(mantissa), mScale(scale) {
    if (scale < 0 || scale > kMaxScale) {
        throw std::invalid_argument("Decimal scale out of range");
    }
}

Result<Decimal> Decimal::Parse(std::string_view text) {
    auto bad = [&](const char* why) {
        return MakeError(
            ErrorCategory::kParse,
            std::string("invalid decimal '") + std::string(text) + "': " + why);
    };
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        negative = text[i++] == '-';
    }

    i128 value = 0;
    int digits = 0;
    int frac_digits = 0;
    bool seen_point = false;
    constexpr i128 kLimit =
        (static_cast<i128>(1) << 120);  // keeps value*10+9 in range
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.') {
            if (seen_point) {
                return bad("multiple decimal points");
            }
            seen_point = true;
        } else if (c >= '0' && c <= '9') {
            if (value >= kLimit) {
                return bad("too many digits");
            }
            value = value * 10 + (c - '0');
            ++digits;
            if (seen_point) {
                ++frac_digits;
            }
        } else {
            break;
        }
    }
    if (digits == 0) {
        return bad("no digits");
    }

    int exponent = 0;
    if (i < text.size()) {
        if (text[i] != 'e' && text[i] != 'E') {
            return bad("unexpected character");
        }
        ++i;
        bool exp_negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            exp_negative = text[i++] == '-';
        }
        if (i >= text.size()) {
            return bad("missing exponent digits");
        }
        for (; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return bad("unexpected character");
            }
            exponent = exponent * 10 + (text[i] - '0');
            if (exponent > 1000) {
                return bad("exponent out of range");
            }
        }
        if (exp_negative) {
            exponent = -exponent;
        }
    }

    if (negative) {
        value = -value;
    }
    int scale = frac_digits - exponent;
    if (value == 0) {
        return Decimal(0, std::clamp(scale, 0, kMaxScale));
    }
    if (scale < 0) {
        if (-scale > 38) {
            return bad("value out of range");
        }
        const i128 factor = Pow10(-scale);
        if (Abs128(value) > static_cast<i128>(kI64Max) / factor) {
            return bad("value out of range");
        }
        value *= factor;
        scale = 0;
    }
    if (scale > 38) {
        return bad("too many fractional digits");
    }
    try {
        return FromWide(value, scale);
    } catch (const std::overflow_error&) {
        return bad("value out of range");
    }
}

Decimal Decimal::Abs() const { return mMantissa < 0 ? -*this : *this; }

Decimal Decimal::Divide(const Decimal& a, const Decimal& b, int scale,
                        Rounding mode) {
    if (b.mMantissa == 0) {
        throw std::domain_error("Decimal division by zero");
    }
    if (scale < 0 || scale > kMaxScale) {
        throw std::invalid_argument("Decimal scale out of range");
    }
    // (a.m / 10^as) / (b.m / 10^bs) * 10^scale = a.m * 10^(scale + bs) / (b.m *
    // 10^as)
    const int shift = scale + b.mScale;
    if (a.mMantissa != 0 && Abs128(static_cast<i128>(a.mMantissa)) >
                                (static_cast<i128>(1) << 126) / Pow10(shift)) {
        throw std::overflow_error("Decimal overflow");
    }
    i128 num = static_cast<i128>(a.mMantissa) * Pow10(shift);
    i128 den = static_cast<i128>(b.mMantissa) * Pow10(a.mScale);
    if (den < 0) {
        num = -num;
        den = -den;
    }
    const i128 q = DivRound(num, den, mode);
    if (!FitsI64(q)) {
        throw std::overflow_error("Decimal overflow");
    }
    return Decimal(static_cast<std::int64_t>(q), scale);
}

Decimal Decimal::operator-() const {
    if (mMantissa == kI64Min) {
        throw std::overflow_error("Decimal overflow");
    }
    return Decimal(-mMantissa, mScale);
}

std::string Decimal::ToString() const {
    const bool neg = mMantissa < 0;
    // Work from the unsigned magnitude so INT64_MIN does not overflow.
    const std::uint64_t mag = neg ? ~static_cast<std::uint64_t>(mMantissa) + 1
                                  : static_cast<std::uint64_t>(mMantissa);
    std::string digits = std::to_string(mag);
    if (mScale > 0) {
        if (digits.size() <= static_cast<std::size_t>(mScale)) {
            digits.insert(
                0, static_cast<std::size_t>(mScale) - digits.size() + 1, '0');
        }
        digits.insert(digits.size() - static_cast<std::size_t>(mScale), ".");
    }
    return neg ? "-" + digits : digits;
}

Decimal Decimal::Normalized() const {
    std::int64_t m = mMantissa;
    int s = mScale;
    while (s > 0 && m % 10 == 0) {
        m /= 10;
        --s;
    }
    return Decimal(m, s);
}

Decimal Decimal::Rescale(int new_scale, Rounding mode) const {
    if (new_scale < 0 || new_scale > kMaxScale) {
        throw std::invalid_argument("Decimal scale out of range");
    }
    if (new_scale == mScale) {
        return *this;
    }
    if (new_scale > mScale) {
        const i128 w = Widen(*this, new_scale);
        if (!FitsI64(w)) {
            throw std::overflow_error("Decimal overflow");
        }
        return Decimal(static_cast<std::int64_t>(w), new_scale);
    }
    const i128 q = DivRound(mMantissa, Pow10(mScale - new_scale), mode);
    return Decimal(static_cast<std::int64_t>(q), new_scale);
}

Decimal Decimal::RoundToIncrement(const Decimal& increment,
                                  Rounding mode) const {
    if (increment.mMantissa <= 0) {
        throw std::invalid_argument("rounding increment must be positive");
    }
    const int scale = std::max(mScale, increment.mScale);
    const i128 v = Widen(*this, scale);
    const i128 inc = Widen(increment, scale);
    const i128 multiples = DivRound(v, inc, mode);
    const i128 result = multiples * inc;
    if (!FitsI64(result)) {
        throw std::overflow_error("Decimal overflow");
    }
    return Decimal(static_cast<std::int64_t>(result), scale);
}

Decimal operator+(const Decimal& a, const Decimal& b) {
    const int scale = std::max(a.mScale, b.mScale);
    return FromWide(Widen(a, scale) + Widen(b, scale), scale);
}

Decimal operator-(const Decimal& a, const Decimal& b) {
    const int scale = std::max(a.mScale, b.mScale);
    return FromWide(Widen(a, scale) - Widen(b, scale), scale);
}

Decimal operator*(const Decimal& a, const Decimal& b) {
    return FromWide(static_cast<i128>(a.mMantissa) * b.mMantissa,
                    a.mScale + b.mScale);
}

bool operator==(const Decimal& a, const Decimal& b) {
    return (a <=> b) == std::strong_ordering::equal;
}

std::strong_ordering operator<=>(const Decimal& a, const Decimal& b) {
    const int scale = std::max(a.mScale, b.mScale);
    const i128 x = Widen(a, scale);
    const i128 y = Widen(b, scale);
    return x < y   ? std::strong_ordering::less
           : x > y ? std::strong_ordering::greater
                   : std::strong_ordering::equal;
}

std::ostream& operator<<(std::ostream& os, const Decimal& d) {
    return os << d.ToString();
}

}  // namespace kestrel::types
