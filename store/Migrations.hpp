#pragma once

#include <QList>
#include <QString>

#include "core/Error.hpp"
#include "store/Database.hpp"

namespace zproject::store {

// One numbered, forward-only schema migration (SPEC.md §16.4).
struct Migration {
  int version;   // 1, 2, 3, ... contiguous
  QString name;  // file name, e.g. "0001_initial.sql"
  QString sql;
};

// The migrations in store/migrations/, embedded at build time.
const QList<Migration>& builtinMigrations();

// Newest schema version this build knows.
int headSchemaVersion();

// Brings the database to the newest version in `migrations`: every pending
// migration runs in ONE transaction and PRAGMA user_version is set at the
// end. On any failure everything is rolled back and the error names the
// migration. A database newer than `migrations` is refused (never
// downgraded). Returns the schema version after the call.
Result<int> migrate(Database& db, const QList<Migration>& migrations = builtinMigrations());

Result<int> schemaVersion(Database& db);

}  // namespace zproject::store
