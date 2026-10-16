#include <limits>
#include <sstream>

#include "test_helpers.hpp"

namespace {

using kestrel::testing::D;
using kestrel::types::Decimal;
using kestrel::types::Rounding;

TEST(DecimalParse, FormatsWithItsScale) {
  EXPECT_EQ(D("123.4500").ToString(), "123.4500");
  EXPECT_EQ(D("-0.05").ToString(), "-0.05");
  EXPECT_EQ(D("0").ToString(), "0");
  EXPECT_EQ(D(".5").ToString(), "0.5");
  EXPECT_EQ(D("+7.").ToString(), "7");
  EXPECT_EQ(D("0.00000001").ToString(), "0.00000001");
  EXPECT_EQ(D("1e-8").ToString(), "0.00000001");
  EXPECT_EQ(D("1.5E3").ToString(), "1500");
  EXPECT_EQ(D("-12.50").Normalized().ToString(), "-12.5");
  EXPECT_EQ(D("100").Normalized().ToString(), "100");
}

TEST(DecimalParse, RoundsExcessFractionDigits) {
  EXPECT_EQ(D("0.1234567890123456789").ToString(), "0.123456789012345679");
}

TEST(DecimalParse, RejectsGarbage) {
  for (const char* bad : {"", ".", "-", "abc", "1.2.3", "1,5", " 1", "1 ", "1e", "1e+", "--1", "0x10",
                          "99999999999999999999"}) {
    EXPECT_FALSE(Decimal::Parse(bad).has_value()) << "'" << bad << "'";
  }
}

TEST(DecimalParse, Int64Extremes) {
  EXPECT_EQ(D("9223372036854775807").ToString(), "9223372036854775807");
  EXPECT_EQ(D("-9223372036854775808").ToString(), "-9223372036854775808");
}

TEST(DecimalCompare, IgnoresScale) {
  EXPECT_EQ(D("1.50"), D("1.5"));
  EXPECT_EQ(D("0.00"), D("0"));
  EXPECT_LT(D("1.49999999"), D("1.5"));
  EXPECT_GT(D("-1.4"), D("-1.5"));
  EXPECT_LE(D("2"), D("2.000"));
}

TEST(DecimalArithmetic, AddSubtractMultiply) {
  EXPECT_EQ((D("0.1") + D("0.2")).ToString(), "0.3");  // exact, unlike binary floating point
  EXPECT_EQ((D("1.00") - D("0.005")).ToString(), "0.995");
  EXPECT_EQ((D("50025.0") * D("0.1")).ToString(), "5002.50");
  EXPECT_EQ((D("-2.5") * D("4")).ToString(), "-10.0");
  EXPECT_EQ((-D("3.2")).ToString(), "-3.2");
  EXPECT_EQ(D("-3.2").Abs(), D("3.2"));
}

TEST(DecimalArithmetic, MultiplyReducesScaleInsteadOfOverflowing) {
  // 18 + 18 = 36 fractional digits do not fit; the product is rounded to fit.
  const Decimal tiny(123456789012345678, 18);
  const Decimal product = tiny * tiny;
  EXPECT_LE(product.Scale(), Decimal::kMaxScale);
  EXPECT_EQ(product.ToString().substr(0, 6), "0.0152");
}

TEST(DecimalArithmetic, OverflowThrows) {
  const Decimal big(std::numeric_limits<std::int64_t>::max(), 0);
  EXPECT_THROW(big + big, std::overflow_error);
  EXPECT_THROW(big * Decimal(2, 0), std::overflow_error);
}

TEST(DecimalDivide, RoundsInRequestedDirection) {
  EXPECT_EQ(Decimal::Divide(D("1"), D("3"), 4, Rounding::kDown).ToString(), "0.3333");
  EXPECT_EQ(Decimal::Divide(D("1"), D("3"), 4, Rounding::kUp).ToString(), "0.3334");
  EXPECT_EQ(Decimal::Divide(D("2"), D("3"), 4, Rounding::kNearest).ToString(), "0.6667");
  EXPECT_EQ(Decimal::Divide(D("-1"), D("3"), 4, Rounding::kDown).ToString(), "-0.3334");
  EXPECT_EQ(Decimal::Divide(D("5002.50"), D("0.1"), 2, Rounding::kNearest).ToString(), "50025.00");
  EXPECT_EQ(Decimal::Divide(D("1"), D("-4"), 2, Rounding::kNearest).ToString(), "-0.25");
  EXPECT_THROW(Decimal::Divide(D("1"), D("0"), 2, Rounding::kDown), std::domain_error);
}

struct RoundCase {
  const char* value;
  const char* increment;
  Rounding mode;
  const char* expected;
};

class DecimalRoundToIncrement : public ::testing::TestWithParam<RoundCase> {};

TEST_P(DecimalRoundToIncrement, Rounds) {
  const auto& c = GetParam();
  EXPECT_EQ(D(c.value).RoundToIncrement(D(c.increment), c.mode), D(c.expected))
      << c.value << " to " << c.increment;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, DecimalRoundToIncrement,
    ::testing::Values(
        // Down = toward -infinity
        RoundCase{"49000.07", "0.1", Rounding::kDown, "49000.0"},
        RoundCase{"49000.10", "0.1", Rounding::kDown, "49000.1"},  // already on tick
        RoundCase{"0.123456789", "0.00000001", Rounding::kDown, "0.12345678"},
        RoundCase{"-0.07", "0.1", Rounding::kDown, "-0.1"},
        RoundCase{"0.04", "0.1", Rounding::kDown, "0"},
        // Up = toward +infinity
        RoundCase{"49000.01", "0.1", Rounding::kUp, "49000.1"},
        RoundCase{"49000.10", "0.1", Rounding::kUp, "49000.1"},
        RoundCase{"-0.07", "0.1", Rounding::kUp, "0"},
        RoundCase{"0.001", "0.01", Rounding::kUp, "0.01"},
        // Nearest, ties away from zero
        RoundCase{"1.24", "0.5", Rounding::kNearest, "1.0"},
        RoundCase{"1.26", "0.5", Rounding::kNearest, "1.5"},
        RoundCase{"0.25", "0.5", Rounding::kNearest, "0.5"},   // tie -> away from zero
        RoundCase{"-0.25", "0.5", Rounding::kNearest, "-0.5"},
        RoundCase{"0.15", "0.1", Rounding::kNearest, "0.2"},
        RoundCase{"0.149", "0.1", Rounding::kNearest, "0.1"},
        // Increments that are not powers of ten
        RoundCase{"10.03", "0.05", Rounding::kDown, "10.00"},
        RoundCase{"10.03", "0.05", Rounding::kUp, "10.05"},
        RoundCase{"10.03", "0.05", Rounding::kNearest, "10.05"},
        RoundCase{"10.02", "0.05", Rounding::kNearest, "10.00"},
        RoundCase{"17", "5", Rounding::kDown, "15"},
        RoundCase{"17", "5", Rounding::kUp, "20"},
        RoundCase{"17", "5", Rounding::kNearest, "15"},
        RoundCase{"0", "0.1", Rounding::kUp, "0"}));

TEST(DecimalRound, RejectsNonPositiveIncrement) {
  EXPECT_THROW(D("1").RoundToIncrement(D("0"), Rounding::kDown), std::invalid_argument);
  EXPECT_THROW(D("1").RoundToIncrement(D("-0.1"), Rounding::kDown), std::invalid_argument);
}

TEST(DecimalRescale, NarrowAndWiden) {
  EXPECT_EQ(D("1.2345").Rescale(2, Rounding::kDown).ToString(), "1.23");
  EXPECT_EQ(D("1.2345").Rescale(2, Rounding::kUp).ToString(), "1.24");
  EXPECT_EQ(D("1.235").Rescale(2, Rounding::kNearest).ToString(), "1.24");
  EXPECT_EQ(D("-1.2345").Rescale(2, Rounding::kDown).ToString(), "-1.24");
  EXPECT_EQ(D("1.5").Rescale(4, Rounding::kDown).ToString(), "1.5000");
}

TEST(DecimalStream, PrintsValue) {
  std::ostringstream os;
  os << D("3.140");
  EXPECT_EQ(os.str(), "3.140");
}

}  // namespace
