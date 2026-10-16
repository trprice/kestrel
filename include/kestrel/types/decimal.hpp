#pragma once

#include <compare>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

#include "kestrel/types/error.hpp"

namespace kestrel::types {

// Direction for rounding to a given increment.
//   kUp      toward +infinity (ceiling)
//   kDown    toward -infinity (floor)
//   kNearest to the nearest multiple; exact ties go away from zero
enum class Rounding { kUp, kDown, kNearest };

// Fixed-point decimal: value = mantissa / 10^scale, scale in [0, 18].
// Equality and ordering compare numeric value, so 1.50 == 1.5.
//
// Arithmetic is exact where the result fits an int64 mantissa. When a product
// needs more digits than fit, the scale is reduced (rounding to nearest) rather
// than failing; only a result whose integer part cannot be represented throws
// std::overflow_error, which is a programming error rather than a runtime
// condition.
class Decimal {
 public:
  static constexpr int kMaxScale = 18;

  Decimal() = default;
  // Throws std::invalid_argument if scale is outside [0, kMaxScale].
  Decimal(std::int64_t mantissa, int scale);

  static Decimal FromInt(std::int64_t value) { return Decimal(value, 0); }

  // Accepts [+-]digits[.digits][(e|E)[+-]digits]. Digits beyond kMaxScale
  // fractional places are rounded to nearest.
  static Result<Decimal> Parse(std::string_view text);

  std::int64_t Mantissa() const { return mMantissa; }
  int Scale() const { return mScale; }

  bool IsZero() const { return mMantissa == 0; }
  bool IsNegative() const { return mMantissa < 0; }
  int Sign() const { return (mMantissa > 0) - (mMantissa < 0); }
  Decimal Abs() const;

  // "123.4500" at scale 4. Always plain notation.
  std::string ToString() const;
  // Same value with trailing fractional zeros removed ("123.45").
  Decimal Normalized() const;

  // Change scale. Narrowing rounds in the given direction; widening throws
  // std::overflow_error if the mantissa would not fit.
  Decimal Rescale(int new_scale, Rounding mode) const;

  // Round to a multiple of `increment` (which must be > 0, else
  // std::invalid_argument). The result keeps max(scale, increment.scale).
  Decimal RoundToIncrement(const Decimal& increment, Rounding mode) const;

  // a / b computed to `scale` fractional digits, rounded in the given
  // direction. Throws std::domain_error on division by zero.
  static Decimal Divide(const Decimal& a, const Decimal& b, int scale, Rounding mode);

  Decimal operator-() const;
  friend Decimal operator+(const Decimal& a, const Decimal& b);
  friend Decimal operator-(const Decimal& a, const Decimal& b);
  friend Decimal operator*(const Decimal& a, const Decimal& b);
  Decimal& operator+=(const Decimal& o) { return *this = *this + o; }
  Decimal& operator-=(const Decimal& o) { return *this = *this - o; }

  friend bool operator==(const Decimal& a, const Decimal& b);
  friend std::strong_ordering operator<=>(const Decimal& a, const Decimal& b);

 private:
  std::int64_t mMantissa = 0;
  int mScale = 0;
};

std::ostream& operator<<(std::ostream& os, const Decimal& d);

}  // namespace kestrel::types
