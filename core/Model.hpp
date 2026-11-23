#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

#include "core/Error.hpp"

// The plan entities (SPEC.md §5). Phase 1a starts with projects and tasks;
// dependencies, calendars and the rest follow in later pull requests.
namespace zproject {

// Fields every entity has (SPEC.md §5).
struct EntityBase {
  QString id;          // 20 lower-case hex (core/Id.hpp)
  QString createdAt;   // UTC ISO 8601 with milliseconds
  QString updatedAt;
  bool deleted = false;  // tombstone; restorable
  qint64 rev = 0;        // incremented on every change; If-Match
};

struct Project : EntityBase {
  QString name;
  QString description;  // Markdown
  QString objectives;
  QString scope;
  QString outOfScope;
  QString successCriteria;
  QString owner;
  QString sponsor;
  QString status = QStringLiteral("active");  // active, on_hold, done, cancelled
  QString rag = QStringLiteral("green");      // green, amber, red
  int priority = 2;                           // 1..4
  QString start;                              // YYYY-MM-DD or empty
  QString finish;                             // optional target
  std::optional<double> budget;
  QString color;   // #rrggbb or empty
  qint64 order = 0;
};

struct Task : EntityBase {
  QString project;
  QString parent;  // empty = top level
  qint64 order = 0;
  QString title;
  QString notes;                                // Markdown
  QString type = QStringLiteral("task");        // task, milestone (summary is derived)
  QString mode = QStringLiteral("auto");        // auto, manual
  double duration = 1;                          // working days; 0 iff milestone
  QString start;                                // stored for manual tasks
  QString finish;
  QString constraintType = QStringLiteral("ASAP");  // ASAP, SNET, FNLT
  QString constraintDate;
  QString deadline;
  QString calendar;  // calendar id; empty = the project's (stored and served from S4)
  int percentComplete = 0;                      // 0..100
  QString actualStart;
  QString actualFinish;
  QString status = QStringLiteral("todo");      // todo, in_progress, done
  int priority = 2;                             // 1..4
  double fixedCost = 0;
};

// A link between two tasks of one project (SPEC.md §5.1): the successor
// is driven by the predecessor's finish (FS) or start (SS), or must finish
// with it (FF), offset by `lag` whole working days of the successor's
// calendar (negative: a lead). SF is phase 2.
struct Dependency : EntityBase {
  QString project;
  QString predecessor;
  QString successor;
  QString type = QStringLiteral("FS");  // FS, SS, FF
  int lag = 0;                          // working days, may be negative
};

inline constexpr int kMaxLagDays = 3650;  // ten years either way

// One problem with one field: {"field": "...", "message": "..."}.
struct FieldError {
  QString field;
  QString message;
};

// Pure validation of a single entity's own fields (SPEC.md §5.2). Rules that
// need other rows (parent exists, no cycles) are checked by the store.
QList<FieldError> validateProject(const Project& p);
QList<FieldError> validateTask(const Task& t);
// A link's own fields; the store checks the tasks exist, share the project,
// and that the link is not a duplicate and makes no cycle.
QList<FieldError> validateDependency(const Dependency& d);

// Turns field errors into a validation Error (empty list -> no error).
std::optional<Error> toValidationError(const QList<FieldError>& errors);

bool isValidDate(const QString& yyyyMmDd);

// JSON with the API's camelCase field names. fromJson ignores unknown keys
// and leaves missing ones at their defaults.
QJsonObject toJson(const Project& p);
QJsonObject toJson(const Task& t);
Project projectFromJson(const QJsonObject& o);
Task taskFromJson(const QJsonObject& o);

// The fields whose values differ between two JSON objects, as
// {before: {...changed fields...}, after: {...}} for change records.
struct FieldDiff {
  QJsonObject before;
  QJsonObject after;
  bool empty() const { return after.isEmpty() && before.isEmpty(); }
};
FieldDiff diffFields(const QJsonObject& before, const QJsonObject& after);

}  // namespace zproject
