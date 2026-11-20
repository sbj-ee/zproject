#include <QJsonArray>
#include <QTest>

#include "core/Id.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::store;

namespace {

const WriteContext kCtx{QStringLiteral("tok_test"), QStringLiteral("req-1")};

std::unique_ptr<SqliteStore> openMemory() {
  auto s = SqliteStore::open(QStringLiteral(":memory:"));
  if (!s.ok()) qFatal("open failed: %s", qPrintable(s.error().message));
  return std::move(s).value();
}

Project newProject(SqliteStore& s, const QString& name = QStringLiteral("Garage")) {
  Project p;
  p.name = name;
  auto r = s.createProject(p, kCtx);
  if (!r.ok()) qFatal("createProject: %s", qPrintable(r.error().message));
  return r.value();
}

Task newTask(SqliteStore& s, const QString& project, const QString& title, const QString& parent = {},
             bool milestone = false) {
  Task t;
  t.project = project;
  t.title = title;
  t.parent = parent;
  if (milestone) {
    t.type = QStringLiteral("milestone");
    t.duration = 0;
  }
  auto r = s.createTask(t, kCtx);
  if (!r.ok()) qFatal("createTask: %s", qPrintable(r.error().message));
  return r.value();
}

QStringList titles(const QList<Task>& ts) {
  QStringList out;
  for (const Task& t : ts) out.append(t.title);
  return out;
}

}  // namespace

class TstStore : public QObject {
  Q_OBJECT

 private slots:
  void createAndReadProject() {
    auto s = openMemory();
    const Project p = newProject(*s);
    QVERIFY(isValidId(p.id));
    QCOMPARE(p.rev, 1);
    QVERIFY(!p.deleted);
    QCOMPARE(p.order, 1);
    QVERIFY(p.createdAt.endsWith(QLatin1Char('Z')));
    auto back = s->project(p.id);
    QVERIFY(back.ok());
    QCOMPARE(toJson(back.value()), toJson(p));
    QCOMPARE(newProject(*s, QStringLiteral("Second")).order, 2);
    QCOMPARE(s->projects()->size(), 2);
  }

  void createRejectsInvalid() {
    auto s = openMemory();
    Project p;
    auto r = s->createProject(p, kCtx);
    QVERIFY(!r.ok());
    QCOMPARE(r.error().code, QStringLiteral("validation"));
    QCOMPARE(s->projects()->size(), 0);
    QCOMPARE(s->changesSince(0)->size(), 0);
  }

  void notFound() {
    auto s = openMemory();
    QCOMPARE(s->project(newId()).error().code, QStringLiteral("not_found"));
    QCOMPARE(s->task(newId()).error().code, QStringLiteral("not_found"));
    Task t;
    t.project = newId();
    t.title = QStringLiteral("orphan");
    QCOMPARE(s->createTask(t, kCtx).error().code, QStringLiteral("not_found"));
  }

  void updateProjectWithRev() {
    auto s = openMemory();
    Project p = newProject(*s);
    p.name = QStringLiteral("Garage v2");
    p.rag = QStringLiteral("amber");

    auto stale = s->updateProject(p, 7, kCtx);
    QVERIFY(!stale.ok());
    QCOMPARE(stale.error().code, QStringLiteral("rev_mismatch"));
    QCOMPARE(stale.error().details.value(QStringLiteral("current")).toInt(), 1);

    auto r = s->updateProject(p, 1, kCtx);
    QVERIFY(r.ok());
    QCOMPARE(r->rev, 2);
    QCOMPARE(s->project(p.id)->name, QStringLiteral("Garage v2"));

    // Same values again: no new revision and no change row.
    const qsizetype rows = s->changesSince(0)->size();
    auto same = s->updateProject(r.value(), 2, kCtx);
    QVERIFY(same.ok());
    QCOMPARE(same->rev, 2);
    QCOMPARE(s->changesSince(0)->size(), rows);
  }

  void changeRecords() {
    auto s = openMemory();
    Project p = newProject(*s);
    p.name = QStringLiteral("Renamed");
    QVERIFY(s->updateProject(p, 1, kCtx).ok());

    auto changes = s->changesSince(0);
    QVERIFY(changes.ok());
    QCOMPARE(changes->size(), 2);
    const ChangeRecord& created = changes->at(0);
    QCOMPARE(created.entityKind, QStringLiteral("project"));
    QCOMPARE(created.entityId, p.id);
    QCOMPARE(created.actor, QStringLiteral("tok_test"));
    QCOMPARE(created.requestId, QStringLiteral("req-1"));
    QVERIFY(created.before.isEmpty());
    QCOMPARE(created.after.value(QStringLiteral("name")).toString(), QStringLiteral("Garage"));

    const ChangeRecord& renamed = changes->at(1);
    QVERIFY(renamed.seq > created.seq);
    QVERIFY(renamed.ts > created.ts);  // hybrid clock is strictly increasing
    QCOMPARE(renamed.before, (QJsonObject{{QStringLiteral("name"), QStringLiteral("Garage")}}));
    QCOMPARE(renamed.after, (QJsonObject{{QStringLiteral("name"), QStringLiteral("Renamed")}}));

    QCOMPARE(s->changesSince(created.seq)->size(), 1);
    QCOMPARE(s->changesSince(0, 1)->size(), 1);
  }

