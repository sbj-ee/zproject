// The desktop's read-only cache (SPEC.md §16.5): file modes, applying change
// records (create, update, delete, replays), the activity cap, snapshots,
// WBS codes, reset, reopen and format changes. No tokens are ever written.
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

#include "client/Cache.hpp"

using namespace zproject;
using namespace zproject::client;

namespace {

int fileMode(const QString& path) {
  struct stat st {};
  if (::stat(QFile::encodeName(path).constData(), &st) != 0) return -1;
  return static_cast<int>(st.st_mode & 0777);
}

ChangeRecord rec(qint64 seq, const QString& kind, const QString& id, const QJsonObject& before,
                 const QJsonObject& after) {
  ChangeRecord c;
  c.seq = seq;
  c.ts = QStringLiteral("2026-11-22T19:00:%1.000Z").arg(seq % 60, 2, 10, QLatin1Char('0'));
  c.actor = QStringLiteral("tok1");
  c.entityKind = kind;
  c.entityId = id;
  c.before = before;
  c.after = after;
  return c;
}

QJsonObject project(const QString& id, const QString& name, qint64 rev = 1, int order = 1) {
  return {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("rev"), rev},
          {QStringLiteral("deleted"), false}, {QStringLiteral("order"), order}};
}

QJsonObject task(const QString& id, const QString& projectId, const QString& title, qint64 rev = 1, int order = 1) {
  return {{QStringLiteral("id"), id},       {QStringLiteral("project"), projectId}, {QStringLiteral("title"), title},
          {QStringLiteral("rev"), rev},     {QStringLiteral("deleted"), false},     {QStringLiteral("order"), order},
          {QStringLiteral("percentComplete"), 0}};
}

// A small history: project P with tasks A and B, A renamed, B deleted.
QList<ChangeRecord> history() {
  return {
      rec(1, QStringLiteral("project"), QStringLiteral("P"), {}, project(QStringLiteral("P"), QStringLiteral("Plan"))),
      rec(2, QStringLiteral("task"), QStringLiteral("A"), {}, task(QStringLiteral("A"), QStringLiteral("P"), QStringLiteral("Design"), 1, 1)),
      rec(3, QStringLiteral("task"), QStringLiteral("B"), {}, task(QStringLiteral("B"), QStringLiteral("P"), QStringLiteral("Build"), 1, 2)),
      rec(4, QStringLiteral("task"), QStringLiteral("A"),
          QJsonObject{{QStringLiteral("title"), QStringLiteral("Design")}, {QStringLiteral("rev"), 1}},
          QJsonObject{{QStringLiteral("title"), QStringLiteral("Design v2")}, {QStringLiteral("rev"), 2}}),
      rec(5, QStringLiteral("task"), QStringLiteral("B"),
          QJsonObject{{QStringLiteral("deleted"), false}, {QStringLiteral("rev"), 1}},
          QJsonObject{{QStringLiteral("deleted"), true}, {QStringLiteral("rev"), 2}}),
  };
}

}  // namespace

class TestCache : public QObject {
  Q_OBJECT

 private slots:
  void fileIsPrivate() {
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/cache/zproject/cache.db");
    {
      auto c = Cache::open(path);
      QVERIFY2(c.ok(), qPrintable(c.ok() ? QString() : c.error().message));
      QVERIFY(c.value()->isEmpty());
      QVERIFY(c.value()->apply(history()).ok());
      QCOMPARE(fileMode(path), 0600);
      QCOMPARE(fileMode(dir.path() + QStringLiteral("/cache/zproject")), 0700);
      if (QFile::exists(path + QStringLiteral("-wal"))) QCOMPARE(fileMode(path + QStringLiteral("-wal")), 0600);
    }
    QCOMPARE(fileMode(path), 0600);
    // Reopen keeps the data and the feed position.
    {
      auto c = Cache::open(path);
      QVERIFY(c.ok());
      QCOMPARE(c.value()->lastSeq(), qint64(5));
      QCOMPARE(c.value()->tasks(QStringLiteral("P")).size(), 1);
    }
    QVERIFY(Cache::removeFiles(path).ok());
    QVERIFY(!QFile::exists(path));
    auto fresh = Cache::open(path);
    QVERIFY(fresh.ok());
    QCOMPARE(fresh.value()->lastSeq(), qint64(0));
  }

  void appliesChanges() {
    auto c = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    auto r = c->apply(history());
    QVERIFY(r.ok());
    QCOMPARE(r->projects, QSet<QString>{QStringLiteral("P")});
    QCOMPARE(r->applied, 5);
    QVERIFY(!r->incomplete);
    QCOMPARE(c->lastSeq(), qint64(5));
    QCOMPARE(c->lastSeqTs(), QStringLiteral("2026-11-22T19:00:05.000Z"));
    QCOMPARE(c->projects().size(), 1);
    QCOMPARE(c->projects().first().value(QStringLiteral("name")).toString(), QStringLiteral("Plan"));
    const QList<QJsonObject> ts = c->tasks(QStringLiteral("P"));
    QCOMPARE(ts.size(), 1);  // B is deleted
    QCOMPARE(ts.first().value(QStringLiteral("title")).toString(), QStringLiteral("Design v2"));
    QCOMPARE(ts.first().value(QStringLiteral("rev")).toInt(), 2);
    QCOMPARE(ts.first().value(QStringLiteral("percentComplete")).toInt(), 0);  // merged, not replaced
    QVERIFY(!c->task(QStringLiteral("B")));
    QVERIFY(c->task(QStringLiteral("A")));

    // Replaying the same records (or older ones) is a no-op.
    r = c->apply(history());
    QVERIFY(r.ok());
    QCOMPARE(r->applied, 0);
    QVERIFY(r->projects.isEmpty());
    QCOMPARE(c->tasks(QStringLiteral("P")).first().value(QStringLiteral("title")).toString(), QStringLiteral("Design v2"));

    // Deleting the project hides it.
    QVERIFY(c->apply({rec(6, QStringLiteral("project"), QStringLiteral("P"), QJsonObject{{QStringLiteral("deleted"), false}},
                          QJsonObject{{QStringLiteral("deleted"), true}, {QStringLiteral("rev"), 2}})})
                .ok());
    QVERIFY(c->projects().isEmpty());
    QVERIFY(!c->project(QStringLiteral("P")));
  }

