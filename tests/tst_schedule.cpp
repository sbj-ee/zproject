// The CPM engine (scheduling plan S2; SPEC.md §6.2, §7.2, §17, §19).
//  - golden cases, one rule each, with the dates worked out by hand: each
//    link type with lag and lead, weekends and holidays, milestones,
//    constraints, manual tasks, actuals, summaries, float, critical, no
//    project start, target finish, calendars per task, violations, cycles;
//  - the ripple between two schedules;
//  - property tests on random acyclic plans (fixed seeds): every link is
//    satisfied, float is never negative without constraints, the critical
//    tasks run from the project start to its finish, and the result does
//    not depend on the order links are given in;
//  - 2,000 tasks and 4,000 links within 50 ms (SPEC.md §17).
// Project start in most cases: Monday 2 November 2026 (Standard calendar).
#include <QElapsedTimer>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QTest>

#include "core/Schedule.hpp"

using namespace zproject;

namespace {

QDate d(const char* iso) { return QDate::fromString(QLatin1String(iso), Qt::ISODate); }
const QDate kMon = d("2026-11-02");

Task task(const char* id, double duration = 1, const char* parent = nullptr) {
  Task t;
  t.id = QLatin1String(id);
  t.title = QLatin1String(id);
  t.duration = duration;
  if (parent) t.parent = QLatin1String(parent);
  return t;
}
Task milestone(const char* id, const char* parent = nullptr) {
  Task t = task(id, 0, parent);
  t.type = QStringLiteral("milestone");
  return t;
}
Task manual(const char* id, const char* start, const char* finish, double duration = 1) {
  Task t = task(id, duration);
  t.mode = QStringLiteral("manual");
  t.start = QLatin1String(start);
  t.finish = QLatin1String(finish);
  return t;
}
// Not `link`: POSIX link(const char*, const char*) from <unistd.h> would be ambiguous on Linux.
Dependency dep(const char* pred, const char* succ, const char* type = "FS", int lag = 0) {
  Dependency l;
  l.id = QStringLiteral("%1-%2").arg(QLatin1String(pred), QLatin1String(succ));
  l.predecessor = QLatin1String(pred);
  l.successor = QLatin1String(succ);
  l.type = QLatin1String(type);
  l.lag = lag;
  return l;
}

Schedule run(const QList<Task>& tasks, const QList<Dependency>& links = {}, QDate start = kMon,
             const QHash<QString, WorkCalendar>& cals = {}, const QString& projectCal = {}, QDate target = {}) {
  ScheduleInput in;
  in.projectStart = start;
  in.projectFinish = target;
  in.projectCalendar = projectCal;
  in.tasks = tasks;
  in.links = links;
  in.calendars = cals;
  auto r = computeSchedule(in);
  if (!r) qWarning("schedule failed: %s", qPrintable(r.error().message));
  return r ? r.value() : Schedule();
}

QString span(const Schedule& s, const char* id) {
  const ScheduledTask* t = s.task(QLatin1String(id));
  if (!t) return QStringLiteral("missing");
  if (!t->scheduled) return QStringLiteral("unscheduled");
  return t->start.toString(Qt::ISODate) + QLatin1Char('/') + t->finish.toString(Qt::ISODate);
}
const ScheduledTask& at(const Schedule& s, const char* id) { return *s.task(QLatin1String(id)); }
QStringList kinds(const Schedule& s) {
  QStringList k;
  for (const ScheduleViolation& v : s.violations) k << v.kind + QLatin1Char(':') + v.task;
  return k;
}

}  // namespace

class TestSchedule : public QObject {
  Q_OBJECT

 private slots:
  // ---- links ----

  void finishToStart() {
    const Schedule s = run({task("a", 3), task("b", 2)}, {dep("a", "b")});
    QCOMPARE(span(s, "a"), QStringLiteral("2026-11-02/2026-11-04"));  // Mon–Wed
    QCOMPARE(span(s, "b"), QStringLiteral("2026-11-05/2026-11-06"));  // Thu–Fri
    QCOMPARE(s.finish, d("2026-11-06"));
    QVERIFY(s.violations.isEmpty());
  }