  void changesAndAuditAreAppendOnly() {
    auto s = openMemory();
    newProject(*s);
    Database& db = s->database();
    QVERIFY(!db.exec(QStringLiteral("UPDATE changes SET actor = 'x'")).ok());
    QVERIFY(!db.exec(QStringLiteral("DELETE FROM changes")).ok());
    QVERIFY(db.exec(QStringLiteral(
                        "INSERT INTO audit (ts, method, path, status, prevHash, hash) "
                        "VALUES ('2026-11-20T00:00:00.000Z', 'GET', '/v1/health', 200, '', 'h1')"))
                .ok());
    QVERIFY(!db.exec(QStringLiteral("UPDATE audit SET status = 500")).ok());
    QVERIFY(!db.exec(QStringLiteral("DELETE FROM audit")).ok());
    QCOMPARE(db.scalar(QStringLiteral("SELECT COUNT(*) FROM changes"))->toLongLong(), 1);
    QCOMPARE(db.scalar(QStringLiteral("SELECT COUNT(*) FROM audit"))->toLongLong(), 1);
  }

  void wbsOrder() {
    auto s = openMemory();
    const Project p = newProject(*s);
    const Task design = newTask(*s, p.id, QStringLiteral("Design"));
    const Task build = newTask(*s, p.id, QStringLiteral("Build"));
    newTask(*s, p.id, QStringLiteral("Sketch"), design.id);
    newTask(*s, p.id, QStringLiteral("Permit"), design.id);
    newTask(*s, p.id, QStringLiteral("Frame"), build.id);
    newTask(*s, p.id, QStringLiteral("Done"), {}, true);
    QCOMPARE(titles(s->tasks(p.id).value()),
             (QStringList{"Design", "Sketch", "Permit", "Build", "Frame", "Done"}));
    // Sibling order is per parent.
    QCOMPARE(s->task(build.id)->order, 2);
    QCOMPARE(s->tasks(p.id)->at(2).order, 2);  // Permit
  }

  void parentRules() {
    auto s = openMemory();
    const Project p = newProject(*s);
    const Project other = newProject(*s, QStringLiteral("Other"));
    const Task ms = newTask(*s, p.id, QStringLiteral("Gate"), {}, true);
    const Task elsewhere = newTask(*s, other.id, QStringLiteral("Elsewhere"));

    Task child;
    child.project = p.id;
    child.title = QStringLiteral("Child");
    child.parent = ms.id;
    QCOMPARE(s->createTask(child, kCtx).error().code, QStringLiteral("validation"));  // milestone parent
    child.parent = elsewhere.id;
    QCOMPARE(s->createTask(child, kCtx).error().code, QStringLiteral("validation"));  // other project
    child.parent = newId();
    QCOMPARE(s->createTask(child, kCtx).error().code, QStringLiteral("validation"));  // missing

    // A task with children cannot become a milestone.
    const Task parent = newTask(*s, p.id, QStringLiteral("Parent"));
    newTask(*s, p.id, QStringLiteral("Kid"), parent.id);
    Task asMilestone = parent;
    asMilestone.type = QStringLiteral("milestone");
    asMilestone.duration = 0;
    QCOMPARE(s->updateTask(asMilestone, parent.rev, kCtx).error().code, QStringLiteral("validation"));

    // Tasks stay in their project.
    Task moved = elsewhere;
    moved.project = p.id;
    QCOMPARE(s->updateTask(moved, moved.rev, kCtx).error().code, QStringLiteral("validation"));
  }

  void noCyclesInTree() {
    auto s = openMemory();
    const Project p = newProject(*s);
    const Task a = newTask(*s, p.id, QStringLiteral("A"));
    const Task b = newTask(*s, p.id, QStringLiteral("B"), a.id);
    const Task c = newTask(*s, p.id, QStringLiteral("C"), b.id);

    Task aUnderC = a;
    aUnderC.parent = c.id;
    auto r = s->updateTask(aUnderC, a.rev, kCtx);
    QVERIFY(!r.ok());
    QCOMPARE(r.error().code, QStringLiteral("cycle"));
    QCOMPARE(r.error().details.value(QStringLiteral("cycle")).toArray().size(), 4);  // A C B A

    Task self = a;
    self.parent = a.id;
    QCOMPARE(s->updateTask(self, a.rev, kCtx).error().code, QStringLiteral("cycle"));

    // Moving C to the top level is fine.
    Task cTop = c;
    cTop.parent.clear();
    auto ok = s->updateTask(cTop, c.rev, kCtx);
    QVERIFY(ok.ok());
    QVERIFY(ok->parent.isEmpty());
    QCOMPARE(ok->rev, 2);
  }

