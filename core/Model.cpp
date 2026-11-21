#include "Model.hpp"

#include <QDate>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>

namespace zproject {
namespace {

const QStringList kProjectStatuses = {QStringLiteral("active"), QStringLiteral("on_hold"),
                                      QStringLiteral("done"), QStringLiteral("cancelled")};
const QStringList kRags = {QStringLiteral("green"), QStringLiteral("amber"), QStringLiteral("red")};
const QStringList kTaskTypes = {QStringLiteral("task"), QStringLiteral("milestone")};
const QStringList kModes = {QStringLiteral("auto"), QStringLiteral("manual")};
const QStringList kConstraints = {QStringLiteral("ASAP"), QStringLiteral("SNET"), QStringLiteral("FNLT")};
const QStringList kTaskStatuses = {QStringLiteral("todo"), QStringLiteral("in_progress"),
                                   QStringLiteral("done")};

constexpr int kMaxTitle = 500;

void checkOneOf(QList<FieldError>& errs, const QString& field, const QString& value,
                const QStringList& allowed) {
  if (!allowed.contains(value)) {
    errs.append({field, QStringLiteral("must be one of: %1").arg(allowed.join(QStringLiteral(", ")))});
  }
}

void checkDate(QList<FieldError>& errs, const QString& field, const QString& value) {
  if (!value.isEmpty() && !isValidDate(value)) {
    errs.append({field, QStringLiteral("must be a date YYYY-MM-DD")});
  }
}

void checkOrder(QList<FieldError>& errs, const QString& fromField, const QString& from,
                const QString& toField, const QString& to) {
  if (isValidDate(from) && isValidDate(to) && to < from) {
    errs.append({toField, QStringLiteral("must not be before %1").arg(fromField)});
  }
}

void checkPriority(QList<FieldError>& errs, int priority) {
  if (priority < 1 || priority > 4) {
    errs.append({QStringLiteral("priority"), QStringLiteral("must be 1 to 4")});
  }
}

QString str(const QJsonObject& o, const char* key, const QString& fallback = {}) {
  const QJsonValue v = o.value(QLatin1String(key));
  return v.isString() ? v.toString() : fallback;
}

double num(const QJsonObject& o, const char* key, double fallback) {
  const QJsonValue v = o.value(QLatin1String(key));
  return v.isDouble() ? v.toDouble() : fallback;
}

void baseToJson(QJsonObject& o, const EntityBase& e) {
  o.insert(QStringLiteral("id"), e.id);
  o.insert(QStringLiteral("createdAt"), e.createdAt);
  o.insert(QStringLiteral("updatedAt"), e.updatedAt);
  o.insert(QStringLiteral("deleted"), e.deleted);
  o.insert(QStringLiteral("rev"), e.rev);
}

void baseFromJson(EntityBase& e, const QJsonObject& o) {
  e.id = str(o, "id");
  e.createdAt = str(o, "createdAt");
  e.updatedAt = str(o, "updatedAt");
  e.deleted = o.value(QStringLiteral("deleted")).toBool(false);
  e.rev = static_cast<qint64>(num(o, "rev", 0));
}

QJsonValue optString(const QString& s) { return s.isEmpty() ? QJsonValue() : QJsonValue(s); }

}  // namespace

bool isValidDate(const QString& s) {
  static const QRegularExpression re(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
  return re.match(s).hasMatch() && QDate::fromString(s, Qt::ISODate).isValid();
}

QList<FieldError> validateProject(const Project& p) {
  QList<FieldError> errs;
  if (p.name.trimmed().isEmpty()) {
    errs.append({QStringLiteral("name"), QStringLiteral("is required")});
  } else if (p.name.size() > kMaxTitle) {
    errs.append({QStringLiteral("name"), QStringLiteral("is longer than %1 characters").arg(kMaxTitle)});
  }
  checkOneOf(errs, QStringLiteral("status"), p.status, kProjectStatuses);
  checkOneOf(errs, QStringLiteral("rag"), p.rag, kRags);
  checkPriority(errs, p.priority);
  checkDate(errs, QStringLiteral("start"), p.start);
  checkDate(errs, QStringLiteral("finish"), p.finish);
  checkOrder(errs, QStringLiteral("start"), p.start, QStringLiteral("finish"), p.finish);
  if (p.budget && (!std::isfinite(*p.budget) || *p.budget < 0)) {
    errs.append({QStringLiteral("budget"), QStringLiteral("must be a number >= 0")});
  }
  static const QRegularExpression color(QStringLiteral("^#[0-9a-fA-F]{6}$"));
  if (!p.color.isEmpty() && !color.match(p.color).hasMatch()) {
    errs.append({QStringLiteral("color"), QStringLiteral("must be #rrggbb")});
  }
  return errs;
}

QList<FieldError> validateTask(const Task& t) {
  QList<FieldError> errs;
  if (t.title.trimmed().isEmpty()) {
    errs.append({QStringLiteral("title"), QStringLiteral("is required")});
  } else if (t.title.size() > kMaxTitle) {
    errs.append({QStringLiteral("title"), QStringLiteral("is longer than %1 characters").arg(kMaxTitle)});
  }
  checkOneOf(errs, QStringLiteral("type"), t.type, kTaskTypes);
  checkOneOf(errs, QStringLiteral("mode"), t.mode, kModes);
  checkOneOf(errs, QStringLiteral("constraint"), t.constraintType, kConstraints);
  checkOneOf(errs, QStringLiteral("status"), t.status, kTaskStatuses);
  checkPriority(errs, t.priority);

  if (!std::isfinite(t.duration) || t.duration < 0) {
    errs.append({QStringLiteral("duration"), QStringLiteral("must be a number of working days >= 0")});
  } else if (t.type == QLatin1String("milestone") && t.duration != 0.0) {
    errs.append({QStringLiteral("duration"), QStringLiteral("must be 0 for a milestone")});
  } else if (t.type == QLatin1String("task") && t.duration == 0.0) {
    errs.append({QStringLiteral("duration"), QStringLiteral("0 is only for milestones")});
  }

  for (const auto& [field, value] : {std::pair{QStringLiteral("start"), t.start},
                                     std::pair{QStringLiteral("finish"), t.finish},
                                     std::pair{QStringLiteral("constraintDate"), t.constraintDate},
                                     std::pair{QStringLiteral("deadline"), t.deadline},
                                     std::pair{QStringLiteral("actualStart"), t.actualStart},
                                     std::pair{QStringLiteral("actualFinish"), t.actualFinish}}) {
    checkDate(errs, field, value);
  }
  checkOrder(errs, QStringLiteral("start"), t.start, QStringLiteral("finish"), t.finish);
  checkOrder(errs, QStringLiteral("actualStart"), t.actualStart, QStringLiteral("actualFinish"),
             t.actualFinish);

  if (t.constraintType == QLatin1String("ASAP") && !t.constraintDate.isEmpty()) {
    errs.append({QStringLiteral("constraintDate"), QStringLiteral("must be empty for ASAP")});
  } else if (t.constraintType != QLatin1String("ASAP") && t.constraintDate.isEmpty()) {
    errs.append({QStringLiteral("constraintDate"),
                 QStringLiteral("is required for %1").arg(t.constraintType)});
  }

  if (t.percentComplete < 0 || t.percentComplete > 100) {
    errs.append({QStringLiteral("percentComplete"), QStringLiteral("must be 0 to 100")});
  }
  if (!t.actualFinish.isEmpty() && t.percentComplete != 100) {
    errs.append({QStringLiteral("percentComplete"), QStringLiteral("must be 100 when actualFinish is set")});
  }
  if (t.percentComplete > 0 && t.actualStart.isEmpty()) {
    errs.append({QStringLiteral("actualStart"), QStringLiteral("is required when percentComplete > 0")});
  }
  if (!std::isfinite(t.fixedCost) || t.fixedCost < 0) {
    errs.append({QStringLiteral("fixedCost"), QStringLiteral("must be a number >= 0")});
  }
  return errs;
}

std::optional<Error> toValidationError(const QList<FieldError>& errors) {
  if (errors.isEmpty()) {
    return std::nullopt;
  }
  QJsonArray fields;
  for (const FieldError& e : errors) {
    fields.append(QJsonObject{{QStringLiteral("field"), e.field}, {QStringLiteral("message"), e.message}});
  }
  const FieldError& first = errors.first();
  return Error::validation(first.field + QLatin1Char(' ') + first.message,
                           QJsonObject{{QStringLiteral("fields"), fields}});
}

QJsonObject toJson(const Project& p) {
  QJsonObject o;
  baseToJson(o, p);
  o.insert(QStringLiteral("name"), p.name);
  o.insert(QStringLiteral("description"), p.description);
  o.insert(QStringLiteral("objectives"), p.objectives);
  o.insert(QStringLiteral("scope"), p.scope);
  o.insert(QStringLiteral("outOfScope"), p.outOfScope);
  o.insert(QStringLiteral("successCriteria"), p.successCriteria);
  o.insert(QStringLiteral("owner"), p.owner);
  o.insert(QStringLiteral("sponsor"), p.sponsor);
  o.insert(QStringLiteral("status"), p.status);
  o.insert(QStringLiteral("rag"), p.rag);
  o.insert(QStringLiteral("priority"), p.priority);
  o.insert(QStringLiteral("start"), optString(p.start));
  o.insert(QStringLiteral("finish"), optString(p.finish));
  o.insert(QStringLiteral("budget"), p.budget ? QJsonValue(*p.budget) : QJsonValue());
  o.insert(QStringLiteral("color"), optString(p.color));
  o.insert(QStringLiteral("order"), p.order);
  return o;
}

Project projectFromJson(const QJsonObject& o) {
  Project p;
  baseFromJson(p, o);
  p.name = str(o, "name");
  p.description = str(o, "description");
  p.objectives = str(o, "objectives");
  p.scope = str(o, "scope");
  p.outOfScope = str(o, "outOfScope");
  p.successCriteria = str(o, "successCriteria");
  p.owner = str(o, "owner");
  p.sponsor = str(o, "sponsor");
  p.status = str(o, "status", p.status);
  p.rag = str(o, "rag", p.rag);
  p.priority = static_cast<int>(num(o, "priority", p.priority));
  p.start = str(o, "start");
  p.finish = str(o, "finish");
  if (o.value(QStringLiteral("budget")).isDouble()) {
    p.budget = o.value(QStringLiteral("budget")).toDouble();
  }
  p.color = str(o, "color");
  p.order = static_cast<qint64>(num(o, "order", 0));
  return p;
}

QJsonObject toJson(const Task& t) {
  QJsonObject o;
  baseToJson(o, t);
  o.insert(QStringLiteral("project"), t.project);
  o.insert(QStringLiteral("parent"), optString(t.parent));
  o.insert(QStringLiteral("order"), t.order);
  o.insert(QStringLiteral("title"), t.title);
  o.insert(QStringLiteral("notes"), t.notes);
  o.insert(QStringLiteral("type"), t.type);
  o.insert(QStringLiteral("mode"), t.mode);
  o.insert(QStringLiteral("duration"), t.duration);
  o.insert(QStringLiteral("start"), optString(t.start));
  o.insert(QStringLiteral("finish"), optString(t.finish));
  o.insert(QStringLiteral("constraint"), t.constraintType);
  o.insert(QStringLiteral("constraintDate"), optString(t.constraintDate));
  o.insert(QStringLiteral("deadline"), optString(t.deadline));
  o.insert(QStringLiteral("percentComplete"), t.percentComplete);
  o.insert(QStringLiteral("actualStart"), optString(t.actualStart));
  o.insert(QStringLiteral("actualFinish"), optString(t.actualFinish));
  o.insert(QStringLiteral("status"), t.status);
  o.insert(QStringLiteral("priority"), t.priority);
  o.insert(QStringLiteral("fixedCost"), t.fixedCost);
  return o;
}

Task taskFromJson(const QJsonObject& o) {
  Task t;
  baseFromJson(t, o);
  t.project = str(o, "project");
  t.parent = str(o, "parent");
  t.order = static_cast<qint64>(num(o, "order", 0));
  t.title = str(o, "title");
  t.notes = str(o, "notes");
  t.type = str(o, "type", t.type);
  t.mode = str(o, "mode", t.mode);
  t.duration = num(o, "duration", t.type == QLatin1String("milestone") ? 0 : t.duration);
  t.start = str(o, "start");
  t.finish = str(o, "finish");
  t.constraintType = str(o, "constraint", t.constraintType);
  t.constraintDate = str(o, "constraintDate");
  t.deadline = str(o, "deadline");
  t.percentComplete = static_cast<int>(num(o, "percentComplete", 0));
  t.actualStart = str(o, "actualStart");
  t.actualFinish = str(o, "actualFinish");
  t.status = str(o, "status", t.status);
  t.priority = static_cast<int>(num(o, "priority", t.priority));
  t.fixedCost = num(o, "fixedCost", 0);
  return t;
}

FieldDiff diffFields(const QJsonObject& before, const QJsonObject& after) {
  // rev and updatedAt change on every write; they are in the change row itself.
  static const QStringList kIgnored = {QStringLiteral("rev"), QStringLiteral("updatedAt")};
  FieldDiff d;
  QStringList keys = before.keys();
  for (const QString& k : after.keys()) {
    if (!keys.contains(k)) keys.append(k);
  }
  for (const QString& k : keys) {
    if (kIgnored.contains(k)) continue;
    const QJsonValue b = before.value(k);
    const QJsonValue a = after.value(k);
    if (b != a) {
      if (before.contains(k)) d.before.insert(k, b);
      if (after.contains(k)) d.after.insert(k, a);
    }
  }
  return d;
}

}  // namespace zproject