  void lagAndLead() {
    QCOMPARE(span(run({task("a", 3), task("b", 2)}, {dep("a", "b", "FS", 2)}), "b"),
             QStringLiteral("2026-11-09/2026-11-10"));  // Thu, Fri skipped: Mon–Tue
    QCOMPARE(span(run({task("a", 3), task("b", 2)}, {dep("a", "b", "FS", -1)}), "b"),
             QStringLiteral("2026-11-04/2026-11-05"));  // a one-day lead: Wed–Thu
    // A lead can't pull a task before the project start.
    QCOMPARE(span(run({task("a", 1), task("b", 1)}, {dep("a", "b", "FS", -5)}), "b"),
             QStringLiteral("2026-11-02/2026-11-02"));
  }

  void startToStart() {
    const Schedule s = run({task("a", 3), task("b", 2)}, {dep("a", "b", "SS", 1)});
    QCOMPARE(span(s, "b"), QStringLiteral("2026-11-03/2026-11-04"));  // a day after a starts
    QCOMPARE(span(run({task("a", 3), task("b", 2)}, {dep("a", "b", "SS")}), "b"),
             QStringLiteral("2026-11-02/2026-11-03"));
  }

  void finishToFinish() {
    // b (1 day) finishes with a (Wed): Wed.
    QCOMPARE(span(run({task("a", 3), task("b", 1)}, {dep("a", "b", "FF")}), "b"),
             QStringLiteral("2026-11-04/2026-11-04"));
    // b longer than a would start before the project: the project start wins.
    QCOMPARE(span(run({task("a", 3), task("b", 5)}, {dep("a", "b", "FF")}), "b"),
             QStringLiteral("2026-11-02/2026-11-06"));
    // FF + 2: finishes two working days after a.
    QCOMPARE(span(run({task("a", 3), task("b", 1)}, {dep("a", "b", "FF", 2)}), "b"),
             QStringLiteral("2026-11-06/2026-11-06"));
  }

  void weekends() {
    const Schedule s = run({task("a", 4)}, {}, d("2026-11-05"));  // from a Thursday
    QCOMPARE(span(s, "a"), QStringLiteral("2026-11-05/2026-11-10"));  // Thu, Fri, Mon, Tue
    // A project starting on a Saturday starts work on Monday.
    QCOMPARE(span(run({task("a", 1)}, {}, d("2026-11-07")), "a"), QStringLiteral("2026-11-09/2026-11-09"));
    // Fractional durations take whole days.
    QCOMPARE(span(run({task("a", 1.5)}), "a"), QStringLiteral("2026-11-02/2026-11-03"));
  }

  void holidaysInACalendar() {
    CalendarDef c;
    c.id = QStringLiteral("us");
    c.name = QStringLiteral("US");
    CalendarException tg;
    tg.from = d("2026-11-26");
    tg.to = d("2026-11-27");
    c.exceptions = {tg};
    const QHash<QString, WorkCalendar> cals{{c.id, WorkCalendar::resolve(c.id, {{c.id, c}}).value()}};
    Task a = task("a", 3);
    a.constraintType = QStringLiteral("SNET");
    a.constraintDate = QStringLiteral("2026-11-25");
    const Schedule s = run({a}, {}, kMon, cals, QStringLiteral("us"));
    QCOMPARE(span(s, "a"), QStringLiteral("2026-11-25/2026-12-01"));  // Wed, (Thu–Sun off), Mon, Tue
    QVERIFY(s.violations.isEmpty());
  }

