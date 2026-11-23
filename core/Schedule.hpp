#pragma once

#include <QDate>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include "core/Error.hpp"
#include "core/Model.hpp"
#include "core/WorkCalendar.hpp"

// The CPM scheduling engine (SPEC.md §6.2, §7.2; scheduling plan S2). Pure,
// deterministic Qt Core code shared by the server (which stores the
// results, S5) and the desktop client. Nothing here reads or writes storage.
//
// Conventions
// - Time is counted in working days of each task's own calendar (task →
//   project → Standard). A task of duration d (> 0, rounded up to whole
//   days) starting on working day S finishes on the (d − 1)-th working day
//   after S: a 3-day task starting Monday finishes Wednesday.
// - Links: FS — the successor starts after the predecessor finishes; SS —
//   it starts no earlier than the predecessor starts; FF — it finishes no
//   earlier than the predecessor finishes. `lag` whole working days of the
//   successor's calendar are added (negative: a lead). A start is always
//   on a working day of the task's own calendar.
// - A milestone is a point in time. One driven by a finish (FS or FF, from
//   a task or a milestone) shows that finish's date, as MS Project does; one
//   driven by a start (the project start, SNET, SS) shows that day.
// - Summaries are never scheduled themselves. As a predecessor a summary is
//   its span (earliest leaf start, latest leaf finish); a link into a
//   summary bounds every leaf below it (FS/SS: their starts, FF: their
//   finishes), so its earliest leaf starts no sooner than the link allows.
//   A link between a task and its own ancestor is a cycle.
// - Manual tasks keep their stored dates; tasks with an actual finish are
//   fixed; an actual start fixes the start. These are never moved, and a
//   manual task that starts or finishes too early for a link is reported
//   (link_broken), never fixed.
// - With no project start, an auto task is scheduled only when something
//   anchors it (SNET, an actual start, a scheduled predecessor); otherwise
//   it stays unscheduled with a no_start violation (plan Q3).
// - Backward pass from the project finish: the latest finish, or the target
//   finish when that is later. FNLT bounds a task's late finish; a deadline
//   only reports (plan Q5). Total float = late finish − early finish; a task
//   that isn't done is critical when its total float ≤ 0 (plan Q4).
// - Ties in the order of work break by the input order (pass tasks in
//   outline order), so the same input always gives the same schedule.
namespace zproject {

struct ScheduleInput {
  QDate projectStart;       // invalid: none
  QDate projectFinish;      // optional target finish
  QString projectCalendar;  // empty: Standard
  QList<Task> tasks;        // one project's live tasks, in outline order
  QList<Dependency> links;  // its live links
  // Resolved calendars by id; Standard needn't be listed. An unknown id is
  // reported (unknown_calendar) and the next calendar out is used.
  QHash<QString, WorkCalendar> calendars;
};

struct ScheduledTask {
  QString id;
  bool scheduled = false;
  bool summary = false;
  bool milestone = false;
  bool fixed = false;  // manual, or done (actual finish): never moved
  QDate start;
  QDate finish;
  QDate lateStart;
  QDate lateFinish;
  qint64 totalFloat = 0;  // working days of the task's calendar
  qint64 freeFloat = 0;
  bool critical = false;
};

// Reported, never fixed (SPEC.md §6.2 step 5). Kinds: no_start,
// manual_no_dates, link_broken (`other` = the predecessor), constraint_missed
// (FNLT), deadline_missed, unknown_calendar.
struct ScheduleViolation {
  QString kind;
  QString task;
  QString other;
  QString message;
  bool operator==(const ScheduleViolation&) const = default;
};

struct Schedule {
  QHash<QString, ScheduledTask> tasks;  // every input task, summaries included
  QStringList order;                    // the input order
  QDate finish;                         // the latest scheduled finish; invalid if none
  QStringList critical;                 // critical leaf tasks, by start then input order
  QList<ScheduleViolation> violations;  // in input order of their task
  const ScheduledTask* task(const QString& id) const {
    const auto it = tasks.constFind(id);
    return it == tasks.constEnd() ? nullptr : &it.value();
  }
};

// Errors: a cycle (409 `cycle`, details.path = the task ids around it,
// first repeated at the end). The store refuses cyclic writes, so this
// only guards the engine.
Result<Schedule> computeSchedule(const ScheduleInput& in);

// What a change does to the plan (the dry-run `ripple`, SPEC.md §9.3):
// tasks whose dates moved (deltas in calendar days; a task that gained or
// lost its dates is listed with zero deltas), the change in the project's
// finish, and whether the set of critical tasks changed.
struct RippleMove {
  QString task;
  QDate startBefore;
  QDate startAfter;
  QDate finishBefore;
  QDate finishAfter;
  qint64 startDelta = 0;
  qint64 finishDelta = 0;
};
struct Ripple {
  QList<RippleMove> moved;  // in the after schedule's order
  qint64 finishDelta = 0;
  bool criticalChanged = false;
};
Ripple diffSchedules(const Schedule& before, const Schedule& after);

}  // namespace zproject
