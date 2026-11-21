#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include "core/Model.hpp"

using namespace zproject;

namespace {
bool hasField(const QList<FieldError>& errs, const QString& field) {
  for (const auto& e : errs) {
    if (e.field == field) return true;
  }
  return false;
}
Project validProject() {
  Project p;
  p.name = QStringLiteral("Garage rebuild");
  return p;
}
Task validTask() {
  Task t;
  t.project = QStringLiteral("0123456789abcdef0123");
  t.title = QStringLiteral("Pour slab");
  return t;
}
}  // namespace

class TstModel : public QObject {
  Q_OBJECT

 private slots:
  void dates() {
    QVERIFY(isValidDate(QStringLiteral("2026-11-20")));
    QVERIFY(isValidDate(QStringLiteral("2028-02-29")));
    QVERIFY(!isValidDate(QStringLiteral("2026-02-29")));
    QVERIFY(!isValidDate(QStringLiteral("2026-13-01")));
    QVERIFY(!isValidDate(QStringLiteral("2026-1-01")));
    QVERIFY(!isValidDate(QStringLiteral("20261120")));
    QVERIFY(!isValidDate(QStringLiteral("2026-11-20T00:00")));
    QVERIFY(!isValidDate(QString()));
  }

  void projectValid() { QVERIFY(validateProject(validProject()).isEmpty()); }

  void projectRules() {
    Project p = validProject();
    p.name = QStringLiteral("   ");
    QVERIFY(hasField(validateProject(p), QStringLiteral("name")));
    p = validProject();
    p.status = QStringLiteral("paused");
    QVERIFY(hasField(validateProject(p), QStringLiteral("status")));
    p = validProject();
    p.rag = QStringLiteral("blue");
    QVERIFY(hasField(validateProject(p), QStringLiteral("rag")));
    p = validProject();
    p.priority = 5;
    QVERIFY(hasField(validateProject(p), QStringLiteral("priority")));
    p = validProject();
    p.start = QStringLiteral("2026-12-01");
    p.finish = QStringLiteral("2026-11-01");
    QVERIFY(hasField(validateProject(p), QStringLiteral("finish")));
    p = validProject();
    p.budget = -1;
    QVERIFY(hasField(validateProject(p), QStringLiteral("budget")));
    p = validProject();
    p.color = QStringLiteral("2fa8f0");
    QVERIFY(hasField(validateProject(p), QStringLiteral("color")));
    p.color = QStringLiteral("#2fa8f0");
    QVERIFY(validateProject(p).isEmpty());
  }

  void taskValid() {
    QVERIFY(validateTask(validTask()).isEmpty());
    Task m = validTask();
    m.type = QStringLiteral("milestone");
    m.duration = 0;
    QVERIFY(validateTask(m).isEmpty());
  }

  void milestoneIffZeroDuration() {
    Task t = validTask();
    t.type = QStringLiteral("milestone");
    t.duration = 2;
    QVERIFY(hasField(validateTask(t), QStringLiteral("duration")));
    t = validTask();
    t.duration = 0;
    QVERIFY(hasField(validateTask(t), QStringLiteral("duration")));
    t.duration = -1;
    QVERIFY(hasField(validateTask(t), QStringLiteral("duration")));
  }

  void constraints() {
    Task t = validTask();
    t.constraintType = QStringLiteral("SNET");
    QVERIFY(hasField(validateTask(t), QStringLiteral("constraintDate")));
    t.constraintDate = QStringLiteral("2026-12-01");
    QVERIFY(validateTask(t).isEmpty());
    t.constraintType = QStringLiteral("ASAP");
    QVERIFY(hasField(validateTask(t), QStringLiteral("constraintDate")));
    t.constraintType = QStringLiteral("MSO");  // phase 2
    QVERIFY(hasField(validateTask(t), QStringLiteral("constraint")));
  }

  void progressRules() {
    Task t = validTask();
    t.percentComplete = 50;
    QVERIFY(hasField(validateTask(t), QStringLiteral("actualStart")));
    t.actualStart = QStringLiteral("2026-11-02");
    QVERIFY(validateTask(t).isEmpty());
    t.actualFinish = QStringLiteral("2026-11-05");
    QVERIFY(hasField(validateTask(t), QStringLiteral("percentComplete")));
    t.percentComplete = 100;
    QVERIFY(validateTask(t).isEmpty());
    t.actualFinish = QStringLiteral("2026-11-01");
    QVERIFY(hasField(validateTask(t), QStringLiteral("actualFinish")));
    t = validTask();
    t.percentComplete = 101;
    QVERIFY(hasField(validateTask(t), QStringLiteral("percentComplete")));
  }

  void validationErrorShape() {
    QVERIFY(!toValidationError({}).has_value());
    Task t = validTask();
    t.title.clear();
    t.priority = 0;
    const auto e = toValidationError(validateTask(t));
    QVERIFY(e.has_value());
    QCOMPARE(e->code, QStringLiteral("validation"));
    QCOMPARE(e->details.value(QStringLiteral("fields")).toArray().size(), 2);
  }

  void jsonRoundTrip() {
    Project p = validProject();
    p.id = QStringLiteral("0123456789abcdef0123");
    p.budget = 1500.5;
    p.start = QStringLiteral("2026-11-20");
    p.color = QStringLiteral("#2fa8f0");
    p.rev = 3;
    const Project p2 = projectFromJson(toJson(p));
    QCOMPARE(toJson(p2), toJson(p));
    QVERIFY(toJson(validProject()).value(QStringLiteral("budget")).isNull());

    Task t = validTask();
    t.parent = QStringLiteral("aaaaaaaaaaaaaaaaaaaa");
    t.constraintType = QStringLiteral("FNLT");
    t.constraintDate = QStringLiteral("2026-12-24");
    t.fixedCost = 99.25;
    QCOMPARE(toJson(taskFromJson(toJson(t))), toJson(t));
    QCOMPARE(toJson(t).value(QStringLiteral("constraint")).toString(), QStringLiteral("FNLT"));
  }

  void diff() {
    const QJsonObject a{{QStringLiteral("title"), QStringLiteral("A")},
                        {QStringLiteral("rev"), 1},
                        {QStringLiteral("updatedAt"), QStringLiteral("x")},
                        {QStringLiteral("duration"), 1}};
    QJsonObject b = a;
    b.insert(QStringLiteral("title"), QStringLiteral("B"));
    b.insert(QStringLiteral("rev"), 2);
    b.insert(QStringLiteral("updatedAt"), QStringLiteral("y"));
    const FieldDiff d = diffFields(a, b);
    QCOMPARE(d.before, (QJsonObject{{QStringLiteral("title"), QStringLiteral("A")}}));
    QCOMPARE(d.after, (QJsonObject{{QStringLiteral("title"), QStringLiteral("B")}}));
    QVERIFY(diffFields(a, a).empty());
    const FieldDiff created = diffFields({}, a);
    QVERIFY(created.before.isEmpty());
    QCOMPARE(created.after.size(), 2);  // rev and updatedAt are not field changes
  }
};

QTEST_GUILESS_MAIN(TstModel)
#include "tst_model.moc"
