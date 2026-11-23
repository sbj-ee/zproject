#include "core/WorkCalendar.hpp"

#include <QJsonArray>
#include <QSet>

#include <algorithm>

namespace zproject {

namespace {

bool knownKind(const QString& k) {
  return k == QLatin1String("holiday") || k == QLatin1String("non_working") || k == QLatin1String("custom_hours");
}

int dayIndex(const QDate& d) { return d.dayOfWeek() - 1; }

}  // namespace

QList<FieldError> validateCalendarException(const CalendarException& e) {
  QList<FieldError> errs;
  if (!e.from.isValid()) errs.append({QStringLiteral("from"), QStringLiteral("must be a date (YYYY-MM-DD)")});
  if (!e.to.isValid()) errs.append({QStringLiteral("to"), QStringLiteral("must be a date (YYYY-MM-DD)")});
  if (e.from.isValid() && e.to.isValid()) {
    if (e.to < e.from) {
      errs.append({QStringLiteral("to"), QStringLiteral("must not be before from")});
    } else if (e.from.daysTo(e.to) >= kMaxExceptionDays) {
      errs.append({QStringLiteral("to"), QStringLiteral("an exception spans at most %1 days").arg(kMaxExceptionDays)});
    }
  }
  if (!knownKind(e.kind)) {
    errs.append({QStringLiteral("kind"), QStringLiteral("must be holiday, non_working or custom_hours")});
  }
  if (e.kind == QLatin1String("custom_hours") && (e.hours <= 0 || e.hours > 24)) {
    errs.append({QStringLiteral("hours"), QStringLiteral("must be more than 0 and at most 24")});
  }
  if (e.kind != QLatin1String("custom_hours") && e.hours != 0) {
    errs.append({QStringLiteral("hours"), QStringLiteral("only custom_hours exceptions have hours")});
  }
  if (e.label.size() > 200) errs.append({QStringLiteral("label"), QStringLiteral("must be at most 200 characters")});
  return errs;
}

QList<FieldError> validateCalendar(const CalendarDef& c) {
  QList<FieldError> errs;
  if (c.name.trimmed().isEmpty()) errs.append({QStringLiteral("name"), QStringLiteral("is required")});
  if (c.name.size() > 200) errs.append({QStringLiteral("name"), QStringLiteral("must be at most 200 characters")});
  if (!c.id.isEmpty() && c.base == c.id) errs.append({QStringLiteral("base"), QStringLiteral("must not be the calendar itself")});
  if (c.hoursPerDay <= 0 || c.hoursPerDay > 24) {
    errs.append({QStringLiteral("hoursPerDay"), QStringLiteral("must be more than 0 and at most 24")});
  }
  if (c.workWeek) {
    const WorkWeek& w = *c.workWeek;
    if (std::none_of(w.working.begin(), w.working.end(), [](bool b) { return b; })) {
      errs.append({QStringLiteral("workWeek"), QStringLiteral("needs at least one working day")});
    }
    for (int i = 0; i < 7; ++i) {
      const double h = w.hours[static_cast<size_t>(i)];
      const bool on = w.working[static_cast<size_t>(i)];
      if (h < 0 || h > 24 || (on && h <= 0) || (!on && h != 0)) {
        errs.append({QStringLiteral("workWeek"),
                     QStringLiteral("day %1: a working day has 0–24 hours (more than 0); a non-working day has 0").arg(i + 1)});
        break;
      }
    }
  }
  for (const CalendarException& e : c.exceptions) {
    for (const FieldError& f : validateCalendarException(e)) {
      errs.append({QStringLiteral("exceptions.") + f.field, f.message});
    }
  }
  return errs;
}

namespace {

QJsonValue optText(const QString& s) { return s.isEmpty() ? QJsonValue() : QJsonValue(s); }
QString text(const QJsonObject& o, const char* key) {
  const QJsonValue v = o.value(QLatin1String(key));
  return v.isString() ? v.toString() : QString();
}
void baseOut(QJsonObject& o, const EntityBase& e) {
  o.insert(QStringLiteral("id"), e.id);
  o.insert(QStringLiteral("createdAt"), e.createdAt);
  o.insert(QStringLiteral("updatedAt"), e.updatedAt);
  o.insert(QStringLiteral("deleted"), e.deleted);
  o.insert(QStringLiteral("rev"), e.rev);
}
void baseIn(EntityBase& e, const QJsonObject& o) {
  e.id = text(o, "id");
  e.createdAt = text(o, "createdAt");
  e.updatedAt = text(o, "updatedAt");
  e.deleted = o.value(QStringLiteral("deleted")).toBool(false);
  e.rev = static_cast<qint64>(o.value(QStringLiteral("rev")).toDouble(0));
}

}  // namespace

QJsonObject toJson(const CalendarDef& c) {
  QJsonObject o;
  baseOut(o, c);
  o.insert(QStringLiteral("name"), c.name);
  o.insert(QStringLiteral("base"), optText(c.base));
  if (c.workWeek) {
    QJsonArray days;
    for (int i = 0; i < 7; ++i) {
      days.append(QJsonObject{{QStringLiteral("working"), c.workWeek->working[static_cast<size_t>(i)]},
                              {QStringLiteral("hours"), c.workWeek->hours[static_cast<size_t>(i)]}});
    }
    o.insert(QStringLiteral("workWeek"), days);
  } else {
    o.insert(QStringLiteral("workWeek"), QJsonValue());
  }
  o.insert(QStringLiteral("hoursPerDay"), c.hoursPerDay);
  return o;
}

CalendarDef calendarFromJson(const QJsonObject& o) {
  CalendarDef c;
  baseIn(c, o);
  c.name = text(o, "name");
  c.base = text(o, "base");
  c.hoursPerDay = o.value(QStringLiteral("hoursPerDay")).toDouble(c.hoursPerDay);
  const QJsonValue w = o.value(QStringLiteral("workWeek"));
  if (w.isArray()) {
    // Anything but seven {working, hours} days becomes a week validation refuses.
    WorkWeek week;
    week.working.fill(false);
    week.hours.fill(0);
    const QJsonArray days = w.toArray();
    if (days.size() == 7) {
      for (int i = 0; i < 7; ++i) {
        const QJsonObject d = days.at(i).toObject();
        week.working[static_cast<size_t>(i)] = d.value(QStringLiteral("working")).toBool(false);
        week.hours[static_cast<size_t>(i)] = d.value(QStringLiteral("hours")).toDouble(-1);
      }
    }
    c.workWeek = week;
  }
  return c;
}

QJsonObject toJson(const CalendarException& e) {
  QJsonObject o;
  baseOut(o, e);
  o.insert(QStringLiteral("calendar"), e.calendar);
  o.insert(QStringLiteral("from"), e.from.toString(Qt::ISODate));
  o.insert(QStringLiteral("to"), e.to.toString(Qt::ISODate));
  o.insert(QStringLiteral("kind"), e.kind);
  o.insert(QStringLiteral("hours"), e.hours);
  o.insert(QStringLiteral("label"), e.label);
  return o;
}

CalendarException calendarExceptionFromJson(const QJsonObject& o) {
  CalendarException e;
  baseIn(e, o);
  e.calendar = text(o, "calendar");
  // Strictly YYYY-MM-DD; anything else is an invalid date.
  auto day = [](const QString& s) { return s.size() == 10 ? QDate::fromString(s, Qt::ISODate) : QDate(); };
  e.from = day(text(o, "from"));
  e.to = o.contains(QStringLiteral("to")) ? day(text(o, "to")) : e.from;
  e.kind = o.contains(QStringLiteral("kind")) ? text(o, "kind") : e.kind;
  e.hours = o.value(QStringLiteral("hours")).toDouble(0);
  e.label = text(o, "label");
  return e;
}

Result<WorkCalendar> WorkCalendar::resolve(const QString& id, const QHash<QString, CalendarDef>& defs) {
  WorkCalendar cal;
  if (id.isEmpty()) return cal;
  // The chain, leaf first.
  QList<const CalendarDef*> chain;
  QSet<QString> seen;
  QString cur = id;
  while (!cur.isEmpty()) {
    if (seen.contains(cur)) {
      return Error::validation(QStringLiteral("calendar %1 inherits from itself").arg(id),
                               QJsonObject{{QStringLiteral("field"), QStringLiteral("base")}});
    }
    if (chain.size() >= kMaxCalendarDepth) {
      return Error::validation(QStringLiteral("calendar %1: more than %2 base calendars").arg(id).arg(kMaxCalendarDepth),
                               QJsonObject{{QStringLiteral("field"), QStringLiteral("base")}});
    }
    const auto it = defs.constFind(cur);
    if (it == defs.constEnd()) return Error::notFound(QStringLiteral("calendar %1 not found").arg(cur));
    seen.insert(cur);
    chain.append(&it.value());
    cur = it->base;
  }
  cal.id_ = id;
  cal.hoursPerDay_ = chain.first()->hoursPerDay;
  for (const CalendarDef* d : chain) {
    if (d->workWeek) {
      cal.week_ = *d->workWeek;
      break;
    }
  }
  // Root first, so the leaf's own exceptions win where they overlap.
  for (auto it = chain.crbegin(); it != chain.crend(); ++it) {
    for (const CalendarException& e : (*it)->exceptions) {
      if (!e.from.isValid() || !e.to.isValid() || e.to < e.from || e.from.daysTo(e.to) >= kMaxExceptionDays) continue;
      for (qint64 jd = e.from.toJulianDay(); jd <= e.to.toJulianDay(); ++jd) {
        Override& o = cal.overrides_[jd];
        if (e.kind == QLatin1String("custom_hours")) {
          o.hours = e.hours;
        } else {
          o.working = false;
          o.hours.reset();
        }
      }
    }
  }
  return cal;
}

bool WorkCalendar::computeWorking(qint64 jd) const {
  const auto it = overrides_.constFind(jd);
  if (it != overrides_.constEnd()) {
    if (const std::optional<bool> w = it->working) return w.value_or(false);
  }
  return week_.working[static_cast<size_t>(dayIndex(QDate::fromJulianDay(jd)))];
}

bool WorkCalendar::prepared(const QDate& d) const {
  if (!d.isValid() || prefix_.empty()) return false;
  const qint64 i = d.toJulianDay() - lo_;
  return i >= 0 && i + 1 < static_cast<qint64>(prefix_.size());
}

bool WorkCalendar::isWorking(const QDate& d) const {
  if (!d.isValid()) return false;
  if (prepared(d)) {
    const auto i = static_cast<size_t>(d.toJulianDay() - lo_);
    return prefix_[i + 1] != prefix_[i];
  }
  return computeWorking(d.toJulianDay());
}

double WorkCalendar::hoursOn(const QDate& d) const {
  if (!isWorking(d)) return 0;
  const auto it = overrides_.constFind(d.toJulianDay());
  if (it != overrides_.constEnd()) {
    if (const std::optional<double> h = it->hours) return h.value_or(0);
  }
  return week_.hours[static_cast<size_t>(dayIndex(d))];
}

void WorkCalendar::prepare(const QDate& from, const QDate& to) {
  prefix_.clear();
  if (!from.isValid() || !to.isValid() || to < from) return;
  lo_ = from.toJulianDay();
  const qint64 n = from.daysTo(to) + 1;
  prefix_.resize(static_cast<size_t>(n) + 1);
  prefix_[0] = 0;
  for (qint64 i = 0; i < n; ++i) {
    prefix_[static_cast<size_t>(i) + 1] = prefix_[static_cast<size_t>(i)] + (computeWorking(lo_ + i) ? 1 : 0);
  }
}

QDate WorkCalendar::nextWorking(const QDate& d) const {
  if (!d.isValid()) return {};
  QDate x = d;
  for (int i = 0; i <= kSearchLimitDays; ++i, x = x.addDays(1)) {
    if (isWorking(x)) return x;
  }
  return {};
}

QDate WorkCalendar::previousWorking(const QDate& d) const {
  if (!d.isValid()) return {};
  QDate x = d;
  for (int i = 0; i <= kSearchLimitDays; ++i, x = x.addDays(-1)) {
    if (isWorking(x)) return x;
  }
  return {};
}

// The slow path: step day by day, giving up after kSearchLimitDays
// non-working days in a row.
QDate WorkCalendar::walk(const QDate& d, int n) const {
  QDate x = d;
  const int step = n > 0 ? 1 : -1;
  int left = n > 0 ? n : -n;
  int idle = 0;
  while (left > 0) {
    x = x.addDays(step);
    if (isWorking(x)) {
      --left;
      idle = 0;
    } else if (++idle > kSearchLimitDays) {
      return {};
    }
  }
  return x;
}

QDate WorkCalendar::addWorkingDays(const QDate& d, int n) const {
  if (!d.isValid()) return {};
  if (n == 0) return d;
  if (prepared(d)) {
    const qint64 idx = d.toJulianDay() - lo_;
    // Working days in [lo_, j] is prefix_[j + 1]; the first k with
    // prefix_[k] >= target makes day k - 1 the one wanted.
    const qint64 target =
        n > 0 ? static_cast<qint64>(prefix_[static_cast<size_t>(idx) + 1]) + n : static_cast<qint64>(prefix_[static_cast<size_t>(idx)]) + n + 1;
    if (target >= 1 && target <= prefix_.back()) {
      const auto it = std::lower_bound(prefix_.begin(), prefix_.end(), target);
      const qint64 k = it - prefix_.begin();
      return QDate::fromJulianDay(lo_ + k - 1);
    }
  }
  return walk(d, n);
}

qint64 WorkCalendar::workingDaysBetween(const QDate& a, const QDate& b) const {
  if (!a.isValid() || !b.isValid()) return 0;
  if (b < a) return -workingDaysBetween(b, a);
  if (a == b) return 0;
  const QDate last = b.addDays(-1);
  if (prepared(a) && prepared(last)) {
    return prefix_[static_cast<size_t>(last.toJulianDay() - lo_) + 1] - prefix_[static_cast<size_t>(a.toJulianDay() - lo_)];
  }
  qint64 n = 0;
  for (QDate x = a; x < b; x = x.addDays(1)) {
    if (isWorking(x)) ++n;
  }
  return n;
}

}  // namespace zproject
