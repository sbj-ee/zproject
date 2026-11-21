#include "Backup.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include "core/Version.hpp"
#include "store/Migrations.hpp"

namespace zproject::store {
namespace {

constexpr auto kOwnerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

Status integrityCheck(const QString& path) {
  auto db = Database::openReadOnly(path);
  if (!db) return db.error();
  auto r = db.value().scalar(QStringLiteral("PRAGMA integrity_check"));
  if (!r) return r.error();
  if (r->toString() != QLatin1String("ok")) {
    return Error::internal(QStringLiteral("integrity_check failed on %1: %2").arg(path, r->toString()));
  }
  return Ok{};
}

QString stamp() {
  return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
}

}  // namespace

Result<QJsonObject> exportAll(SqliteStore& store) {
  auto projects = store.projects(true);
  if (!projects) return projects.error();
  QJsonArray out;
  for (const Project& p : projects.value()) {
    auto tasks = store.tasks(p.id, true);
    if (!tasks) return tasks.error();
    QJsonArray ts;
    for (const Task& t : tasks.value()) ts.append(toJson(t));
    QJsonObject po = toJson(p);
    po.insert(QStringLiteral("tasks"), ts);
    out.append(po);
  }
  return QJsonObject{{QStringLiteral("format"), QStringLiteral("zproject-export")},
                     {QStringLiteral("formatVersion"), 1},
                     {QStringLiteral("appVersion"), appVersion()},
                     {QStringLiteral("schemaVersion"), store.schemaVersion()},
                     {QStringLiteral("exportedAt"),
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                     {QStringLiteral("projects"), out}};
}

Result<BackupResult> backup(SqliteStore& store, const QString& dir, const QDate& today) {
  if (!QDir().mkpath(dir)) return Error::internal(QStringLiteral("cannot create backup directory %1").arg(dir));
  QFile::setPermissions(dir, kOwnerOnly | QFileDevice::ExeOwner);
  const QString base = QDir(dir).filePath(QStringLiteral("zproject-%1").arg(today.toString(Qt::ISODate)));
  BackupResult result;
  result.databaseFile = base + QStringLiteral(".db");
  result.exportFile = base + QStringLiteral(".json");

  // VACUUM INTO refuses an existing file: write a temporary, then rename.
  const QString tmp = result.databaseFile + QStringLiteral(".tmp");
  QFile::remove(tmp);
  auto st = store.database().prepare(QStringLiteral("VACUUM INTO ?1"));
  if (!st) return st.error();
  st.value().bind(1, tmp);
  if (auto s = st.value().run(); !s) {
    QFile::remove(tmp);
    return s.error();
  }
  QFile::setPermissions(tmp, kOwnerOnly);
  if (auto s = integrityCheck(tmp); !s) {
    QFile::remove(tmp);
    return s.error();
  }
  QFile::remove(result.databaseFile);
  if (!QFile::rename(tmp, result.databaseFile)) {
    return Error::internal(QStringLiteral("cannot move %1 into place").arg(tmp));
  }

  auto exported = exportAll(store);
  if (!exported) return exported.error();
  QSaveFile json(result.exportFile);
  if (!json.open(QIODevice::WriteOnly)) {
    return Error::internal(QStringLiteral("cannot write %1").arg(result.exportFile));
  }
  json.setPermissions(kOwnerOnly);
  json.write(QJsonDocument(exported.value()).toJson(QJsonDocument::Indented));
  if (!json.commit()) return Error::internal(QStringLiteral("cannot write %1").arg(result.exportFile));
  QFile::setPermissions(result.exportFile, kOwnerOnly);

  auto removed = pruneBackups(dir, today);
  if (!removed) return removed.error();
  result.removed = removed.value();
  return result;
}

Result<QStringList> pruneBackups(const QString& dir, const QDate& today) {
  static const QRegularExpression re(QStringLiteral("^zproject-(\\d{4}-\\d{2}-\\d{2})\\.(db|json)$"));
  QDir d(dir);
  QMap<QDate, QStringList> byDate;
  for (const QString& name : d.entryList(QDir::Files)) {
    const auto m = re.match(name);
    if (!m.hasMatch()) continue;
    const QDate date = QDate::fromString(m.captured(1), Qt::ISODate);
    if (date.isValid()) byDate[date].append(name);
  }
  QSet<QDate> keep;
  QMap<int, QDate> newestPerMonth;  // year*12+month -> newest date
  for (auto it = byDate.constBegin(); it != byDate.constEnd(); ++it) {
    const QDate date = it.key();
    if (date > today.addDays(-30) && date <= today) keep.insert(date);
    const int month = date.year() * 12 + date.month() - 1;
    if (!newestPerMonth.contains(month) || newestPerMonth[month] < date) newestPerMonth[month] = date;
  }
  const int thisMonth = today.year() * 12 + today.month() - 1;
  for (auto it = newestPerMonth.constBegin(); it != newestPerMonth.constEnd(); ++it) {
    if (it.key() > thisMonth - 12 && it.key() <= thisMonth) keep.insert(it.value());
  }
  QStringList removed;
  for (auto it = byDate.constBegin(); it != byDate.constEnd(); ++it) {
    if (keep.contains(it.key())) continue;
    for (const QString& name : it.value()) {
      if (!d.remove(name)) return Error::internal(QStringLiteral("cannot remove %1").arg(d.filePath(name)));
      removed.append(d.filePath(name));
    }
  }
  return removed;
}

Result<QString> restore(const QString& from, const QString& databasePath) {
  if (!QFileInfo::exists(from)) return Error::notFound(QStringLiteral("%1 does not exist").arg(from));
  if (auto s = integrityCheck(from); !s) return s.error();

  const QString tmp = databasePath + QStringLiteral(".restore-tmp");
  for (const QString& f : {tmp, tmp + QStringLiteral("-wal"), tmp + QStringLiteral("-shm")}) QFile::remove(f);
  if (!QFile::copy(from, tmp)) return Error::internal(QStringLiteral("cannot copy %1 to %2").arg(from, tmp));
  QFile::setPermissions(tmp, kOwnerOnly);
  {
    // Migrates the copy to this build's schema; refuses a newer one.
    auto store = SqliteStore::open(tmp);
    if (!store) {
      QFile::remove(tmp);
      return store.error();
    }
    if (auto s = store.value()->database().exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")); !s) {
      return s.error();
    }
  }
  for (const QString& f : {tmp + QStringLiteral("-wal"), tmp + QStringLiteral("-shm")}) QFile::remove(f);

  QString kept;
  if (QFileInfo::exists(databasePath)) {
    {
      // Fold the live database's WAL into it before moving it aside.
      auto old = Database::open(databasePath);
      if (!old) return old.error();
      if (auto s = old.value().exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")); !s) return s.error();
    }
    kept = databasePath + QStringLiteral(".before-restore-") + stamp();
    if (!QFile::rename(databasePath, kept)) {
      return Error::internal(QStringLiteral("cannot move %1 aside").arg(databasePath));
    }
    for (const QString& f : {databasePath + QStringLiteral("-wal"), databasePath + QStringLiteral("-shm")}) {
      QFile::remove(f);
    }
  }
  if (!QFile::rename(tmp, databasePath)) {
    return Error::internal(QStringLiteral("cannot move %1 into place").arg(tmp));
  }
  return kept;
}

Result<QString> preMigrationSnapshot(const QString& path, const QString& dir) {
  if (path == QLatin1String(":memory:") || !QFileInfo::exists(path)) return QString();
  auto db = Database::open(path);
  if (!db) return db.error();
  auto version = schemaVersion(db.value());
  if (!version) return version.error();
  if (version.value() == 0 || version.value() >= headSchemaVersion()) return QString();
  if (!QDir().mkpath(dir)) return Error::internal(QStringLiteral("cannot create backup directory %1").arg(dir));
  const QString file = QDir(dir).filePath(
      QStringLiteral("zproject-pre-migration-s%1-%2.db").arg(version.value()).arg(stamp()));
  auto st = db.value().prepare(QStringLiteral("VACUUM INTO ?1"));
  if (!st) return st.error();
  st.value().bind(1, file);
  if (auto s = st.value().run(); !s) return s.error();
  QFile::setPermissions(file, kOwnerOnly);
  return file;
}

}  // namespace zproject::store
