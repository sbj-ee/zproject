// Working calendars and holidays (scheduling plan S1; SPEC.md §6.1, §17, §19).
//  - WorkCalendar: Standard, work weeks, exceptions, base inheritance and
//    its errors, custom hours, day arithmetic forwards and backwards;
//  - the prepared (bitmap) path against a plain day-by-day count, on
//    random calendars with fixed seeds;
//  - validation of calendars and exceptions;
//  - usFederalHolidays() against OPM's published dates, observance rules
//    and edge years.
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTest>

#include "core/Holidays.hpp"
#include "core/WorkCalendar.hpp"

using namespace zproject;

namespace {

QDate d(const char* iso) { return QDate::fromString(QLatin1String(iso), Qt::ISODate); }

CalendarException exc(const char* from, const char* to, const char* kind = "holiday", double hours = 0) {
  CalendarException e;
  e.from = d(from);
  e.to = d(to);
  e.kind = QLatin1String(kind);
  e.hours = hours;
  return e;
}

WorkCalendar resolved(const QString& id, const QHash<QString, CalendarDef>& defs) {
  auto r = WorkCalendar::resolve(id, defs);
  if (!r) qWarning("resolve failed: %s", qPrintable(r.error().message));
  return r ? r.value() : WorkCalendar();
}

// The plain definitions the fast path must agree with.
QDate naiveAdd(const WorkCalendar& c, QDate x, int n) {
  const int step = n > 0 ? 1 : -1;
  for (int left = std::abs(n); left > 0;) {
    x = x.addDays(step);
    if (c.isWorking(x)) --left;
  }
  return x;
}
qint64 naiveBetween(const WorkCalendar& c, QDate a, QDate b) {
  qint64 n = 0;
  for (QDate x = a; x < b; x = x.addDays(1)) n += c.isWorking(x) ? 1 : 0;
  return n;
}

QStringList holidayList(int year) {
  QStringList out;
  for (const CalendarException& e : usFederalHolidays(year)) out << e.from.toString(Qt::ISODate);
  return out;
}

}  // namespace

class TestWorkCalendar : public QObject {
  Q_OBJECT

 private slots:
  // ---- Standard and day arithmetic ----

  void standardIsMondayToFriday() {
    const WorkCalendar c;
    QVERIFY(c.isWorking(d("2026-11-23")));   // Monday
    QVERIFY(c.isWorking(d("2026-11-27")));   // Friday
    QVERIFY(!c.isWorking(d("2026-11-28")));  // Saturday
    QVERIFY(!c.isWorking(d("2026-11-29")));  // Sunday
    QCOMPARE(c.hoursOn(d("2026-11-23")), 8.0);
    QCOMPARE(c.hoursOn(d("2026-11-28")), 0.0);
    QCOMPARE(c.hoursPerDay(), 8.0);
    QVERIFY(!c.isWorking(QDate()));
  }

  void nextAndPreviousWorking() {
    const WorkCalendar c;
    QCOMPARE(c.nextWorking(d("2026-11-27")), d("2026-11-27"));      // already working
    QCOMPARE(c.nextWorking(d("2026-11-28")), d("2026-11-30"));      // Sat -> Mon
    QCOMPARE(c.previousWorking(d("2026-11-29")), d("2026-11-27"));  // Sun -> Fri
    QVERIFY(!c.nextWorking(QDate()).isValid());
  }

  void addWorkingDays() {
    const WorkCalendar c;
    QCOMPARE(c.addWorkingDays(d("2026-11-23"), 0), d("2026-11-23"));
    QCOMPARE(c.addWorkingDays(d("2026-11-23"), 2), d("2026-11-25"));   // Mon + 2 = Wed
    QCOMPARE(c.addWorkingDays(d("2026-11-26"), 2), d("2026-11-30"));   // Thu + 2 = Mon
    QCOMPARE(c.addWorkingDays(d("2026-11-28"), 1), d("2026-11-30"));   // from a Saturday
    QCOMPARE(c.addWorkingDays(d("2026-11-30"), -1), d("2026-11-27"));  // Mon - 1 = Fri
    QCOMPARE(c.addWorkingDays(d("2026-11-29"), -1), d("2026-11-27"));  // from a Sunday
    QCOMPARE(c.addWorkingDays(d("2026-11-23"), 10), d("2026-12-07"));  // two weeks
    QCOMPARE(c.addWorkingDays(d("2026-12-07"), -10), d("2026-11-23"));
  }

