#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

#include "core/Error.hpp"
#include "core/Model.hpp"

namespace zproject::store {

// One row of the append-only `changes` table (SPEC.md §5.1).
struct ChangeRecord {
  qint64 seq = 0;
  QString ts;
  QString actor;
  QString entityKind;  // "project", "task", ...
  QString entityId;
  QJsonObject before;  // changed fields only
  QJsonObject after;
  QString requestId;
};

// Who is writing, recorded in every change row.
struct WriteContext {
  QString actor = QStringLiteral("system");  // token id
  QString requestId;
};

// The storage interface (SPEC.md §4.1, §16.2). SqliteStore is the only
// implementation; the interface keeps Postgres possible later. Every write
// runs in one transaction that updates the current-state tables and appends
// to `changes`. `expectedRev` implements If-Match (412 rev_mismatch).
class Store {
 public:
  virtual ~Store() = default;

  virtual int schemaVersion() const = 0;

  // Projects
  virtual Result<Project> createProject(const Project& p, const WriteContext& ctx) = 0;
  virtual Result<Project> project(const QString& id, bool includeDeleted = false) = 0;
  virtual Result<QList<Project>> projects(bool includeDeleted = false) = 0;
  virtual Result<Project> updateProject(const Project& p, qint64 expectedRev, const WriteContext& ctx) = 0;
  // Tombstones the project and all its tasks.
  virtual Result<Project> deleteProject(const QString& id, qint64 expectedRev, const WriteContext& ctx) = 0;

  // Tasks (the WBS tree: parent + order)
  virtual Result<Task> createTask(const Task& t, const WriteContext& ctx) = 0;
  virtual Result<Task> task(const QString& id, bool includeDeleted = false) = 0;
  // Ordered depth-first by WBS position (parent before children, siblings by order).
  virtual Result<QList<Task>> tasks(const QString& projectId, bool includeDeleted = false) = 0;
  virtual Result<Task> updateTask(const Task& t, qint64 expectedRev, const WriteContext& ctx) = 0;
  // Tombstones the task and its whole subtree.
  virtual Result<Task> deleteTask(const QString& id, qint64 expectedRev, const WriteContext& ctx) = 0;

  // The sync feed: change rows with seq > since, oldest first (limit <= 500).
  virtual Result<QList<ChangeRecord>> changesSince(qint64 since, int limit = 500) = 0;
};

}  // namespace zproject::store
