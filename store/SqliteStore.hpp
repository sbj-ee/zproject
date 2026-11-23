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
  Result<QList<Task>> planMove(const QString& id, const QString& parent, std::optional<int> position) override;
  Result<Task> moveTask(const QString& id, const QString& parent, std::optional<int> position, qint64 expectedRev,
                        const WriteContext& ctx) override;
  Result<QList<Task>> planRestore(const QString& id) override;
  Result<Task> restoreTask(const QString& id, qint64 expectedRev, const WriteContext& ctx) override;

  Result<Dependency> checkDependency(const Dependency& d) override;
  Result<Dependency> createDependency(const Dependency& d, const WriteContext& ctx) override;
  Result<Dependency> dependency(const QString& id, bool includeDeleted = false) override;
  Result<QList<Dependency>> dependencies(const QString& projectId, bool includeDeleted = false) override;
  Result<Dependency> updateDependency(const Dependency& d, qint64 expectedRev, const WriteContext& ctx) override;
  Result<Dependency> deleteDependency(const QString& id, qint64 expectedRev, const WriteContext& ctx) override;
  Result<QList<Dependency>> linksTouching(const QString& projectId, const QSet<QString>& taskIds) override;
  Result<QList<Dependency>> planRestoreLinks(const QString& taskId) override;
  Status checkLinkCycles(const QList<Task>& tasks) override;

  Status checkCalendar(const CalendarDef& c) override;
  Result<CalendarDef> createCalendar(const CalendarDef& c, const WriteContext& ctx) override;
  Result<CalendarDef> calendar(const QString& id, bool includeDeleted = false) override;
  Result<QList<CalendarDef>> calendars(bool includeDeleted = false) override;
  Result<CalendarDef> updateCalendar(const CalendarDef& c, qint64 expectedRev, const WriteContext& ctx) override;
  Status checkCalendarDelete(const QString& id) override;
  Result<CalendarDef> deleteCalendar(const QString& id, qint64 expectedRev, const WriteContext& ctx) override;
  Result<QList<CalendarException>> calendarExceptions(const QString& calendarId, bool includeDeleted = false) override;
  Result<CalendarException> calendarException(const QString& id, bool includeDeleted = false) override;
  Result<CalendarException> addCalendarException(const CalendarException& e, const WriteContext& ctx) override;
  Result<CalendarException> deleteCalendarException(const QString& id, qint64 expectedRev,
                                                    const WriteContext& ctx) override;
  Result<QList<CalendarException>> planImportHolidays(const QString& calendarId, const QString& country,
                                                      int year) override;
  Result<QList<CalendarException>> importHolidays(const QString& calendarId, const QString& country, int year,
                                                  const WriteContext& ctx) override;
  Result<QHash<QString, WorkCalendar>> workCalendars() override;
  Result<Schedule> projectSchedule(const QString& projectId) override;
  Result<int> reschedule(const QString& projectId, const WriteContext& ctx) override;
  // 400 if a write would set derived dates by hand (auto tasks, summaries);
  // the API checks it first too, so a dry run says the same.
  static Status checkDerivedDates(const Task& before, const Task& after, bool summary);

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
  // Writes `t` as the next revision of `cur` (rev + 1, updatedAt = ts) with
  // its change row.
  Status writeRevision(const Task& cur, Task t, const WriteContext& ctx, const QString& ts);
  // The ts of the entity's newest change row if that row tombstoned it, else empty.
  Result<QString> tombstoneTs(const QString& id, const QString& kind = QStringLiteral("task"));
  Status writeDependency(const Dependency& d, bool insert);
  // Writes `d` as the next revision of `cur`, with its change row.
  Status writeDependencyRevision(const Dependency& cur, Dependency d, const WriteContext& ctx, const QString& ts);
  // Tombstones the live links touching these tasks (one task delete).
  Status tombstoneLinks(const QString& projectId, const QSet<QString>& taskIds, const WriteContext& ctx,
                        const QString& ts);
  Status writeCalendar(const CalendarDef& c, bool insert);
  Status writeCalendarException(const CalendarException& e, bool insert);
  // 400 unless `id` is empty or a live calendar (a project's or task's).
  Status checkCalendarRef(const QString& id);
  // checkLinkCycles over explicit links (a candidate included).
  Status checkCycles(const QList<Task>& tasks, const QList<Dependency>& links);
  // Recomputes the project's summary tasks (core/Rollup.hpp) and writes the
  // ones that changed, each with its own revision and change row, inside the
  // caller's transaction.
  Status rollUp(const QString& projectId, const WriteContext& ctx);
  // On open: corrects summaries left stale by earlier versions (actor
  // "system"). A no-op when everything is already in step.
  Status reconcileRollups();
  // On open: reschedules every project (actor "system"); a no-op when the
  // stored dates are the schedule's.
  Status reconcileSchedules();
  // After a calendar changes: every live project.
  Status rescheduleAll(const WriteContext& ctx);


  Database db_;
  int schemaVersion_ = 0;
  qint64 lastTsMs_ = 0;
};

}  // namespace zproject::store