  void calendarsPerTask() {
    // b works Monday–Thursday; a (Standard) finishes Thursday 5 Nov.
    CalendarDef c;
    c.id = QStringLiteral("four");
    c.name = QStringLiteral("Four days");
    WorkWeek w;
    w.working = {true, true, true, true, false, false, false};
    w.hours = {10, 10, 10, 10, 0, 0, 0};
    c.workWeek = w;
    const QHash<QString, WorkCalendar> cals{{c.id, WorkCalendar::resolve(c.id, {{c.id, c}}).value()}};
    Task b = task("b", 2);
    b.calendar = QStringLiteral("four");
    const Schedule s = run({task("a", 4), b}, {dep("a", "b")}, kMon, cals);
    QCOMPARE(span(s, "a"), QStringLiteral("2026-11-02/2026-11-05"));
    QCOMPARE(span(s, "b"), QStringLiteral("2026-11-09/2026-11-10"));  // its next working day is Monday
    // An unknown calendar is reported and the project's is used.
    Task x = task("x", 1);
    x.calendar = QStringLiteral("gone");
    const Schedule u = run({x});
    QCOMPARE(kinds(u), QStringList{QStringLiteral("unknown_calendar:x")});
    QCOMPARE(span(u, "x"), QStringLiteral("2026-11-02/2026-11-02"));
  }

  // ---- milestones ----

  void milestones() {
    // After a task: on its finish day, as MS Project shows it.
    const Schedule s = run({task("a", 3), milestone("m"), task("b", 1)}, {dep("a", "m"), dep("m", "b")});
    QCOMPARE(span(s, "m"), QStringLiteral("2026-11-04/2026-11-04"));
    QCOMPARE(span(s, "b"), QStringLiteral("2026-11-05/2026-11-05"));  // the day after
    QVERIFY(at(s, "m").milestone);
    QVERIFY(at(s, "m").critical);
    // With nothing before it: the project start.
    QCOMPARE(span(run({milestone("m")}), "m"), QStringLiteral("2026-11-02/2026-11-02"));
    // Start-to-start from a task: that task's start.
    QCOMPARE(span(run({task("a", 3), milestone("m")}, {dep("a", "m", "SS", 1)}), "m"),
             QStringLiteral("2026-11-03/2026-11-03"));
    // Two milestones in a row (FS): the same day.
    QCOMPARE(span(run({task("a", 2), milestone("m"), milestone("n")}, {dep("a", "m"), dep("m", "n")}), "n"),
             QStringLiteral("2026-11-03/2026-11-03"));
  }

  // ---- constraints, float, critical ----

  void startNoEarlierThan() {
    Task b = task("b", 2);
    b.constraintType = QStringLiteral("SNET");
    b.constraintDate = QStringLiteral("2026-11-12");
    const Schedule s = run({task("a", 3), b}, {dep("a", "b")});
    QCOMPARE(span(s, "b"), QStringLiteral("2026-11-12/2026-11-13"));
    // SNET earlier than the link: the link wins.
    b.constraintDate = QStringLiteral("2026-11-03");
    QCOMPARE(span(run({task("a", 3), b}, {dep("a", "b")}), "b"), QStringLiteral("2026-11-05/2026-11-06"));
  }

  void floatAndCritical() {
    // a (3) and b (1) both before c (1): a is critical, b has 2 days.
    const Schedule s = run({task("a", 3), task("b", 1), task("c", 1)}, {dep("a", "c"), dep("b", "c")});
    QCOMPARE(span(s, "c"), QStringLiteral("2026-11-05/2026-11-05"));
    QCOMPARE(at(s, "a").totalFloat, 0);
    QVERIFY(at(s, "a").critical);
    QCOMPARE(at(s, "b").totalFloat, 2);
    QCOMPARE(at(s, "b").freeFloat, 2);
    QVERIFY(!at(s, "b").critical);
    QCOMPARE(at(s, "b").lateStart, d("2026-11-04"));
    QCOMPARE(at(s, "b").lateFinish, d("2026-11-04"));
    QVERIFY(at(s, "c").critical);
    QCOMPARE(s.critical, (QStringList{QStringLiteral("a"), QStringLiteral("c")}));
  }

