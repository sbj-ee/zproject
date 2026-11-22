#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

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

  // Moving a task within its project (SPEC.md §7.1, §9.4): `parent` empty =
  // top level; `position` is the 0-based index among the destination's live
  // siblings (the task itself not counted), nullopt = last. planMove returns
  // the rows the move would write with their new values: the task first, then
  // the siblings whose order changes (renumbered 1..n). Empty = nothing to do.
  // Refuses a cycle (409 cycle), a milestone or missing parent, another
  // project's parent and a position out of range (400 validation).
  virtual Result<QList<Task>> planMove(const QString& id, const QString& parent, std::optional<int> position) = 0;
  virtual Result<Task> moveTask(const QString& id, const QString& parent, std::optional<int> position,
                                qint64 expectedRev, const WriteContext& ctx) = 0;
  // Restoring a deleted task (SPEC.md §5, §9.4): the task and the part of its
  // subtree that the same delete tombstoned (tasks deleted earlier stay
  // deleted). planRestore returns those rows as they are now, the task first.
  // 409 conflict if the task isn't deleted, or its project or parent is
  // deleted, or its parent has become a milestone.
  virtual Result<QList<Task>> planRestore(const QString& id) = 0;
  virtual Result<Task> restoreTask(const QString& id, qint64 expectedRev, const WriteContext& ctx) = 0;

  // The sync feed: change rows with seq > since, oldest first (limit <= 500).
  virtual Result<QList<ChangeRecord>> changesSince(qint64 since, int limit = 500) = 0;
  // One entity's history, oldest first.
  virtual Result<QList<ChangeRecord>> changesFor(const QString& entityKind, const QString& entityId,
                                                 qint64 since = 0, int limit = 500) = 0;
};

}  // namespace zproject::store
