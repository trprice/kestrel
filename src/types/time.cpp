#include "kestrel/types/time.hpp"

#include <charconv>
#include <cstdio>

namespace kestrel::types {

Timestamp Now() {
  return std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

namespace {

bool ReadInt(std::string_view s, std::size_t& pos, std::size_t width, int& out) {
  if (pos + width > s.size()) return false;
  const auto [ptr, ec] = std::from_chars(s.data() + pos, s.data() + pos + width, out);
  if (ec != std::errc() || ptr != s.data() + pos + width) return false;
  pos += width;
  return true;
}

bool Expect(std::string_view s, std::size_t& pos, char c) {
  if (pos >= s.size() || s[pos] != c) return false;
  ++pos;
  return true;
}

}  // namespace

Result<Timestamp> ParseIso8601(std::string_view text) {
  auto bad = [&] {
    return MakeError(ErrorCategory::kParse, "invalid ISO-8601 timestamp '" + std::string(text) + "'");
  };
  std::size_t pos = 0;
  int y, mo, d, h, mi, sec;
  if (!ReadInt(text, pos, 4, y) || !Expect(text, pos, '-') || !ReadInt(text, pos, 2, mo) ||
      !Expect(text, pos, '-') || !ReadInt(text, pos, 2, d)) {
    return bad();
  }
  if (pos < text.size() && (text[pos] == 'T' || text[pos] == ' ')) ++pos;
  else return bad();
  if (!ReadInt(text, pos, 2, h) || !Expect(text, pos, ':') || !ReadInt(text, pos, 2, mi) ||
      !Expect(text, pos, ':') || !ReadInt(text, pos, 2, sec)) {
    return bad();
  }
  std::int64_t millis = 0;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    int digits = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      if (digits < 3) millis = millis * 10 + (text[pos] - '0');
      ++digits;
      ++pos;
    }
    if (digits == 0) return bad();
    for (int i = digits; i < 3; ++i) millis *= 10;
  }
  const std::string_view rest = text.substr(pos);
  if (rest != "Z" && rest != "+00:00" && rest != "+0000") return bad();

  using namespace std::chrono;
  const year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)}, day{static_cast<unsigned>(d)}};
  if (!ymd.ok() || h > 23 || mi > 59 || sec > 60) return bad();
  const sys_days days{ymd};
  return Timestamp(duration_cast<milliseconds>(days.time_since_epoch()) + hours(h) + minutes(mi) +
                   seconds(sec) + milliseconds(millis));
}

std::string FormatIso8601(Timestamp t) {
  using namespace std::chrono;
  const auto days = floor<std::chrono::days>(t);
  const year_month_day ymd{days};
  const auto tod = t - days;
  const auto h = duration_cast<hours>(tod);
  const auto m = duration_cast<minutes>(tod - h);
  const auto s = duration_cast<seconds>(tod - h - m);
  const auto ms = duration_cast<milliseconds>(tod - h - m - s);
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                static_cast<int>(h.count()), static_cast<int>(m.count()),
                static_cast<int>(s.count()), static_cast<int>(ms.count()));
  return buf;
}

}  // namespace kestrel::types
