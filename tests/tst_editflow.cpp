// The desktop write flow without a GUI (plan §4.4-§4.5): PATCH bodies from
// edits, dry-run previews and the 412 field merge.
#include <QJsonArray>
#include <QTest>

#include "client/EditFlow.hpp"

using namespace zproject::client;
using Kind = ConflictMerge::Kind;

class TestEditFlow : public QObject {
  Q_OBJECT

  static QJsonObject project() {
    return {{QStringLiteral("id"), QStringLiteral("aaaaaaaaaaaaaaaaaaaa")},
            {QStringLiteral("rev"), 3},
            {QStringLiteral("updatedAt"), QStringLiteral("2026-11-22T20:00:00.000Z")},
            {QStringLiteral("name"), QStringLiteral("Kitchen")},
            {QStringLiteral("owner"), QStringLiteral("Alex")},
            {QStringLiteral("rag"), QStringLiteral("green")},
            {QStringLiteral("priority"), 2},
            {QStringLiteral("budget"), QJsonValue()},
            {QStringLiteral("start"), QJsonValue()},
            {QStringLiteral("description"), QString()}};
  }

 private slots:
  void bodyHasOnlyChangedFields() {
    QJsonObject edited = project();
    edited.insert(QStringLiteral("owner"), QStringLiteral("Sam"));
    edited.insert(QStringLiteral("priority"), 2.0);       // same number
    edited.insert(QStringLiteral("start"), QJsonValue());  // still null
    edited.insert(QStringLiteral("budget"), 1200);
    QCOMPARE(changedFields(project(), edited), (QStringList{QStringLiteral("budget"), QStringLiteral("owner")}));
    const QJsonObject body = changedBody(project(), edited);
    QCOMPARE(body.size(), 2);
    QCOMPARE(body.value(QStringLiteral("budget")).toDouble(), 1200.0);
    // A missing key and null are the same.
    QJsonObject sparse = project();
    sparse.remove(QStringLiteral("start"));
    QVERIFY(changedFields(sparse, project()).isEmpty());
  }

