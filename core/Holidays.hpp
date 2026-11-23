#pragma once

#include <QList>

#include "core/WorkCalendar.hpp"

// Holidays for "Import holidays" (SPEC.md §6.1, decision 14; scheduling plan
// S1, Q10): computed here, never fetched, so the import works offline and
// the same year always gives the same dates.
namespace zproject {

// The US federal holidays (5 U.S.C. 6103) whose observed day falls in
// `year`, in date order, as `holiday` exceptions of one day each, labelled
// with the holiday's name ("Independence Day (observed)" when moved).
//
// A holiday on a Saturday is observed the Friday before, on a Sunday the
// Monday after, as OPM publishes them. So New Year's Day of year + 1 can
// land on 31 December of `year` (2027 has twelve entries), and that year's
// own 1 January is then not in its list. Juneteenth counts from 2021.
// Years before 1971 (the current Monday rules) are refused: empty list.
QList<CalendarException> usFederalHolidays(int year);

// The n-th (1-based) given weekday (1 = Monday … 7 = Sunday) of a month;
// n = -1 is the last. Invalid when there is no such day.
QDate nthWeekday(int year, int month, int weekday, int n);

}  // namespace zproject
