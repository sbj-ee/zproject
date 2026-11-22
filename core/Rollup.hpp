#pragma once

#include <QList>
#include <QSet>
#include <QString>

#include "core/Model.hpp"

// Summary-task roll-ups (SPEC.md §5.3). A summary task is a task with at
// least one live (not deleted) child. Its percentComplete, status,
// actualStart and actualFinish are derived from its children; the server
// keeps the stored values in step on every write.
namespace zproject {

// Ids of the tasks in `tasks` that have a live child (deleted rows are
// ignored, both as parents and as children).
QSet<QString> summaryIds(const QList<Task>& tasks);

// Recomputes every summary in `tasks` (one project's rows, any order),
// bottom-up so nested summaries roll up through each other, and updates the
// four derived fields in place. Returns the positions of the rows that
// changed, children before their parents. Rows other than summaries are
// never modified.
//
// - percentComplete: weighted by duration over the summary's leaf
//   descendants, Σ(percent × duration) / Σ duration, the way MS Project
//   rolls up % complete. Milestones (duration 0) carry no weight; when no
//   leaf has any weight (only milestones) it is the plain average. Rounded
//   to the nearest integer, but never 100 unless every leaf is at 100 and
//   never 0 once any weighted work is done.
// - status (from the direct children's statuses, already rolled up): all
//   `done` -> `done`; all `todo` -> `todo`; anything else (a mix, or any
//   `in_progress`) -> `in_progress`. A status other than these three is not
//   accepted by validateTask; if one is ever added it counts as "started"
//   (in_progress) until this rule is extended.
// - actualStart: the earliest actualStart among the children (empty if none).
// - actualFinish: the latest actualFinish, only when every child has one;
//   otherwise empty.
QList<qsizetype> rollUpSummaries(QList<Task>& tasks);

// Validation errors for a client edit of a summary task's derived fields:
// one per field whose value in `next` differs from `current`. Empty when the
// edit leaves them alone (sending the current value back is fine).
QList<FieldError> checkSummaryEdit(const Task& current, const Task& next);

}  // namespace zproject
