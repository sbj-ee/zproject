#include <QTemporaryDir>
#include <QTest>

#include "store/Database.hpp"
#include "store/Migrations.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::store;

namespace {
qint64 count(Database& db, const QString& sql) {
  auto v = db.scalar(sql);
  return v ? v->toLongLong() : -1;
}
}  // namespace

class TstMigrations : public QObject {
  Q_OBJECT

 private slots:
  void builtinListIsContiguous() {
    const auto& m = builtinMigrations();
    QVERIFY(!m.isEmpty());
    for (qsizetype i = 0; i < m.size(); ++i) {
      QCOMPARE(m[i].version, static_cast<int>(i) + 1);
      QVERIFY(m[i].name.startsWith(QStringLiteral("%1_").arg(m[i].version, 4, 10, QLatin1Char('0'))));
      QVERIFY(!m[i].sql.trimmed().isEmpty());
    }
    QCOMPARE(headSchemaVersion(), m.last().version);
  }

  // CI applies every migration from schema 1 to head (SPEC.md §16.4): apply
  // them one at a time, each step on its own, and check user_version.
  void stepwiseToHead() {
    auto db = Database::open(QStringLiteral(":memory:"));
    QVERIFY(db.ok());
    const auto& all = builtinMigrations();
    for (qsizetype n = 1; n <= all.size(); ++n) {
      auto v = migrate(db.value(), all.mid(0, n));
      QVERIFY2(v.ok(), qPrintable(v.ok() ? QString() : v.error().message));
      QCOMPARE(*v, static_cast<int>(n));
      QCOMPARE(*schemaVersion(db.value()), static_cast<int>(n));
    }
  }

  void freshFileDatabase() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("zproject.db"));
    auto store = SqliteStore::open(path);
    QVERIFY2(store.ok(), qPrintable(store.ok() ? QString() : store.error().message));
    QCOMPARE(store.value()->schemaVersion(), headSchemaVersion());

    Database& db = store.value()->database();
    QCOMPARE(db.scalar(QStringLiteral("PRAGMA journal_mode"))->toString().toLower(), QStringLiteral("wal"));
    QCOMPARE(count(db, QStringLiteral("PRAGMA foreign_keys")), 1);
    QCOMPARE(count(db, QStringLiteral("PRAGMA synchronous")), 1);  // NORMAL
    QCOMPARE(count(db, QStringLiteral("PRAGMA busy_timeout")), 5000);
    for (const char* table : {"meta", "project", "task", "changes", "audit"}) {
      QCOMPARE(count(db, QStringLiteral("SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name = '%1'")
                             .arg(QLatin1String(table))),
               1);
    }
    QCOMPARE(db.scalar(QStringLiteral("SELECT value FROM meta WHERE key = 'currency'"))->toString(),
             QStringLiteral("USD"));
    const QString installationId =
        db.scalar(QStringLiteral("SELECT value FROM meta WHERE key = 'installationId'"))->toString();
    QCOMPARE(installationId.size(), 20);

    // Re-opening is a no-op: same schema, same installation id.
    store.value().reset();
    auto again = SqliteStore::open(path);
    QVERIFY(again.ok());
    QCOMPARE(again.value()->schemaVersion(), headSchemaVersion());
    QCOMPARE(again.value()->database().scalar(
                 QStringLiteral("SELECT value FROM meta WHERE key = 'installationId'"))->toString(),
             installationId);
  }

  void refusesNewerSchema() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("future.db"));
    {
      auto db = Database::open(path);
      QVERIFY(db.ok());
      QVERIFY(db.value().exec(QStringLiteral("PRAGMA user_version = %1").arg(headSchemaVersion() + 1)).ok());
    }
    auto store = SqliteStore::open(path);
    QVERIFY(!store.ok());
    QVERIFY(store.error().message.contains(QStringLiteral("newer")));
  }

  void failedMigrationRollsBackEverything() {
    auto db = Database::open(QStringLiteral(":memory:"));
    QVERIFY(db.ok());
    QList<Migration> list = builtinMigrations();
    list.append({static_cast<int>(list.size()) + 1, QStringLiteral("9999_broken.sql"),
                 QStringLiteral("CREATE TABLE fine (x INTEGER); THIS IS NOT SQL;")});
    auto v = migrate(db.value(), list);
    QVERIFY(!v.ok());
    QVERIFY(v.error().message.contains(QStringLiteral("9999_broken.sql")));
    // Nothing from any migration in the batch survived, and the version is untouched.
    QCOMPARE(*schemaVersion(db.value()), 0);
    QCOMPARE(count(db.value(), QStringLiteral("SELECT COUNT(*) FROM sqlite_schema")), 0);
  }

  void rejectsGapInList() {
    auto db = Database::open(QStringLiteral(":memory:"));
    QList<Migration> list{{1, QStringLiteral("0001_a.sql"), QStringLiteral("SELECT 1;")},
                          {3, QStringLiteral("0003_c.sql"), QStringLiteral("SELECT 1;")}};
    QVERIFY(!migrate(db.value(), list).ok());
  }
};

QTEST_GUILESS_MAIN(TstMigrations)
#include "tst_migrations.moc"
