#pragma once

#include <QString>

#include <memory>

#include "store/Database.hpp"
#include "store/Store.hpp"

namespace zproject::store {

// The SQLite implementation of Store (SPEC.md §16.1). One instance owns one
// connection and is the only writer; it is not thread-safe.
class SqliteStore final : public Store {
 public:
  // Opens or creates the database at `path` (":memory:" for tests), applies
  // the connection settings and runs pending migrations. Refuses a database
  // whose schema is newer than this build.
  static Result<std::unique_ptr<SqliteStore>> open(const QString& path);

  int schemaVersion() const override { return schemaVersion_; }
  Database& database() { return db_; }

  Result<Project> createProject(const Project& p, const WriteContext& ctx) override;
  Result<Project> project(const QString& id, bool includeDeleted = false) override;
  Result<QList<Project>> projects(bool includeDeleted = false) override;
  Result<Project> updateProject(const Project& p, qint64 expectedRev, const WriteContext& ctx) override;
  Result<Project> deleteProject(const QString& id, qint64 expectedRev, const WriteContext& ctx) override;

  Result<Task> createTask(const Task& t, const WriteContext& ctx) override;
  Result<Task> task(const QString& id, bool includeDeleted = false) override;
  Result<QList<Task>> tasks(const QString& projectId, bool includeDeleted = false) override;
  Result<Task> updateTask(const Task& t, qint64 expectedRev, const WriteContext& ctx) override;
  Result<Task> deleteTask(const QString& id, qint64 expectedRev, const WriteContext& ctx) override;

  Result<QList<ChangeRecord>> changesSince(qint64 since, int limit = 500) override;
  Result<QList<ChangeRecord>> changesFor(const QString& entityKind, const QString& entityId,
                                         qint64 since = 0, int limit = 500) override;

 private:
  SqliteStore(Database db, int schemaVersion) : db_(std::move(db)), schemaVersion_(schemaVersion) {}

  // Hybrid clock for change rows: max(wall clock, last + 1 ms), UTC ISO 8601.
  QString nextTimestamp();
  Status appendChange(const QString& kind, const QString& id, const QJsonObject& before,
                      const QJsonObject& after, const WriteContext& ctx, const QString& ts);
  Status writeProject(const Project& p, bool insert);
  Status writeTask(const Task& t, bool insert);
  Status checkParent(const Task& t);
  Status tombstoneTask(const Task& t, const WriteContext& ctx, const QString& ts);

  Database db_;
  int schemaVersion_ = 0;
  qint64 lastTsMs_ = 0;
};

}  // namespace zproject::store
