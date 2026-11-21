#include "Migrations.hpp"

namespace zproject::store {

int headSchemaVersion() {
  const auto& m = builtinMigrations();
  return m.isEmpty() ? 0 : m.last().version;
}

Result<int> schemaVersion(Database& db) {
  auto v = db.scalar(QStringLiteral("PRAGMA user_version"));
  if (!v) return v.error();
  return static_cast<int>(v->toLongLong());
}

Result<int> migrate(Database& db, const QList<Migration>& migrations) {
  for (qsizetype i = 0; i < migrations.size(); ++i) {
    if (migrations[i].version != static_cast<int>(i) + 1) {
      return Error::internal(QStringLiteral("migration list is not contiguous at %1").arg(migrations[i].name));
    }
  }
  const int head = migrations.isEmpty() ? 0 : migrations.last().version;

  auto current = schemaVersion(db);
  if (!current) return current.error();
  if (*current > head) {
    return Error::internal(QStringLiteral("database schema %1 is newer than this build supports (%2); "
                                          "refusing to open it")
                               .arg(*current)
                               .arg(head));
  }
  if (*current == head) return head;

  if (auto s = db.exec(QStringLiteral("BEGIN IMMEDIATE")); !s) return s.error();
  for (const Migration& m : migrations) {
    if (m.version <= *current) continue;
    if (auto s = db.exec(m.sql); !s) {
      (void)db.exec(QStringLiteral("ROLLBACK"));
      Error e = s.error();
      e.message = QStringLiteral("migration %1 failed, nothing was changed: %2").arg(m.name, e.message);
      e.details.insert(QStringLiteral("migration"), m.name);
      return e;
    }
  }
  if (auto s = db.exec(QStringLiteral("PRAGMA user_version = %1").arg(head)); !s) {
    (void)db.exec(QStringLiteral("ROLLBACK"));
    return s.error();
  }
  if (auto s = db.exec(QStringLiteral("COMMIT")); !s) {
    (void)db.exec(QStringLiteral("ROLLBACK"));
    return s.error();
  }
  return head;
}

}  // namespace zproject::store