  void freeVersusTotalFloat() {
    // b (1) → c (1) → d (1), alongside a (5) → d: b and c share 2 days of
    // total float, but only c has free float.
    const Schedule s = run({task("a", 5), task("b", 1), task("c", 1), task("d", 1)},
                           {dep("a", "d"), dep("b", "c"), dep("c", "d")});
    QCOMPARE(at(s, "b").totalFloat, 3);
    QCOMPARE(at(s, "b").freeFloat, 0);
    QCOMPARE(at(s, "c").totalFloat, 3);
    QCOMPARE(at(s, "c").freeFloat, 3);
  }

  void targetFinishGivesFloat() {
    const Schedule s = run({task("a", 3)}, {}, kMon, {}, {}, d("2026-11-13"));
    QCOMPARE(at(s, "a").totalFloat, 7);  // finishes Wed 4, target Fri 13
    QVERIFY(!at(s, "a").critical);
    QCOMPARE(s.finish, d("2026-11-04"));
    QVERIFY(s.critical.isEmpty());
  }

  void finishNoLaterThan() {
    Task a = task("a", 5);
    a.constraintType = QStringLiteral("FNLT");
    a.constraintDate = QStringLiteral("2026-11-04");
    const Schedule s = run({a});
    QCOMPARE(span(s, "a"), QStringLiteral("2026-11-02/2026-11-06"));  // not moved
    QCOMPARE(at(s, "a").totalFloat, -2);
    QVERIFY(at(s, "a").critical);
    QCOMPARE(kinds(s), QStringList{QStringLiteral("constraint_missed:a")});
    // Met: no violation, and it limits the late finish.
    a.constraintDate = QStringLiteral("2026-11-10");
    const Schedule ok = run({a}, {}, kMon, {}, {}, d("2026-11-30"));
    QVERIFY(ok.violations.isEmpty());
    QCOMPARE(at(ok, "a").lateFinish, d("2026-11-10"));
  }

  void deadlinesOnlyReport() {
    Task a = task("a", 5);
    a.deadline = QStringLiteral("2026-11-04");
    const Schedule s = run({a});
    QCOMPARE(kinds(s), QStringList{QStringLiteral("deadline_missed:a")});
    QCOMPARE(at(s, "a").totalFloat, 0);  // schedule to deadline is off (plan Q5)
  }

  // ---- manual tasks and actuals ----

  void manualTasksKeepTheirDates() {
    const Schedule s = run({task("a", 3), manual("m", "2026-11-03", "2026-11-04", 2)}, {dep("a", "m")});
    QCOMPARE(span(s, "m"), QStringLiteral("2026-11-03/2026-11-04"));
    QVERIFY(at(s, "m").fixed);
    QCOMPARE(kinds(s), QStringList{QStringLiteral("link_broken:m")});
    QCOMPARE(s.violations.first().other, QStringLiteral("a"));
    QVERIFY(at(s, "a").totalFloat < 0);  // a should have finished before m started
    // A manual task with no dates.
    Task none = task("n", 1);
    none.mode = QStringLiteral("manual");
    const Schedule u = run({none, task("b")}, {dep("n", "b")});
    QCOMPARE(span(u, "n"), QStringLiteral("unscheduled"));
    QCOMPARE(span(u, "b"), QStringLiteral("2026-11-02/2026-11-02"));  // its link is ignored
    QCOMPARE(kinds(u), QStringList{QStringLiteral("manual_no_dates:n")});
  }

  void actuals() {
    Task a = task("a", 3);
    a.actualStart = QStringLiteral("2026-11-03");
    a.percentComplete = 10;
    QCOMPARE(span(run({a}), "a"), QStringLiteral("2026-11-03/2026-11-05"));
    // An actual start wins over the links (it happened).
    QCOMPARE(span(run({task("p", 5), a}, {dep("p", "a")}), "a"), QStringLiteral("2026-11-03/2026-11-05"));
    a.actualFinish = QStringLiteral("2026-11-10");
    a.percentComplete = 100;
    const Schedule s = run({a, task("b", 1)}, {dep("a", "b")});
    QCOMPARE(span(s, "a"), QStringLiteral("2026-11-03/2026-11-10"));
    QVERIFY(at(s, "a").fixed);
    QVERIFY(!at(s, "a").critical);  // done tasks aren't critical
    QCOMPARE(span(s, "b"), QStringLiteral("2026-11-11/2026-11-11"));
  }

