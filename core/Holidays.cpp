#include "core/Holidays.hpp"

#include <algorithm>

namespace zproject {

QDate nthWeekday(int year, int month, int weekday, int n) {
  const QDate first(year, month, 1);
  if (!first.isValid() || weekday < 1 || weekday > 7 || n == 0) return {};
  if (n > 0) {
    const int offset = (weekday - first.dayOfWeek() + 7) % 7;
    const QDate d = first.addDays(offset + 7 * (n - 1));
    return d.month() == month ? d : QDate();
  }
  const QDate last(year, month, first.daysInMonth());
  const int back = (last.dayOfWeek() - weekday + 7) % 7;
  const QDate d = last.addDays(-back - 7 * (-n - 1));
  return d.month() == month ? d : QDate();
}

namespace {

struct Holiday {
  QDate date;  // the actual date
  QString name;
  bool fixed;  // a fixed date that moves when it falls on a weekend
};

QList<Holiday> holidaysOf(int year) {
  QList<Holiday> h = {
      {QDate(year, 1, 1), QStringLiteral("New Year's Day"), true},
      {nthWeekday(year, 1, 1, 3), QStringLiteral("Birthday of Martin Luther King, Jr."), false},
      {nthWeekday(year, 2, 1, 3), QStringLiteral("Washington's Birthday"), false},
      {nthWeekday(year, 5, 1, -1), QStringLiteral("Memorial Day"), false},
      {QDate(year, 7, 4), QStringLiteral("Independence Day"), true},
      {nthWeekday(year, 9, 1, 1), QStringLiteral("Labor Day"), false},
      {nthWeekday(year, 10, 1, 2), QStringLiteral("Columbus Day"), false},
      {QDate(year, 11, 11), QStringLiteral("Veterans Day"), true},
      {nthWeekday(year, 11, 4, 4), QStringLiteral("Thanksgiving Day"), false},
      {QDate(year, 12, 25), QStringLiteral("Christmas Day"), true},
  };
  if (year >= 2021) h.append({QDate(year, 6, 19), QStringLiteral("Juneteenth National Independence Day"), true});
  return h;
}

}  // namespace

QList<CalendarException> usFederalHolidays(int year) {
  QList<CalendarException> out;
  if (year < 1971 || year > 9998) return out;
  // This year's holidays, and next year's New Year's Day (it can be observed
  // on 31 December).
  QList<Holiday> all = holidaysOf(year);
  all.append({QDate(year + 1, 1, 1), QStringLiteral("New Year's Day"), true});
  for (const Holiday& h : all) {
    QDate observed = h.date;
    if (h.fixed && observed.dayOfWeek() == 6) observed = observed.addDays(-1);
    if (h.fixed && observed.dayOfWeek() == 7) observed = observed.addDays(1);
    if (observed.year() != year) continue;
    CalendarException e;
    e.from = observed;
    e.to = observed;
    e.kind = QStringLiteral("holiday");
    e.label = observed == h.date ? h.name : h.name + QStringLiteral(" (observed)");
    out.append(e);
  }
  std::sort(out.begin(), out.end(), [](const CalendarException& a, const CalendarException& b) { return a.from < b.from; });
  return out;
}

}  // namespace zproject
