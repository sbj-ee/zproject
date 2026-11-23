#pragma once

#include <QJsonObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <optional>

#include "core/Error.hpp"
#include "core/Model.hpp"
#include "core/WorkCalendar.hpp"

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

  // Dependencies (SPEC.md §5.1, §5.2): links between two live tasks of one
  // project; `project` comes from the tasks. checkDependency validates a new
  // link without writing (dry runs): its fields, both tasks live and in one
  // project (400), no live link between the same two tasks in the same
  // direction (409 conflict, reason duplicate), and no cycle through the
  // links and the task tree (409 cycle, details.path). A task's links are
  // tombstoned with it and restored with it; a move or a restore that would
  // close a cycle is refused (409 cycle).
  virtual Result<Dependency> checkDependency(const Dependency& d) = 0;
  virtual Result<Dependency> createDependency(const Dependency& d, const WriteContext& ctx) = 0;
  virtual Result<Dependency> dependency(const QString& id, bool includeDeleted = false) = 0;
  virtual Result<QList<Dependency>> dependencies(const QString& projectId, bool includeDeleted = false) = 0;
  // Only type and lag change; the tasks a link joins are fixed (400).
  virtual Result<Dependency> updateDependency(const Dependency& d, qint64 expectedRev, const WriteContext& ctx) = 0;
  virtual Result<Dependency> deleteDependency(const QString& id, qint64 expectedRev, const WriteContext& ctx) = 0;
  // For dry runs: the live links touching any of these tasks (what deleting
  // them tombstones), and the links restoring a task would bring back.
  virtual Result<QList<Dependency>> linksTouching(const QString& projectId, const QSet<QString>& taskIds) = 0;
  virtual Result<QList<Dependency>> planRestoreLinks(const QString& taskId) = 0;
  // 409 cycle if the project's live links would loop with its tasks as given
  // (e.g. a task tree after a move); dry runs use it as the writes do.
  virtual Status checkLinkCycles(const QList<Task>& tasks) = 0;

  // Calendars (SPEC.md §5.1, §6.1, §7.4): one pool for the installation.
  // Standard (kStandardCalendarId) always exists: it has no base and can't be
  // deleted. checkCalendar validates without writing (dry runs): the fields,
  // a live base, no loop of bases (400). A calendar that a live project,
  // task or calendar still uses can't be deleted (409 conflict, reason
  // in_use, details.usedBy); deleting one tombstones its exceptions. A
  // project's or task's `calendar` must name a live calendar (400).
  virtual Status checkCalendar(const CalendarDef& c) = 0;
  virtual Result<CalendarDef> createCalendar(const CalendarDef& c, const WriteContext& ctx) = 0;
  virtual Result<CalendarDef> calendar(const QString& id, bool includeDeleted = false) = 0;
  virtual Result<QList<CalendarDef>> calendars(bool includeDeleted = false) = 0;
  virtual Result<CalendarDef> updateCalendar(const CalendarDef& c, qint64 expectedRev, const WriteContext& ctx) = 0;
  virtual Status checkCalendarDelete(const QString& id) = 0;
  virtual Result<CalendarDef> deleteCalendar(const QString& id, qint64 expectedRev, const WriteContext& ctx) = 0;
  virtual Result<QList<CalendarException>> calendarExceptions(const QString& calendarId, bool includeDeleted = false) = 0;
  virtual Result<CalendarException> calendarException(const QString& id, bool includeDeleted = false) = 0;
  virtual Result<CalendarException> addCalendarException(const CalendarException& e, const WriteContext& ctx) = 0;
  virtual Result<CalendarException> deleteCalendarException(const QString& id, qint64 expectedRev,
                                                            const WriteContext& ctx) = 0;
  // A year's holidays (core/Holidays.hpp; country "US") not already on the
  // calendar as a holiday that day: planImportHolidays lists them,
  // importHolidays adds them (each its own exception, deletable alone).
  virtual Result<QList<CalendarException>> planImportHolidays(const QString& calendarId, const QString& country,
                                                              int year) = 0;
  virtual Result<QList<CalendarException>> importHolidays(const QString& calendarId, const QString& country, int year,
                                                          const WriteContext& ctx) = 0;
  // Every live calendar, resolved through its bases, by id (the scheduler's input).
  virtual Result<QHash<QString, WorkCalendar>> workCalendars() = 0;

  // The sync feed: change rows with seq > since, oldest first (limit <= 500).
  virtual Result<QList<ChangeRecord>> changesSince(qint64 since, int limit = 500) = 0;
  // One entity's history, oldest first.
  virtual Result<QList<ChangeRecord>> changesFor(const QString& entityKind, const QString& entityId,
                                                 qint64 since = 0, int limit = 500) = 0;
};

}  // namespace zproject::store