  // ---- summaries ----

  void summaries() {
    // s = {a (2), b (3)}, a → b; s → x.
    const QList<Task> ts = {task("s"), task("a", 2, "s"), task("b", 3, "s"), task("x", 1)};
    const Schedule s = run(ts, {dep("a", "b"), dep("s", "x")});
    QCOMPARE(span(s, "a"), QStringLiteral("2026-11-02/2026-11-03"));
    QCOMPARE(span(s, "b"), QStringLiteral("2026-11-04/2026-11-06"));
    QCOMPARE(span(s, "s"), QStringLiteral("2026-11-02/2026-11-06"));
    QVERIFY(at(s, "s").summary);
    QCOMPARE(span(s, "x"), QStringLiteral("2026-11-09/2026-11-09"));
    QVERIFY(at(s, "s").critical);
    // A link into a summary bounds every leaf in it.
    const Schedule y = run({task("y", 2), task("s"), task("a", 2, "s"), task("b", 3, "s")},
                           {dep("y", "s"), dep("a", "b")});
    QCOMPARE(span(y, "a"), QStringLiteral("2026-11-04/2026-11-05"));
    QCOMPARE(span(y, "b"), QStringLiteral("2026-11-06/2026-11-10"));
    QCOMPARE(span(y, "s"), QStringLiteral("2026-11-04/2026-11-10"));
    // Nested: the inner summary's leaves are bounded too.
    const Schedule n = run({task("y", 1), task("s"), task("t", 1, "s"), task("a", 1, "t")}, {dep("y", "s")});
    QCOMPARE(span(n, "a"), QStringLiteral("2026-11-03/2026-11-03"));
    QCOMPARE(span(n, "t"), QStringLiteral("2026-11-03/2026-11-03"));
  }

  // ---- no start, errors ----

  void noProjectStart() {
    Task anchored = task("b", 1);
    anchored.constraintType = QStringLiteral("SNET");
    anchored.constraintDate = QStringLiteral("2026-11-04");
    const Schedule s = run({task("a", 2), anchored, task("c", 1)}, {dep("b", "c")}, QDate());
    QCOMPARE(span(s, "a"), QStringLiteral("unscheduled"));
    QCOMPARE(span(s, "b"), QStringLiteral("2026-11-04/2026-11-04"));
    QCOMPARE(span(s, "c"), QStringLiteral("2026-11-05/2026-11-05"));  // anchored by its predecessor
    QCOMPARE(kinds(s), QStringList{QStringLiteral("no_start:a")});
    QCOMPARE(s.finish, d("2026-11-05"));
  }

  void cyclesAreErrors() {
    ScheduleInput in;
    in.projectStart = kMon;
    in.tasks = {task("a"), task("b"), task("c")};
    in.links = {dep("a", "b"), dep("b", "c"), dep("c", "a")};
    auto r = computeSchedule(in);
    QVERIFY(!r);
    QCOMPARE(r.error().code, QStringLiteral("cycle"));
    const QJsonArray path = r.error().details.value(QStringLiteral("path")).toArray();
    QCOMPARE(path.size(), 4);
    QCOMPARE(path.first(), path.last());
    // A link from a task to its own summary.
    in.tasks = {task("s"), task("a", 1, "s")};
    in.links = {dep("a", "s")};
    QCOMPARE(computeSchedule(in).error().code, QStringLiteral("cycle"));
    in.links = {dep("s", "a")};
    QCOMPARE(computeSchedule(in).error().code, QStringLiteral("cycle"));
    // Links to tasks not in the input are ignored.
    in.tasks = {task("a")};
    in.links = {dep("a", "gone")};
    QVERIFY(computeSchedule(in).ok());
  }

  // ---- ripple ----

