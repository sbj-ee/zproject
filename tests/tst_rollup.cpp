// Summary-task roll-ups in core (core/Rollup.hpp, SPEC.md §5.3): weighting,
// rounding, status rules, nesting, and the edit check for summaries.
#include <QTest>

#include "core/Rollup.hpp"

using namespace zproject;

namespace {

Task leaf(const QString& id, const QString& parent, double duration, int percent, const QString& status) {
  Task t;
  t.id = id;
  t.project = QStringLiteral("p");
  t.parent = parent;
  t.title = id;
  t.duration = duration;
  if (duration == 0) t.type = QStringLiteral("milestone");
  t.percentComplete = percent;
  t.status = status;
  if (percent > 0) t.actualStart = QStringLiteral("2026-11-20");
  if (percent == 100) t.actualFinish = QStringLiteral("2026-11-21");
  return t;
}

Task summary(const QString& id, const QString& parent = {}) {
  Task t;
  t.id = id;
  t.project = QStringLiteral("p");
  t.parent = parent;
  t.title = id;
  return t;
}

const Task& byId(const QList<Task>& ts, const QString& id) {
  for (const Task& t : ts) {
    if (t.id == id) return t;
  }
  qFatal("no task %s", qPrintable(id));
}

const QString kTodo = QStringLiteral("todo");
const QString kWip = QStringLiteral("in_progress");
const QString kDone = QStringLiteral("done");

}  // namespace

class TstRollup : public QObject {
  Q_OBJECT
 private slots:
  void durationWeighted() {
    QList<Task> ts{summary(QStringLiteral("s")), leaf(QStringLiteral("a"), QStringLiteral("s"), 3, 100, kDone),
                   leaf(QStringLiteral("b"), QStringLiteral("s"), 1, 0, kTodo)};
    const auto changed = rollUpSummaries(ts);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(byId(ts, QStringLiteral("s")).percentComplete, 75);  // (3×100 + 1×0) / 4
    QCOMPARE(byId(ts, QStringLiteral("s")).status, kWip);
    QCOMPARE(byId(ts, QStringLiteral("s")).actualStart, QStringLiteral("2026-11-20"));
    QVERIFY(byId(ts, QStringLiteral("s")).actualFinish.isEmpty());
    QVERIFY(validateTask(byId(ts, QStringLiteral("s"))).isEmpty());
    // Leaves are never touched.
    QCOMPARE(byId(ts, QStringLiteral("b")).percentComplete, 0);
    // A second pass changes nothing.
    QVERIFY(rollUpSummaries(ts).isEmpty());
  }

  void milestonesCarryNoWeight() {
    QList<Task> ts{summary(QStringLiteral("s")), leaf(QStringLiteral("a"), QStringLiteral("s"), 2, 50, kWip),
                   leaf(QStringLiteral("m"), QStringLiteral("s"), 0, 100, kDone)};
    rollUpSummaries(ts);
    QCOMPARE(byId(ts, QStringLiteral("s")).percentComplete, 50);
    // Only milestones: the plain average.
    QList<Task> ms{summary(QStringLiteral("s")), leaf(QStringLiteral("m1"), QStringLiteral("s"), 0, 100, kDone),
                   leaf(QStringLiteral("m2"), QStringLiteral("s"), 0, 0, kTodo)};
    rollUpSummaries(ms);
    QCOMPARE(byId(ms, QStringLiteral("s")).percentComplete, 50);
    QCOMPARE(byId(ms, QStringLiteral("s")).status, kWip);
  }

  void roundingNeverClaimsDoneOrNothing() {
    QList<Task> ts{summary(QStringLiteral("s")), leaf(QStringLiteral("a"), QStringLiteral("s"), 999, 100, kDone),
                   leaf(QStringLiteral("b"), QStringLiteral("s"), 1, 0, kTodo)};
    rollUpSummaries(ts);
    QCOMPARE(byId(ts, QStringLiteral("s")).percentComplete, 99);  // 99.9 is not done
    QList<Task> low{summary(QStringLiteral("s")), leaf(QStringLiteral("a"), QStringLiteral("s"), 1, 1, kWip),
                    leaf(QStringLiteral("b"), QStringLiteral("s"), 999, 0, kTodo)};
    rollUpSummaries(low);
    QCOMPARE(byId(low, QStringLiteral("s")).percentComplete, 1);  // 0.001 is not nothing
  }