  void workingDaysBetween() {
    const WorkCalendar c;
    QCOMPARE(c.workingDaysBetween(d("2026-11-23"), d("2026-11-23")), 0);
    QCOMPARE(c.workingDaysBetween(d("2026-11-23"), d("2026-11-30")), 5);   // [Mon, next Mon)
    QCOMPARE(c.workingDaysBetween(d("2026-11-28"), d("2026-11-30")), 0);   // a weekend
    QCOMPARE(c.workingDaysBetween(d("2026-11-30"), d("2026-11-23")), -5);  // backwards
    // between(a, add(a, n)) == n for a working a.
    QCOMPARE(c.workingDaysBetween(d("2026-11-23"), c.addWorkingDays(d("2026-11-23"), 7)), 7);
  }

  // ---- exceptions and inheritance ----

  void exceptionsMakeDaysNonWorking() {
    CalendarDef cal;
    cal.id = QStringLiteral("c");
    cal.name = QStringLiteral("Office");
    cal.exceptions = {exc("2026-11-26", "2026-11-27"),  // Thanksgiving + Friday
                      exc("2026-12-24", "2026-12-24", "non_working")};
    const WorkCalendar c = resolved(QStringLiteral("c"), {{cal.id, cal}});
    QVERIFY(!c.isWorking(d("2026-11-26")));
    QVERIFY(!c.isWorking(d("2026-11-27")));
    QVERIFY(!c.isWorking(d("2026-12-24")));
    QCOMPARE(c.addWorkingDays(d("2026-11-25"), 1), d("2026-11-30"));
    QCOMPARE(c.workingDaysBetween(d("2026-11-23"), d("2026-11-30")), 3);
    QCOMPARE(c.id(), QStringLiteral("c"));
  }

  void customHoursChangeHoursNotDays() {
    CalendarDef cal;
    cal.id = QStringLiteral("c");
    cal.name = QStringLiteral("Short Fridays");
    cal.exceptions = {exc("2026-11-27", "2026-11-27", "custom_hours", 4),
                      exc("2026-11-28", "2026-11-28", "custom_hours", 6)};  // a Saturday stays off
    const WorkCalendar c = resolved(QStringLiteral("c"), {{cal.id, cal}});
    QVERIFY(c.isWorking(d("2026-11-27")));
    QCOMPARE(c.hoursOn(d("2026-11-27")), 4.0);
    QVERIFY(!c.isWorking(d("2026-11-28")));
    QCOMPARE(c.hoursOn(d("2026-11-28")), 0.0);
    QCOMPARE(c.workingDaysBetween(d("2026-11-23"), d("2026-11-30")), 5);
  }

  void ownWorkWeek() {
    CalendarDef cal;
    cal.id = QStringLiteral("c");
    cal.name = QStringLiteral("Four tens");
    WorkWeek w;
    w.working = {true, true, true, true, false, false, false};
    w.hours = {10, 10, 10, 10, 0, 0, 0};
    cal.workWeek = w;
    cal.hoursPerDay = 10;
    const WorkCalendar c = resolved(QStringLiteral("c"), {{cal.id, cal}});
    QVERIFY(!c.isWorking(d("2026-11-27")));  // Friday off
    QCOMPARE(c.hoursOn(d("2026-11-23")), 10.0);
    QCOMPARE(c.hoursPerDay(), 10.0);
    QCOMPARE(c.addWorkingDays(d("2026-11-26"), 1), d("2026-11-30"));  // Thu + 1 = Mon
  }

  void baseInheritance() {
    CalendarDef company;
    company.id = QStringLiteral("co");
    company.name = QStringLiteral("Company");
    WorkWeek w;
    w.working = {true, true, true, true, false, false, false};
    w.hours = {9, 9, 9, 9, 0, 0, 0};
    company.workWeek = w;
    company.exceptions = {exc("2026-12-25", "2026-12-25"), exc("2026-12-21", "2026-12-21")};
    CalendarDef site;
    site.id = QStringLiteral("site");
    site.name = QStringLiteral("Site");
    site.base = QStringLiteral("co");
    site.hoursPerDay = 9;
    // Own exceptions add to the base's, and win where they overlap.
    site.exceptions = {exc("2026-12-22", "2026-12-22"), exc("2026-12-21", "2026-12-21", "custom_hours", 5)};
    const QHash<QString, CalendarDef> defs{{company.id, company}, {site.id, site}};
    const WorkCalendar c = resolved(QStringLiteral("site"), defs);
    QCOMPARE(c.workWeek(), w);                // inherited
    QVERIFY(!c.isWorking(d("2026-12-25")));   // the base's holiday
    QVERIFY(!c.isWorking(d("2026-12-22")));   // its own
    QVERIFY(!c.isWorking(d("2026-12-21")));   // base holiday; custom_hours never makes a day count
    QVERIFY(c.isWorking(d("2026-12-24")));    // a Thursday, no exception
    QVERIFY(!c.isWorking(d("2026-12-18")));   // a Friday: the inherited four-day week
  }