  void previewParseAndRule() {
    const QJsonObject edit{
        {QStringLiteral("dryRun"), true},
        {QStringLiteral("changes"),
         QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("update")},
                                {QStringLiteral("entityKind"), QStringLiteral("project")},
                                {QStringLiteral("entityId"), QStringLiteral("p1")},
                                {QStringLiteral("after"), QJsonObject{{QStringLiteral("owner"), QStringLiteral("Sam")}}}}}},
        {QStringLiteral("ripple"), QJsonObject{}}};
    auto p = DryRunPreview::parse(edit);
    QVERIFY(p.ok());
    QCOMPARE(p->changes.size(), 1);
    QCOMPARE(p->changes[0].after.value(QStringLiteral("owner")).toString(), QStringLiteral("Sam"));
    QVERIFY(p->confirmId.isEmpty());
    // Q6: a plain edit applies directly; destructive or rippling writes preview.
    QVERIFY(!p->needsPreview(false, QStringLiteral("project"), QStringLiteral("p1")));
    QVERIFY(p->needsPreview(true, QStringLiteral("project"), QStringLiteral("p1")));
    QVERIFY(p->needsPreview(false, QStringLiteral("project"), QStringLiteral("p2")));

    // A create (no id yet) is the entity itself.
    QJsonObject create = edit;
    create.insert(QStringLiteral("changes"),
                  QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("create")},
                                         {QStringLiteral("entityKind"), QStringLiteral("project")},
                                         {QStringLiteral("entityId"), QJsonValue()}}});
    QVERIFY(!DryRunPreview::parse(create)->needsPreview(false, QStringLiteral("project"), QString()));

    // A delete with its tasks, and a confirmId.
    QJsonObject del = edit;
    del.insert(QStringLiteral("changes"),
               QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("delete")},
                                      {QStringLiteral("entityKind"), QStringLiteral("project")},
                                      {QStringLiteral("entityId"), QStringLiteral("p1")}},
                          QJsonObject{{QStringLiteral("op"), QStringLiteral("delete")},
                                      {QStringLiteral("entityKind"), QStringLiteral("task")},
                                      {QStringLiteral("entityId"), QStringLiteral("t1")}}});
    del.insert(QStringLiteral("confirmId"), QStringLiteral("c1"));
    auto d = DryRunPreview::parse(del);
    QCOMPARE(d->otherChanges(QStringLiteral("project"), QStringLiteral("p1")), 1);
    QCOMPARE(d->confirmId, QStringLiteral("c1"));

    // Not a dry run, or malformed.
    QVERIFY(!DryRunPreview::parse(QJsonObject{{QStringLiteral("id"), QStringLiteral("p1")}}).ok());
    QVERIFY(!DryRunPreview::parse({{QStringLiteral("dryRun"), true}, {QStringLiteral("changes"), QJsonArray{1}}}).ok());
    QVERIFY(!DryRunPreview::parse({{QStringLiteral("dryRun"), true},
                                   {QStringLiteral("changes"), QJsonArray{QJsonObject{{QStringLiteral("op"), QString()}}}}})
                 .ok());
  }

  void conflictMergeKinds() {
    const QJsonObject base = project();
    QJsonObject mine = base;
    mine.insert(QStringLiteral("owner"), QStringLiteral("Sam"));             // both, differently
    mine.insert(QStringLiteral("description"), QStringLiteral("New cabinets"));  // only mine
    mine.insert(QStringLiteral("priority"), 1);                               // both, the same
    QJsonObject theirs = base;
    theirs.insert(QStringLiteral("rev"), 5);
    theirs.insert(QStringLiteral("updatedAt"), QStringLiteral("2026-11-22T21:00:00.000Z"));
    theirs.insert(QStringLiteral("owner"), QStringLiteral("Robin"));
    theirs.insert(QStringLiteral("rag"), QStringLiteral("amber"));  // only theirs
    theirs.insert(QStringLiteral("priority"), 1);
    theirs.insert(QStringLiteral("order"), 9);  // server-managed: ignored

    const ConflictMerge m(base, mine, theirs);
    QCOMPARE(m.theirRev(), 5);
    QHash<QString, Kind> kinds;
    for (const auto& f : m.fields()) kinds.insert(f.name, f.kind);
    QCOMPARE(kinds.size(), 4);
    QCOMPARE(kinds.value(QStringLiteral("owner")), Kind::Conflict);
    QCOMPARE(kinds.value(QStringLiteral("description")), Kind::Mine);
    QCOMPARE(kinds.value(QStringLiteral("rag")), Kind::Theirs);
    QCOMPARE(kinds.value(QStringLiteral("priority")), Kind::Same);
    QCOMPARE(m.conflicts(), 1);

    // Keep Mine: everything I changed that differs from the server.
    QJsonObject keep = m.resolution(true);
    QCOMPARE(keep, (QJsonObject{{QStringLiteral("owner"), QStringLiteral("Sam")},
                                {QStringLiteral("description"), QStringLiteral("New cabinets")}}));
    // Take Theirs: only the fields the server didn't touch.
    QCOMPARE(m.resolution(false), (QJsonObject{{QStringLiteral("description"), QStringLiteral("New cabinets")}}));

    // Nothing of mine left once theirs wins everywhere.
    QJsonObject mine2 = base;
    mine2.insert(QStringLiteral("owner"), QStringLiteral("Sam"));
    const ConflictMerge m2(base, mine2, theirs);
    QVERIFY(m2.resolution(false).isEmpty());
    QCOMPARE(ConflictMerge::kindName(Kind::Conflict), QStringLiteral("conflict"));
  }

  void displayValues() {
    QCOMPARE(displayValue(QJsonValue()), QString());
    QCOMPARE(displayValue(1200.0), QStringLiteral("1200"));
    QCOMPARE(displayValue(12.5), QStringLiteral("12.5"));
    QCOMPARE(displayValue(true), QStringLiteral("yes"));
    QCOMPARE(displayValue(QStringLiteral("x")), QStringLiteral("x"));
    QCOMPARE(displayValue(QJsonArray{1, 2}), QStringLiteral("[1,2]"));
  }
};

QTEST_MAIN(TestEditFlow)
#include "tst_editflow.moc"