  void statusRules() {
    auto statusOf = [](const QStringList& statuses) {
      QList<Task> ts{summary(QStringLiteral("s"))};
      int i = 0;
      for (const QString& st : statuses) {
        const int pct = st == QLatin1String("done") ? 100 : st == QLatin1String("in_progress") ? 10 : 0;
        ts.append(leaf(QStringLiteral("c%1").arg(i++), QStringLiteral("s"), 1, pct, st));
      }
      rollUpSummaries(ts);
      return byId(ts, QStringLiteral("s")).status;
    };
    QCOMPARE(statusOf({kDone, kDone}), kDone);
    QCOMPARE(statusOf({kTodo, kTodo}), kTodo);
    QCOMPARE(statusOf({kDone, kTodo}), kWip);
    QCOMPARE(statusOf({kTodo, kWip}), kWip);
    QCOMPARE(statusOf({kDone, kWip}), kWip);
  }

  void nestedSummariesRollUpThroughEachOther() {
    // A ─┬─ B ─┬─ b1 (3 d, done)
    //    │     └─ b2 (1 d, todo)
    //    └─ c (4 d, todo)
    QList<Task> ts{summary(QStringLiteral("A")), summary(QStringLiteral("B"), QStringLiteral("A")),
                   leaf(QStringLiteral("b1"), QStringLiteral("B"), 3, 100, kDone),
                   leaf(QStringLiteral("b2"), QStringLiteral("B"), 1, 0, kTodo),
                   leaf(QStringLiteral("c"), QStringLiteral("A"), 4, 0, kTodo)};
    const auto changed = rollUpSummaries(ts);
    QCOMPARE(changed.size(), 2);
    QCOMPARE(ts[changed[0]].id, QStringLiteral("B"));  // children before parents
    QCOMPARE(byId(ts, QStringLiteral("B")).percentComplete, 75);
    QCOMPARE(byId(ts, QStringLiteral("A")).percentComplete, 38);  // 300 / 8 = 37.5, by leaf work
    QCOMPARE(byId(ts, QStringLiteral("A")).status, kWip);
    QCOMPARE(byId(ts, QStringLiteral("A")).actualStart, QStringLiteral("2026-11-20"));
  }

  void deletedChildrenAreIgnored() {
    QList<Task> ts{summary(QStringLiteral("s")), leaf(QStringLiteral("a"), QStringLiteral("s"), 1, 100, kDone),
                   leaf(QStringLiteral("gone"), QStringLiteral("s"), 1, 0, kTodo)};
    ts[2].deleted = true;
    rollUpSummaries(ts);
    QCOMPARE(byId(ts, QStringLiteral("s")).status, kDone);
    QCOMPARE(byId(ts, QStringLiteral("s")).percentComplete, 100);
    QCOMPARE(byId(ts, QStringLiteral("s")).actualFinish, QStringLiteral("2026-11-21"));
    QVERIFY(validateTask(byId(ts, QStringLiteral("s"))).isEmpty());
    QCOMPARE(summaryIds(ts), QSet<QString>{QStringLiteral("s")});
    ts[1].deleted = true;
    QVERIFY(summaryIds(ts).isEmpty());
  }

  void summaryEditCheck() {
    const Task cur = leaf(QStringLiteral("s"), {}, 1, 50, kWip);
    Task next = cur;
    next.title = QStringLiteral("renamed");
    QVERIFY(checkSummaryEdit(cur, next).isEmpty());
    next.status = kDone;
    next.percentComplete = 100;
    const auto errs = checkSummaryEdit(cur, next);
    QCOMPARE(errs.size(), 2);
    QCOMPARE(errs[0].field, QStringLiteral("percentComplete"));
    QCOMPARE(errs[1].field, QStringLiteral("status"));
  }
};

QTEST_GUILESS_MAIN(TstRollup)
#include "tst_rollup.moc"