  void resolveErrors() {
    CalendarDef a;
    a.id = QStringLiteral("a");
    a.name = QStringLiteral("A");
    a.base = QStringLiteral("b");
    CalendarDef b;
    b.id = QStringLiteral("b");
    b.name = QStringLiteral("B");
    b.base = QStringLiteral("a");
    auto loop = WorkCalendar::resolve(QStringLiteral("a"), {{a.id, a}, {b.id, b}});
    QVERIFY(!loop);
    QCOMPARE(loop.error().code, QStringLiteral("validation"));
    auto missing = WorkCalendar::resolve(QStringLiteral("nope"), {});
    QCOMPARE(missing.error().code, QStringLiteral("not_found"));
    b.base = QStringLiteral("gone");
    QCOMPARE(WorkCalendar::resolve(QStringLiteral("a"), {{a.id, a}, {b.id, b}}).error().code, QStringLiteral("not_found"));
    // A chain deeper than the limit.
    QHash<QString, CalendarDef> deep;
    for (int i = 0; i <= kMaxCalendarDepth; ++i) {
      CalendarDef c;
      c.id = QString::number(i);
      c.name = c.id;
      if (i < kMaxCalendarDepth) c.base = QString::number(i + 1);
      deep.insert(c.id, c);
    }
    QCOMPARE(WorkCalendar::resolve(QStringLiteral("0"), deep).error().code, QStringLiteral("validation"));
    // Empty id: Standard.
    QVERIFY(WorkCalendar::resolve(QString(), {}).ok());
  }

  void noWorkingDayGivesUp() {
    // Validation refuses this calendar; the arithmetic must still end.
    CalendarDef cal;
    cal.id = QStringLiteral("c");
    cal.name = QStringLiteral("Never");
    WorkWeek w;
    w.working.fill(false);
    w.hours.fill(0);
    cal.workWeek = w;
    const WorkCalendar c = resolved(QStringLiteral("c"), {{cal.id, cal}});
    QVERIFY(!c.nextWorking(d("2026-11-23")).isValid());
    QVERIFY(!c.addWorkingDays(d("2026-11-23"), 3).isValid());
  }

  // ---- the prepared path ----

  void preparedMatchesNaive() {
    const quint32 seeds[] = {1, 7, 42, 1234, 99991};
    for (quint32 seed : seeds) {
      QRandomGenerator rng(seed);
      CalendarDef cal;
      cal.id = QStringLiteral("r");
      cal.name = QStringLiteral("Random");
      WorkWeek w;
      for (int i = 0; i < 7; ++i) {
        w.working[static_cast<size_t>(i)] = rng.bounded(4) != 0;
        w.hours[static_cast<size_t>(i)] = w.working[static_cast<size_t>(i)] ? 8 : 0;
      }
      w.working[0] = true;
      w.hours[0] = 8;
      cal.workWeek = w;
      const QDate base = d("2026-01-01");
      for (int i = 0; i < 40; ++i) {
        const QDate from = base.addDays(rng.bounded(700));
        CalendarException e;
        e.from = from;
        e.to = from.addDays(rng.bounded(10));
        e.kind = rng.bounded(5) == 0 ? QStringLiteral("custom_hours") : QStringLiteral("holiday");
        e.hours = e.kind == QLatin1String("custom_hours") ? 4 : 0;
        cal.exceptions.append(e);
      }
      const WorkCalendar slow = resolved(cal.id, {{cal.id, cal}});
      WorkCalendar fast = slow;
      fast.prepare(base, base.addDays(730));
      for (int i = 0; i < 400; ++i) {
        // Some queries start or land outside the prepared range.
        const QDate a = base.addDays(static_cast<int>(rng.bounded(800)) - 30);
        const int n = static_cast<int>(rng.bounded(120)) - 60;
        const QString ctx = QStringLiteral("seed %1, %2 %3").arg(seed).arg(a.toString(Qt::ISODate)).arg(n);
        QCOMPARE(fast.isWorking(a), slow.isWorking(a));
        QVERIFY2(fast.addWorkingDays(a, n) == naiveAdd(slow, a, n), qPrintable(ctx));
        QCOMPARE(slow.addWorkingDays(a, n), naiveAdd(slow, a, n));
        const QDate b = a.addDays(n * 2);
        QVERIFY2(fast.workingDaysBetween(a, b) == (b >= a ? naiveBetween(slow, a, b) : -naiveBetween(slow, b, a)),
                 qPrintable(ctx));
        QCOMPARE(fast.nextWorking(a), slow.nextWorking(a));
        QCOMPARE(fast.hoursOn(a), slow.hoursOn(a));
      }
    }
  }

