#pragma once

#include <QDate>
#include <QHash>
#include <QList>
#include <QString>

#include <array>
#include <optional>
#include <vector>

#include "core/Error.hpp"
#include "core/Model.hpp"

// Working time (SPEC.md §6.1; scheduling plan S1). Durations and lags are
// counted in working days of a task's calendar (task → project → Standard).
// Pure Qt Core, deterministic, shared by the server and the desktop client.
namespace zproject {

// Monday..Sunday (index = QDate::dayOfWeek() - 1).
struct WorkWeek {
  std::array<bool, 7> working{true, true, true, true, true, false, false};
  std::array<double, 7> hours{8, 8, 8, 8, 8, 0, 0};
  bool operator==(const WorkWeek&) const = default;
};

// One exception: `holiday` and `non_working` make every day in [from, to]
// non-working; `custom_hours` changes the hours of the days that are
// working anyway (for work and cost), never whether a day counts.
struct CalendarException : EntityBase {
  QString calendar;  // the calendar it belongs to (stored rows)
  QDate from;
  QDate to;
  QString kind = QStringLiteral("holiday");  // holiday, non_working, custom_hours
  double hours = 0;                          // custom_hours only
  QString label;
};

// A calendar as stored (SPEC.md §5.1). `workWeek` empty: the base's (or,
// at the root, Standard's). Exceptions add to the base's; where both cover
// a day, this calendar's own exception wins.
struct CalendarDef : EntityBase {
  QString name;
  QString base;  // empty = none
  std::optional<WorkWeek> workWeek;
  double hoursPerDay = 8;
  QList<CalendarException> exceptions;  // live ones; not part of the calendar's own JSON
};

// The Standard calendar every installation has (seeded by schema 4): a
// project with no calendar uses it, so holidays added to it apply
// everywhere. Monday–Friday, 8 hours, until someone changes it.
inline constexpr QLatin1String kStandardCalendarId("00000000000000000001");

// JSON with the API's field names. A work week is an array of seven
// {"working": bool, "hours": number}, Monday first, or null (inherited).
// Dates are YYYY-MM-DD. fromJson ignores unknown keys; a malformed work
// week or date decodes to something validate*() refuses.
QJsonObject toJson(const CalendarDef& c);
CalendarDef calendarFromJson(const QJsonObject& o);
QJsonObject toJson(const CalendarException& e);
CalendarException calendarExceptionFromJson(const QJsonObject& o);

inline constexpr int kMaxExceptionDays = 366;  // one exception spans at most a year
inline constexpr int kMaxCalendarDepth = 8;    // base chains deeper than this are refused

QList<FieldError> validateCalendar(const CalendarDef& c);
QList<FieldError> validateCalendarException(const CalendarException& e);

// A resolved calendar: its work week, hours and every exception of its
// base chain, answering working-day questions. A default-constructed one is
// Standard (Monday–Friday, 8 hours).
//
// Day arithmetic counts whole days. addWorkingDays(d, n) is the n-th
// working day after d (n > 0) or before it (n < 0), not counting d itself;
// workingDaysBetween(a, b) counts the working days in [a, b). The scheduler
// builds its conventions (a 3-day task starting Monday finishes Wednesday)
// on these.
//
// prepare(from, to) precomputes a bitmap with prefix counts over that range,
// so queries inside it are O(1) or O(log n) (SPEC.md §17: a 2,000-task
// reschedule in ≤ 50 ms). Outside it, the same answers come from a
// day-by-day walk. A search that finds no working day within
// kSearchLimitDays returns an invalid QDate.
class WorkCalendar {
 public:
  static constexpr int kSearchLimitDays = 3660;  // ten years

  WorkCalendar() = default;
  // `id` and every base it inherits from, from `defs`. Empty id: Standard.
  // Errors: an unknown id or base (not_found), a base cycle or a chain
  // deeper than kMaxCalendarDepth (validation).
  static Result<WorkCalendar> resolve(const QString& id, const QHash<QString, CalendarDef>& defs);

  QString id() const { return id_; }
  const WorkWeek& workWeek() const { return week_; }
  double hoursPerDay() const { return hoursPerDay_; }

  bool isWorking(const QDate& d) const;
  double hoursOn(const QDate& d) const;  // 0 on a non-working day
  // d itself when it is working, else the next (previous) working day.
  QDate nextWorking(const QDate& d) const;
  QDate previousWorking(const QDate& d) const;
  QDate addWorkingDays(const QDate& d, int n) const;
  // Working days in [a, b); negative (minus those in [b, a)) when b < a.
  qint64 workingDaysBetween(const QDate& a, const QDate& b) const;

  void prepare(const QDate& from, const QDate& to);
  bool prepared(const QDate& d) const;

 private:
  struct Override {
    std::optional<bool> working;
    std::optional<double> hours;
  };
  bool computeWorking(qint64 jd) const;
  QDate walk(const QDate& d, int n) const;

  QString id_;
  WorkWeek week_;
  double hoursPerDay_ = 8;
  QHash<qint64, Override> overrides_;  // by Julian day
  qint64 lo_ = 0;                      // Julian day of prefix_[0]
  std::vector<qint32> prefix_;         // prefix_[i] = working days in [lo_, lo_ + i)
};

}  // namespace zproject