  void deleteTaskTombstonesSubtree() {
    auto s = openMemory();
    const Project p = newProject(*s);
    const Task a = newTask(*s, p.id, QStringLiteral("A"));
    const Task b = newTask(*s, p.id, QStringLiteral("B"), a.id);
    newTask(*s, p.id, QStringLiteral("C"), b.id);
    const Task keep = newTask(*s, p.id, QStringLiteral("Keep"));
    const qint64 before = s->changesSince(0)->last().seq;

    QCOMPARE(s->deleteTask(a.id, 99, kCtx).error().code, QStringLiteral("rev_mismatch"));
    auto del = s->deleteTask(a.id, a.rev, kCtx);
    QVERIFY(del.ok());
    QVERIFY(del->deleted);
    QCOMPARE(del->rev, 2);
    QCOMPARE(titles(s->tasks(p.id).value()), QStringList{"Keep"});
    QCOMPARE(s->tasks(p.id, true)->size(), 4);
    QCOMPARE(s->task(b.id).error().code, QStringLiteral("not_found"));
    QVERIFY(s->task(b.id, true)->deleted);
    QCOMPARE(s->task(keep.id)->rev, 1);

    // One change row per tombstoned task, each recording deleted false -> true.
    auto rows = s->changesSince(before);
    QCOMPARE(rows->size(), 3);
    for (const ChangeRecord& c : rows.value()) {
      QCOMPARE(c.before.value(QStringLiteral("deleted")).toBool(), false);
      QCOMPARE(c.after.value(QStringLiteral("deleted")).toBool(), true);
    }
  }

  void deleteProjectTombstonesTasks() {
    auto s = openMemory();
    const Project p = newProject(*s);
    newTask(*s, p.id, QStringLiteral("A"));
    newTask(*s, p.id, QStringLiteral("B"));
    auto del = s->deleteProject(p.id, 1, kCtx);
    QVERIFY(del.ok());
    QVERIFY(del->deleted);
    QCOMPARE(s->projects()->size(), 0);
    QCOMPARE(s->projects(true)->size(), 1);
    QCOMPARE(s->tasks(p.id)->size(), 0);
    QCOMPARE(s->tasks(p.id, true)->size(), 2);
    // A deleted project takes no new tasks and no edits.
    Task t;
    t.project = p.id;
    t.title = QStringLiteral("late");
    QCOMPARE(s->createTask(t, kCtx).error().code, QStringLiteral("not_found"));
    QCOMPARE(s->updateProject(del.value(), del->rev, kCtx).error().code, QStringLiteral("not_found"));
  }

  void failedWriteLeavesNothing() {
    auto s = openMemory();
    const Project p = newProject(*s);
    const qsizetype changes = s->changesSince(0)->size();
    Task bad;
    bad.project = p.id;
    bad.title = QStringLiteral("x");
    bad.parent = newId();
    QVERIFY(!s->createTask(bad, kCtx).ok());
    QCOMPARE(s->changesSince(0)->size(), changes);
    QCOMPARE(s->tasks(p.id, true)->size(), 0);
    // The connection is usable afterwards (the transaction was rolled back).
    QVERIFY(newTask(*s, p.id, QStringLiteral("ok")).rev == 1);
  }

  void schemaChecksBackTheModel() {
    // Direct SQL that bypasses validation still hits the table CHECKs.
    auto s = openMemory();
    const Project p = newProject(*s);
    Database& db = s->database();
    const QString insert = QStringLiteral(
        "INSERT INTO task (id, createdAt, updatedAt, project, title, type, duration) "
        "VALUES ('%1', 'x', 'x', '%2', 't', '%3', %4)");
    QVERIFY(!db.exec(insert.arg(newId(), p.id, QStringLiteral("milestone"), QStringLiteral("2"))).ok());
    QVERIFY(!db.exec(insert.arg(newId(), p.id, QStringLiteral("task"), QStringLiteral("0"))).ok());
    QVERIFY(!db.exec(insert.arg(QStringLiteral("NOT-AN-ID"), p.id, QStringLiteral("task"), QStringLiteral("1"))).ok());
    QVERIFY(!db.exec(insert.arg(newId(), newId(), QStringLiteral("task"), QStringLiteral("1"))).ok());  // FK
    QVERIFY(db.exec(insert.arg(newId(), p.id, QStringLiteral("task"), QStringLiteral("1"))).ok());
  }
};

QTEST_GUILESS_MAIN(TstStore)
#include "tst_store.moc"