  void staleRecordsDontOverwriteNewerData() {
    auto c = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    // A snapshot already at rev 3 (list-polling fallback).
    QVERIFY(c->replaceSnapshot({project(QStringLiteral("P"), QStringLiteral("Plan"))},
                               {task(QStringLiteral("A"), QStringLiteral("P"), QStringLiteral("Newest"), 3)})
                .ok());
    auto r = c->apply({rec(10, QStringLiteral("task"), QStringLiteral("A"), QJsonObject{{QStringLiteral("rev"), 1}},
                           QJsonObject{{QStringLiteral("title"), QStringLiteral("Old")}, {QStringLiteral("rev"), 2}})});
    QVERIFY(r.ok());
    QCOMPARE(r->applied, 0);
    QCOMPARE(c->task(QStringLiteral("A"))->value(QStringLiteral("title")).toString(), QStringLiteral("Newest"));
    QCOMPARE(c->lastSeq(), qint64(10));
  }

  void updateToUnknownEntityIsIncomplete() {
    auto c = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    auto r = c->apply({rec(7, QStringLiteral("task"), QStringLiteral("X"), QJsonObject{{QStringLiteral("rev"), 1}},
                           QJsonObject{{QStringLiteral("title"), QStringLiteral("?")}, {QStringLiteral("rev"), 2}})});
    QVERIFY(r.ok());
    QVERIFY(r->incomplete);
  }

  void orderAndWbs() {
    auto c = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    QVERIFY(c->replaceSnapshot({project(QStringLiteral("Q"), QStringLiteral("Second"), 1, 2),
                                project(QStringLiteral("P"), QStringLiteral("First"), 1, 1)},
                               {task(QStringLiteral("b"), QStringLiteral("P"), QStringLiteral("two"), 1, 2),
                                task(QStringLiteral("a"), QStringLiteral("P"), QStringLiteral("one"), 1, 1)})
                .ok());
    QCOMPARE(c->projects().first().value(QStringLiteral("name")).toString(), QStringLiteral("First"));
    QCOMPARE(c->tasks(QStringLiteral("P")).first().value(QStringLiteral("title")).toString(), QStringLiteral("one"));
    const QJsonArray w{QJsonObject{{QStringLiteral("id"), QStringLiteral("a")}, {QStringLiteral("wbs"), QStringLiteral("1")}}};
    QVERIFY(c->setWbs(QStringLiteral("P"), w).ok());
    QCOMPARE(c->wbs(QStringLiteral("P")), w);
    QVERIFY(c->wbs(QStringLiteral("Q")).isEmpty());
  }

  void activityIsCappedAndFiltered() {
    auto c = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    QVERIFY(c->apply(history()).ok());
    QList<ChangeRecord> more;
    for (int i = 0; i < Cache::kActivityCap + 10; ++i) {
      more.append(rec(100 + i, QStringLiteral("task"), QStringLiteral("A"), QJsonObject{{QStringLiteral("rev"), 2 + i}},
                      QJsonObject{{QStringLiteral("percentComplete"), i % 100}, {QStringLiteral("rev"), 3 + i}}));
    }
    QVERIFY(c->apply(more).ok());
    const QList<ChangeRecord> all = c->activity(QString(), Cache::kActivityCap * 2);
    QCOMPARE(all.size(), Cache::kActivityCap);
    QCOMPARE(all.first().seq, qint64(100 + Cache::kActivityCap + 9));  // newest first
    QCOMPARE(c->activity(QStringLiteral("P"), 3).size(), 3);
    QVERIFY(c->activity(QStringLiteral("other"), 3).isEmpty());
    QCOMPARE(c->activity(QStringLiteral("P"), 1).first().actor, QStringLiteral("tok1"));
  }

  void metaResetAndFormat() {
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/cache.db");
    {
      auto c = std::move(Cache::open(path)).value();
      QVERIFY(c->apply(history()).ok());
      QVERIFY(c->setServer(ServerHealth{QStringLiteral("0.6.0"), 1, 2}).ok());
      const QDateTime when = QDateTime::fromString(QStringLiteral("2026-11-22T19:05:00.000Z"), Qt::ISODateWithMs);
      QVERIFY(c->markRefreshed(when).ok());
      QCOMPARE(c->refreshedAt(), when);
      QCOMPARE(c->schemaVersion(), 2);
      QCOMPARE(c->serverVersion(), QStringLiteral("0.6.0"));
      QVERIFY(c->reset().ok());
      QVERIFY(c->isEmpty());
      QCOMPARE(c->lastSeq(), qint64(0));
      QCOMPARE(c->schemaVersion(), 0);
      QVERIFY(c->apply(history()).ok());
    }
    {
      // A cache written by another format is emptied on open.
      auto db = std::move(store::Database::open(path)).value();
      QVERIFY(db.exec(QStringLiteral("UPDATE meta SET value = '999' WHERE key = 'format'")).ok());
    }
    auto c = std::move(Cache::open(path)).value();
    QVERIFY(c->isEmpty());
    QCOMPARE(c->lastSeq(), qint64(0));
  }
};

QTEST_GUILESS_MAIN(TestCache)
#include "tst_cache.moc"
