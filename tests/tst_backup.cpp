// Backup, retention and restore (SPEC.md §16.3).
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#include "store/Backup.hpp"
#include "store/Migrations.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::store;

class TestBackup : public QObject {
  Q_OBJECT
 private slots:
  void backupAndRestore() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dbPath = tmp.filePath(QStringLiteral("zproject.db"));
    const QString dir = tmp.filePath(QStringLiteral("backups"));
    QString projectId;
    {
      auto s = SqliteStore::open(dbPath);
      QVERIFY(s.ok());
      Project p;
      p.name = QStringLiteral("Garage");
      auto created = s.value()->createProject(p, {});
      QVERIFY(created.ok());
      projectId = created->id;
      Task t;
      t.project = projectId;
      t.title = QStringLiteral("Pour slab");
      QVERIFY(s.value()->createTask(t, {}).ok());

      auto r = backup(*s.value(), dir, QDate(2026, 11, 21));
      QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().message));
      QVERIFY(QFile::exists(r->databaseFile));
      QVERIFY(QFile::exists(r->exportFile));
      QCOMPARE(QFile(r->databaseFile).permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
      QFile json(r->exportFile);
      QVERIFY(json.open(QIODevice::ReadOnly));
      const QJsonObject doc = QJsonDocument::fromJson(json.readAll()).object();
      QCOMPARE(doc.value(QStringLiteral("format")).toString(), QStringLiteral("zproject-export"));
      QCOMPARE(doc.value(QStringLiteral("projects")).toArray().size(), 1);
      QCOMPARE(doc.value(QStringLiteral("projects")).toArray()[0].toObject().value(QStringLiteral("tasks")).toArray().size(), 1);
      // Same day again: replaced, not an error.
      QVERIFY(backup(*s.value(), dir, QDate(2026, 11, 21)).ok());

      // A later change that the restore must undo.
      Project p2;
      p2.name = QStringLiteral("Shed");
      QVERIFY(s.value()->createProject(p2, {}).ok());
    }
    auto kept = restore(QDir(dir).filePath(QStringLiteral("zproject-2026-11-21.db")), dbPath);
    QVERIFY2(kept.ok(), kept.ok() ? "" : qPrintable(kept.error().message));
    QVERIFY(QFile::exists(kept.value()));
    auto s = SqliteStore::open(dbPath);
    QVERIFY(s.ok());
    QCOMPARE(s.value()->projects()->size(), 1);
    QCOMPARE(s.value()->projects()->at(0).id, projectId);
  }

  void restoreRefusesGarbage() {
    QTemporaryDir tmp;
    const QString bad = tmp.filePath(QStringLiteral("bad.db"));
    QFile f(bad);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("this is not a database");
    f.close();
    QVERIFY(!restore(bad, tmp.filePath(QStringLiteral("z.db"))).ok());
    QVERIFY(!QFile::exists(tmp.filePath(QStringLiteral("z.db"))));
  }

  void retention() {
    QTemporaryDir tmp;
    const QDate today(2026, 11, 21);
    // One backup a day for 500 days, plus an unrelated file.
    for (int i = 0; i < 500; ++i) {
      const QString base = tmp.filePath(QStringLiteral("zproject-%1").arg(today.addDays(-i).toString(Qt::ISODate)));
      for (const char* ext : {".db", ".json"}) {
        QFile f(base + QLatin1String(ext));
        QVERIFY(f.open(QIODevice::WriteOnly));
      }
    }
    QFile other(tmp.filePath(QStringLiteral("notes.txt")));
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.close();

    auto removed = pruneBackups(tmp.path(), today);
    QVERIFY(removed.ok());
    QStringList left = QDir(tmp.path()).entryList({QStringLiteral("zproject-*.db")}, QDir::Files, QDir::Name);
    // 30 daily (Oct 23 .. Nov 21) + the newest of each of the 12 months
    // Dec 2025 .. Nov 2026; Oct and Nov's newest are already daily ones.
    QCOMPARE(left.size(), 30 + 10);
    QVERIFY(left.contains(QStringLiteral("zproject-2026-10-23.db")));
    QVERIFY(!left.contains(QStringLiteral("zproject-2026-10-22.db")));
    QVERIFY(left.contains(QStringLiteral("zproject-2026-09-30.db")));
    QVERIFY(left.contains(QStringLiteral("zproject-2025-12-31.db")));
    QVERIFY(!left.contains(QStringLiteral("zproject-2025-11-30.db")));
    QVERIFY(QFile::exists(tmp.filePath(QStringLiteral("notes.txt"))));
    QVERIFY(QFile::exists(tmp.filePath(QStringLiteral("zproject-2025-12-31.json"))));
  }

  void preMigrationSnapshotOnlyWhenOlder() {
    QTemporaryDir tmp;
    const QString dbPath = tmp.filePath(QStringLiteral("z.db"));
    const QString dir = tmp.filePath(QStringLiteral("b"));
    QCOMPARE(preMigrationSnapshot(dbPath, dir).value(), QString());  // no database yet
    {
      auto db = Database::open(dbPath);
      QVERIFY(db.ok());
      QList<Migration> first{builtinMigrations().constFirst()};
      QCOMPARE(migrate(db.value(), first).value(), 1);
    }
    auto snap = preMigrationSnapshot(dbPath, dir);
    QVERIFY(snap.ok());
    QVERIFY(snap->contains(QStringLiteral("pre-migration-s1-")));
    QVERIFY(QFile::exists(snap.value()));
    QVERIFY(SqliteStore::open(dbPath).ok());
    QCOMPARE(preMigrationSnapshot(dbPath, dir).value(), QString());  // now at head
  }
};

QTEST_GUILESS_MAIN(TestBackup)
#include "tst_backup.moc"