  void ripple() {
    const QList<Dependency> links = {dep("a", "b"), dep("c", "d")};
    const Schedule before = run({task("a", 3), task("b", 2), task("c", 1), task("d", 1)}, links);
    const Schedule after = run({task("a", 5), task("b", 2), task("c", 1), task("d", 1)}, links);
    const Ripple r = diffSchedules(before, after);
    QCOMPARE(r.moved.size(), 2);  // a finishes later, b moves
    QCOMPARE(r.moved.at(0).task, QStringLiteral("a"));
    QCOMPARE(r.moved.at(0).startDelta, 0);
    QCOMPARE(r.moved.at(0).finishDelta, 2);  // Wed -> Fri
    QCOMPARE(r.moved.at(1).task, QStringLiteral("b"));
    QCOMPARE(r.moved.at(1).startDelta, 4);  // Thu 5 -> Mon 9
    QCOMPARE(r.finishDelta, 4);             // Fri 6 -> Tue 10
    QVERIFY(!r.criticalChanged);
    QVERIFY(diffSchedules(before, before).moved.isEmpty());
    // c → d becomes the longer path: the critical set changes.
    const Schedule longer = run({task("a", 3), task("b", 2), task("c", 9), task("d", 1)}, links);
    QVERIFY(diffSchedules(before, longer).criticalChanged);
  }

  // ---- properties ----

  void randomPlansSatisfyEveryLink() {
    const quint32 seeds[] = {3, 17, 256, 4099, 65537, 900001};
    for (quint32 seed : seeds) {
      QRandomGenerator rng(seed);
      const int n = 50 + static_cast<int>(rng.bounded(450));
      QList<Task> tasks;
      for (int i = 0; i < n; ++i) {
        Task t = rng.bounded(10) == 0 ? milestone("x") : task("x", 1 + rng.bounded(10));
        t.id = QStringLiteral("t%1").arg(i);
        tasks << t;
      }
      QList<Dependency> links;
      const char* types[] = {"FS", "FS", "FS", "SS", "FF"};
      for (int k = 0; k < n * 2; ++k) {
        const int a = static_cast<int>(rng.bounded(n - 1));
        const int b = a + 1 + static_cast<int>(rng.bounded(n - a - 1));
        Dependency l;
        l.id = QString::number(k);
        l.predecessor = tasks.at(a).id;
        l.successor = tasks.at(b).id;
        l.type = QLatin1String(types[rng.bounded(5)]);
        l.lag = static_cast<int>(rng.bounded(7)) - 2;
        links << l;
      }
      const Schedule s = run(tasks, links);
      const WorkCalendar cal;
      // In working days from the project start: start of a day, end of a day.
      auto sp = [&](const QDate& x) { return cal.workingDaysBetween(kMon, x); };
      auto ep = [&](const QDate& x) { return cal.workingDaysBetween(kMon, x) + (cal.isWorking(x) ? 1 : 0); };
      for (const Dependency& l : links) {
        const ScheduledTask& p = *s.task(l.predecessor);
        const ScheduledTask& q = *s.task(l.successor);
        QVERIFY(p.scheduled && q.scheduled);
        const QString ctx = QStringLiteral("seed %1 %2 %3%4%5").arg(seed).arg(l.predecessor, l.type, l.successor).arg(l.lag);
        // Positions: a task occupies [sp(start), ep(finish)). A milestone is
        // a point, at ep(date) when a finish drives it and sp(date) when a
        // start does; the output doesn't say which, so take the lenient one
        // (earliest as a predecessor, latest as a successor).
        qint64 pStart = sp(p.start);
        qint64 pFinish = ep(p.finish);
        qint64 qStart = sp(q.start);
        qint64 qFinish = ep(q.finish);
        if (p.milestone) pStart = pFinish = sp(p.start);
        if (q.milestone) qStart = qFinish = ep(q.start);
        if (l.type == QLatin1String("FS")) {
          QVERIFY2(qStart >= pFinish + l.lag, qPrintable(ctx));
        } else if (l.type == QLatin1String("SS")) {
          QVERIFY2(qStart >= pStart + l.lag, qPrintable(ctx));
        } else {
          QVERIFY2(qFinish >= pFinish + l.lag, qPrintable(ctx));
        }
      }
      // No constraints and no manual tasks: float is never negative, and
      // the critical tasks reach from the start to the finish.
      bool startsAtStart = false;
      bool reachesFinish = false;
      for (const QString& id : s.order) {
        const ScheduledTask& t = *s.task(id);
        QVERIFY2(t.totalFloat >= 0, qPrintable(QStringLiteral("seed %1 %2").arg(seed).arg(id)));
        QVERIFY(t.freeFloat <= t.totalFloat);
        if (t.critical && t.start == kMon) startsAtStart = true;
        if (t.critical && t.finish == s.finish) reachesFinish = true;
        QCOMPARE(t.critical, t.totalFloat == 0);
      }
      QVERIFY(startsAtStart);
      QVERIFY(reachesFinish);
      QVERIFY(s.violations.isEmpty());
      // The order links come in changes nothing.
      QList<Dependency> shuffled = links;
      std::reverse(shuffled.begin(), shuffled.end());
      const Schedule again = run(tasks, shuffled);
      for (const QString& id : s.order) {
        QCOMPARE(again.task(id)->start, s.task(id)->start);
        QCOMPARE(again.task(id)->finish, s.task(id)->finish);
        QCOMPARE(again.task(id)->totalFloat, s.task(id)->totalFloat);
      }
      QCOMPARE(again.critical, s.critical);
    }
  }