  void preparedIsFast() {
    // The scheduler asks a few working-day questions per task and link: a
    // year-long project's 2,000 tasks and 4,000 links are ~20,000 queries.
    WorkCalendar c;
    c.prepare(d("2026-01-01"), d("2030-12-31"));
    QElapsedTimer t;
    t.start();
    qint64 sum = 0;
    for (int i = 0; i < 20000; ++i) {
      const QDate a = d("2026-01-01").addDays(i % 1000);
      sum += c.addWorkingDays(a, (i % 41) - 20).toJulianDay() + c.workingDaysBetween(a, a.addDays(i % 90));
    }
    const qint64 ms = t.elapsed();
    QVERIFY(sum != 0);
    qInfo("20,000 prepared queries: %lld ms", ms);
#if defined(__SANITIZE_ADDRESS__)
    QVERIFY2(ms < 200, qPrintable(QString::number(ms)));
#else
    QVERIFY2(ms < 50, qPrintable(QString::number(ms)));
#endif
  }

  // ---- validation ----

  void validation() {
    CalendarDef c;
    c.name = QStringLiteral("Ok");
    QVERIFY(validateCalendar(c).isEmpty());
    c.name.clear();
    c.hoursPerDay = 0;
    WorkWeek none;
    none.working.fill(false);
    none.hours.fill(0);
    c.workWeek = none;
    QStringList fields;
    for (const FieldError& e : validateCalendar(c)) fields << e.field;
    QVERIFY(fields.contains(QStringLiteral("name")));
    QVERIFY(fields.contains(QStringLiteral("hoursPerDay")));
    QVERIFY(fields.contains(QStringLiteral("workWeek")));
    WorkWeek bad;
    bad.hours[5] = 4;  // hours on a non-working Saturday
    c = {};
    c.name = QStringLiteral("x");
    c.workWeek = bad;
    QCOMPARE(validateCalendar(c).size(), 1);
    c = {};
    c.id = QStringLiteral("self");
    c.name = QStringLiteral("x");
    c.base = QStringLiteral("self");
    QCOMPARE(validateCalendar(c).first().field, QStringLiteral("base"));

    QVERIFY(validateCalendarException(exc("2026-12-25", "2026-12-25")).isEmpty());
    QCOMPARE(validateCalendarException(exc("2026-12-25", "2026-12-24")).first().field, QStringLiteral("to"));
    QCOMPARE(validateCalendarException(exc("2026-01-01", "2027-06-01")).first().field, QStringLiteral("to"));
    QCOMPARE(validateCalendarException(exc("2026-12-25", "2026-12-25", "party")).first().field, QStringLiteral("kind"));
    QCOMPARE(validateCalendarException(exc("2026-12-25", "2026-12-25", "custom_hours", 0)).first().field,
             QStringLiteral("hours"));
    QCOMPARE(validateCalendarException(exc("2026-12-25", "2026-12-25", "holiday", 3)).first().field, QStringLiteral("hours"));
    CalendarException noDates;
    QCOMPARE(validateCalendarException(noDates).size(), 2);
    c = {};
    c.name = QStringLiteral("x");
    c.exceptions = {exc("2026-12-25", "2026-12-24")};
    QCOMPARE(validateCalendar(c).first().field, QStringLiteral("exceptions.to"));
  }

  // ---- holidays ----

