#pragma once

#include <QDate>
#include <QString>
#include <QStringList>

#include "core/Error.hpp"
#include "store/SqliteStore.hpp"

namespace zproject::store {

struct BackupResult {
  QString databaseFile;  // <dir>/zproject-YYYY-MM-DD.db
  QString exportFile;    // <dir>/zproject-YYYY-MM-DD.json
  QStringList removed;   // pruned by the retention rule
};

// Nightly backup (SPEC.md §16.3): `VACUUM INTO` a dated snapshot, check it
// with PRAGMA integrity_check, write a full JSON export next to it (mode
// 0600 both), then prune to 30 daily and 12 monthly. A snapshot of the same
// day is replaced.
Result<BackupResult> backup(SqliteStore& store, const QString& dir, const QDate& today);

// Retention: keep every backup from the last 30 days (counting today) and the
// newest one of each of the last 12 months; delete the rest. Returns the
// removed files. Only zproject-YYYY-MM-DD.{db,json} names are touched.
Result<QStringList> pruneBackups(const QString& dir, const QDate& today);

// The full-fidelity JSON export (also a backup format, SPEC.md §7.9):
// every project, tombstones included, with its tasks. No token data.
Result<QJsonObject> exportAll(SqliteStore& store);

// `zprojectd --restore FILE`, with the service stopped: checks FILE's
// integrity, migrates a copy to this build's schema, keeps the current
// database as <db>.before-restore-<timestamp>, then moves the copy into
// place.
Result<QString> restore(const QString& from, const QString& databasePath);

// Before a schema migration (SPEC.md §16.3): if the database at `path`
// exists and is older than this build's schema, VACUUM INTO
// <dir>/zproject-pre-migration-s<from>-<timestamp>.db and return that path;
// otherwise return an empty string.
Result<QString> preMigrationSnapshot(const QString& path, const QString& dir);

}  // namespace zproject::store