  void twoThousandTasksFourThousandLinks() {
    QRandomGenerator rng(2026);
    QList<Task> tasks;
    // 200 summaries of 9 tasks each, plus 200 top-level tasks.
    for (int g = 0; g < 200; ++g) {
      Task s = task("x");
      s.id = QStringLiteral("s%1").arg(g);
      tasks << s;
      for (int i = 0; i < 9; ++i) {
        Task t = task("x", 1 + rng.bounded(8));
        t.id = QStringLiteral("s%1-%2").arg(g).arg(i);
        t.parent = s.id;
        tasks << t;
      }
    }
    for (int i = 0; i < 200; ++i) {
      Task t = task("x", 1 + rng.bounded(5));
      t.id = QStringLiteral("t%1").arg(i);
      tasks << t;
    }
    QStringList leaves;
    for (const Task& t : tasks) {
      if (!t.id.startsWith(QLatin1Char('s')) || t.id.contains(QLatin1Char('-'))) leaves << t.id;
    }
    QList<Dependency> links;
    const char* types[] = {"FS", "FS", "SS", "FF"};
    while (links.size() < 4000) {
      const int a = static_cast<int>(rng.bounded(leaves.size() - 1));
      const int b = a + 1 + static_cast<int>(rng.bounded(leaves.size() - a - 1));
      Dependency l;
      l.id = QString::number(links.size());
      l.predecessor = leaves.at(a);
      l.successor = leaves.at(b);
      l.type = QLatin1String(types[rng.bounded(4)]);
      l.lag = static_cast<int>(rng.bounded(5)) - 1;
      links << l;
    }
    QCOMPARE(tasks.size(), 2200);
    ScheduleInput in;
    in.projectStart = kMon;
    in.tasks = tasks;
    in.links = links;
    QVERIFY(computeSchedule(in).ok());  // warm up
    QElapsedTimer t;
    t.start();
    const auto r = computeSchedule(in);
    const qint64 ms = t.elapsed();
    QVERIFY(r.ok());
    qInfo("2,200 tasks, 4,000 links: %lld ms", ms);
#if defined(__SANITIZE_ADDRESS__) || !defined(QT_NO_DEBUG)
    QVERIFY2(ms < 200, qPrintable(QString::number(ms)));
#else
    QVERIFY2(ms < 50, qPrintable(QString::number(ms)));
#endif
  }
};

QTEST_GUILESS_MAIN(TestSchedule)
#include "tst_schedule.moc"