  void nthWeekdays() {
    QCOMPARE(nthWeekday(2026, 1, 1, 3), d("2026-01-19"));   // 3rd Monday
    QCOMPARE(nthWeekday(2026, 5, 1, -1), d("2026-05-25"));  // last Monday
    QCOMPARE(nthWeekday(2026, 11, 4, 4), d("2026-11-26"));  // 4th Thursday
    QCOMPARE(nthWeekday(2027, 5, 1, -1), d("2027-05-31"));  // the 31st itself
    QVERIFY(!nthWeekday(2026, 2, 1, 5).isValid());          // no 5th Monday
    QVERIFY(!nthWeekday(2026, 13, 1, 1).isValid());
  }

  // OPM's published federal holidays (observed dates).
  void usFederalHolidaysGolden_data() {
    QTest::addColumn<int>("year");
    QTest::addColumn<QStringList>("dates");
    QTest::newRow("2020") << 2020
                          << QStringList{"2020-01-01", "2020-01-20", "2020-02-17", "2020-05-25", "2020-07-03",
                                         "2020-09-07", "2020-10-12", "2020-11-11", "2020-11-26", "2020-12-25"};
    // Juneteenth's first year, on a Saturday; New Year's Day 2022 on the 31st.
    QTest::newRow("2021") << 2021
                          << QStringList{"2021-01-01", "2021-01-18", "2021-02-15", "2021-05-31",
                                         "2021-06-18", "2021-07-05", "2021-09-06", "2021-10-11",
                                         "2021-11-11", "2021-11-25", "2021-12-24", "2021-12-31"};
    // 1 January 2022 is a Saturday, observed in 2021: not in this list.
    QTest::newRow("2022") << 2022
                          << QStringList{"2022-01-17", "2022-02-21", "2022-05-30", "2022-06-20", "2022-07-04",
                                         "2022-09-05", "2022-10-10", "2022-11-11", "2022-11-24", "2022-12-26"};
    QTest::newRow("2024") << 2024
                          << QStringList{"2024-01-01", "2024-01-15", "2024-02-19", "2024-05-27",
                                         "2024-06-19", "2024-07-04", "2024-09-02", "2024-10-14",
                                         "2024-11-11", "2024-11-28", "2024-12-25"};
    QTest::newRow("2026") << 2026
                          << QStringList{"2026-01-01", "2026-01-19", "2026-02-16", "2026-05-25",
                                         "2026-06-19", "2026-07-03", "2026-09-07", "2026-10-12",
                                         "2026-11-11", "2026-11-26", "2026-12-25"};
    QTest::newRow("2027") << 2027
                          << QStringList{"2027-01-01", "2027-01-18", "2027-02-15", "2027-05-31",
                                         "2027-06-18", "2027-07-05", "2027-09-06", "2027-10-11",
                                         "2027-11-11", "2027-11-25", "2027-12-24", "2027-12-31"};
  }
  void usFederalHolidaysGolden() {
    QFETCH(int, year);
    QFETCH(QStringList, dates);
    QCOMPARE(holidayList(year), dates);
  }

  void holidayLabelsAndShape() {
    const QList<CalendarException> h = usFederalHolidays(2026);
    for (const CalendarException& e : h) {
      QCOMPARE(e.kind, QStringLiteral("holiday"));
      QCOMPARE(e.from, e.to);
      QVERIFY(validateCalendarException(e).isEmpty());
      QVERIFY(!e.label.isEmpty());
    }
    QCOMPARE(h.at(5).label, QStringLiteral("Independence Day (observed)"));  // 3 July 2026
    QCOMPARE(h.at(0).label, QStringLiteral("New Year's Day"));
    const QList<CalendarException> y27 = usFederalHolidays(2027);
    QCOMPARE(y27.last().label, QStringLiteral("New Year's Day (observed)"));
    QVERIFY(usFederalHolidays(1970).isEmpty());
    QVERIFY(usFederalHolidays(9999).isEmpty());
    // Imported into a calendar, they are non-working days.
    CalendarDef cal;
    cal.id = QStringLiteral("us");
    cal.name = QStringLiteral("US");
    cal.exceptions = h;
    const WorkCalendar c = resolved(cal.id, {{cal.id, cal}});
    QVERIFY(!c.isWorking(d("2026-07-03")));
    QVERIFY(!c.isWorking(d("2026-11-26")));
    QCOMPARE(c.workingDaysBetween(d("2026-11-23"), d("2026-11-30")), 4);
  }
};

QTEST_GUILESS_MAIN(TestWorkCalendar)
#include "tst_workcalendar.moc"
