#include "kestrel/types/market_hours.hpp"

#include <algorithm>
#include <array>

namespace kestrel::types {

namespace {

using namespace std::chrono;

const time_zone* NewYork() {
    static const time_zone* const zone = locate_zone("America/New_York");
    return zone;
}

// The n-th (1-based) `wd` of a month, or with n = 0 the last one.
year_month_day Nth(year y, month m, weekday wd, unsigned n) {
    if (n == 0) {
        return year_month_day{sys_days{y / m / wd[last]}};
    }
    return year_month_day{sys_days{y / m / wd[n]}};
}

// Easter Sunday (Gregorian), by the anonymous algorithm (Meeus).
year_month_day Easter(year y) {
    const int Y = static_cast<int>(y);
    const int a = Y % 19;
    const int b = Y / 100;
    const int c = Y % 100;
    const int d = b / 4;
    const int e = b % 4;
    const int f = (b + 8) / 25;
    const int g = (b - f + 1) / 3;
    const int h = (19 * a + b - d - g + 15) % 30;
    const int i = c / 4;
    const int k = c % 4;
    const int l = (32 + 2 * e + 2 * i - h - k) % 7;
    const int m = (a + 11 * h + 22 * l) / 451;
    const int month_number = (h + l - 7 * m + 114) / 31;
    const int day_number = ((h + l - 7 * m + 114) % 31) + 1;
    return y / month(static_cast<unsigned>(month_number)) /
           day(static_cast<unsigned>(day_number));
}

// A fixed-date holiday moved off the weekend: Saturday to the Friday before,
// Sunday to the Monday after.
year_month_day Observed(year_month_day date) {
    const sys_days d{date};
    const weekday wd{d};
    if (wd == Saturday) {
        return year_month_day{d - days(1)};
    }
    if (wd == Sunday) {
        return year_month_day{d + days(1)};
    }
    return date;
}

// Unscheduled full-day closures of NYSE since 2001.
constexpr std::array kSpecialClosures = {
    2001y / September / 11, 2001y / September / 12, 2001y / September / 13,
    2001y / September / 14, 2004y / June / 11,      2007y / January / 2,
    2012y / October / 29,   2012y / October / 30,   2018y / December / 5,
    2025y / January / 9,
};

// New York local time `hh:mm` on `date`, as an instant.
Timestamp At(year_month_day date, int hh, int mm) {
    const local_days day{date};
    const auto local = day + hours(hh) + minutes(mm);
    return time_point_cast<Timestamp::duration>(
        NewYork()->to_sys(local, choose::earliest));
}

}  // namespace

bool UsEquityCalendar::IsHoliday(year_month_day date) {
    const weekday wd{sys_days{date}};
    if (wd == Saturday || wd == Sunday) {
        return false;  // not a holiday: closed anyway
    }
    if (std::ranges::find(kSpecialClosures, date) != kSpecialClosures.end()) {
        return true;
    }
    const year y = date.year();
    // New Year's Day: a Sunday moves to Monday; a Saturday is not made up
    // (the Friday before is in the old year and trades).
    const year_month_day new_year = y / January / 1;
    if (date == new_year ||
        (weekday{sys_days{new_year}} == Sunday && date == y / January / 2)) {
        return true;
    }
    if (date == Nth(y, January, Monday, 3) ||     // Martin Luther King Jr.
        date == Nth(y, February, Monday, 3) ||    // Washington's Birthday
        date == Nth(y, May, Monday, 0) ||         // Memorial Day
        date == Nth(y, September, Monday, 1) ||   // Labor Day
        date == Nth(y, November, Thursday, 4)) {  // Thanksgiving
        return true;
    }
    if (date == year_month_day{sys_days{Easter(y)} - days(2)}) {
        return true;  // Good Friday
    }
    if (y >= 2022y && date == Observed(y / June / 19)) {
        return true;  // Juneteenth
    }
    return date == Observed(y / July / 4) ||
           date == Observed(y / December / 25);
}

bool UsEquityCalendar::IsEarlyClose(year_month_day date) {
    const weekday wd{sys_days{date}};
    if (wd == Saturday || wd == Sunday || IsHoliday(date)) {
        return false;
    }
    const year y = date.year();
    const year_month_day black_friday{sys_days{Nth(y, November, Thursday, 4)} +
                                      days(1)};
    return date == black_friday || date == y / July / 3 ||
           date == y / December / 24;
}

TradingDay UsEquityCalendar::Hours(year_month_day date, bool early_close) {
    return TradingDay{At(date, 7, 0), At(date, 9, 30),
                      At(date, early_close ? 13 : 16, 0),
                      At(date, early_close ? 17 : 20, 0)};
}

std::optional<TradingDay> UsEquityCalendar::On(year_month_day date) const {
    const weekday wd{sys_days{date}};
    if (wd == Saturday || wd == Sunday || IsHoliday(date)) {
        return std::nullopt;
    }
    return Hours(date, IsEarlyClose(date));
}

year_month_day UsEquityCalendar::DateAt(Timestamp t) const {
    const auto local = NewYork()->to_local(floor<seconds>(t));
    return year_month_day{floor<days>(local)};
}

std::shared_ptr<const MarketHours> MarketHours::AlwaysOpen() {
    static const auto always =
        std::make_shared<const MarketHours>(nullptr, false);
    return always;
}

MarketHours::MarketHours(std::shared_ptr<const TradingCalendar> calendar,
                         bool extended)
    : mCalendar(std::move(calendar)), mExtended(extended) {}

std::optional<MarketHours::Session> MarketHours::SessionOf(
    year_month_day date) const {
    const auto day = mCalendar->On(date);
    if (!day) {
        return std::nullopt;
    }
    return mExtended ? Session{day->pre_open, day->post_close}
                     : Session{day->open, day->close};
}

bool MarketHours::IsOpen(Timestamp t) const {
    if (!mCalendar) {
        return true;
    }
    // Sessions never cross midnight, so the session of `t`'s own date is the
    // only one that can contain it.
    const auto session = SessionOf(mCalendar->DateAt(t));
    return session && t >= session->start && t < session->end;
}

std::optional<MarketHours::Session> MarketHours::CurrentOrNext(
    Timestamp t) const {
    if (!mCalendar) {
        return std::nullopt;
    }
    const sys_days first{mCalendar->DateAt(t)};
    for (int i = 0; i <= 366; ++i) {
        const auto session = SessionOf(year_month_day{first + days(i)});
        if (session && t < session->end) {
            return session;
        }
    }
    return std::nullopt;
}

bool MarketHours::AnyOpen(Timestamp from, Timestamp to) const {
    if (from >= to) {
        return false;
    }
    if (!mCalendar) {
        return true;
    }
    const auto session = CurrentOrNext(from);
    return session && session->start < to;
}

}  // namespace kestrel::types
