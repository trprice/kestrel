#pragma once

#include <chrono>
#include <memory>
#include <optional>

#include "kestrel/types/time.hpp"

namespace kestrel::types {

// One trading day's sessions. Times are instants (UTC); extended trading runs
// from pre_open to open and from close to post_close.
struct TradingDay {
    Timestamp pre_open;
    Timestamp open;
    Timestamp close;
    Timestamp post_close;

    bool operator==(const TradingDay&) const = default;
};

// Which days a market trades and when. Dates are the market's own (New York
// dates for US equities).
class TradingCalendar {
   public:
    virtual ~TradingCalendar() = default;
    TradingCalendar() = default;
    // An interface: implementations are held by pointer, never copied.
    TradingCalendar(const TradingCalendar&) = delete;
    TradingCalendar& operator=(const TradingCalendar&) = delete;

    // The day's sessions, or nullopt when the market is closed all day.
    virtual std::optional<TradingDay> On(
        std::chrono::year_month_day date) const = 0;
    // The market's date at `t` (its local calendar date).
    virtual std::chrono::year_month_day DateAt(Timestamp t) const = 0;
};

// The US equity market (NYSE and Nasdaq) from its rules: closed on weekends
// and on New Year's Day, Martin Luther King Jr. Day, Washington's Birthday,
// Good Friday, Memorial Day, Juneteenth (from 2022), Independence Day, Labor
// Day, Thanksgiving and Christmas, each moved to the Friday before or the
// Monday after when it falls on a weekend (except New Year's Day on a
// Saturday, which is not made up); and on the unscheduled closures since 2001
// (national days of mourning, Sept 11, Hurricane Sandy). The regular session
// is 09:30 to 16:00 New York time, closing at 13:00 on the day after
// Thanksgiving and, when they are trading days, on July 3 and Christmas Eve.
// Extended hours are Schwab's: 07:00 to 09:30, and from the close to 20:00
// (17:00 on an early close). Closures announced at short notice are not known;
// Schwab's calendar (SchwabDataSource) checks upcoming days against its
// /markets.
class UsEquityCalendar : public TradingCalendar {
   public:
    std::optional<TradingDay> On(
        std::chrono::year_month_day date) const override;
    std::chrono::year_month_day DateAt(Timestamp t) const override;

    // The rules alone: whether `date` is a full-day holiday, and whether the
    // regular session closes at 13:00.
    static bool IsHoliday(std::chrono::year_month_day date);
    static bool IsEarlyClose(std::chrono::year_month_day date);
    // The day's sessions by the rules (not checking IsHoliday).
    static TradingDay Hours(std::chrono::year_month_day date, bool early_close);
};

// When a market can be traded: a calendar and which of its sessions count
// (the regular one, or the extended hours around it too). A venue that never
// closes, such as a crypto exchange, has no calendar and is always open.
class MarketHours {
   public:
    static std::shared_ptr<const MarketHours> AlwaysOpen();
    MarketHours(std::shared_ptr<const TradingCalendar> calendar, bool extended);

    bool IsAlwaysOpen() const { return !mCalendar; }
    bool Extended() const { return mExtended; }
    bool IsOpen(Timestamp t) const;

    // The session open at `t`, or else the next one to open after it, as
    // [start, end). Nullopt when always open, or when nothing opens within a
    // year (it never happens for a real calendar).
    struct Session {
        Timestamp start;
        Timestamp end;
    };
    std::optional<Session> CurrentOrNext(Timestamp t) const;

    // Whether any time in [from, to) is open.
    bool AnyOpen(Timestamp from, Timestamp to) const;
    // How much of [from, to) is open: all of it on a market that never
    // closes.
    Timestamp::duration OpenDuration(Timestamp from, Timestamp to) const;

   private:
    // The counted session of one day, if it trades.
    std::optional<Session> SessionOf(std::chrono::year_month_day date) const;

    std::shared_ptr<const TradingCalendar> mCalendar;  // null: always open
    bool mExtended = false;
};

}  // namespace kestrel::types
