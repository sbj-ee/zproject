#include "client/EditFlow.hpp"

#include <QJsonDocument>
#include <QSet>

#include "core/Rollup.hpp"

namespace zproject::client {

namespace {

const QSet<QString>& serverManaged() {
  static const QSet<QString> s{QStringLiteral("id"),      QStringLiteral("createdAt"), QStringLiteral("updatedAt"),
                               QStringLiteral("rev"),     QStringLiteral("deleted"),   QStringLiteral("order"),
                               QStringLiteral("project")};
  return s;
}

// null and a missing key are the same thing; so are 2 and 2.0.
bool same(const QJsonValue& a, const QJsonValue& b) {
  const bool an = a.isNull() || a.isUndefined();
  const bool bn = b.isNull() || b.isUndefined();
  if (an || bn) return an && bn;
  return a == b;
}

}  // namespace

QStringList changedFields(const QJsonObject& base, const QJsonObject& edited) {
  QStringList out;
  for (auto it = edited.constBegin(); it != edited.constEnd(); ++it) {
    if (!same(base.value(it.key()), it.value())) out.append(it.key());
  }
  return out;
}

QJsonObject changedBody(const QJsonObject& base, const QJsonObject& edited) {
  QJsonObject out;
  for (const QString& k : changedFields(base, edited)) out.insert(k, edited.value(k));
  return out;
}

Result<DryRunPreview> DryRunPreview::parse(const QJsonObject& json) {
  if (!json.value(QStringLiteral("dryRun")).toBool() || !json.value(QStringLiteral("changes")).isArray()) {
    return Error::validation(QStringLiteral("the server's answer is not a dry run"));
  }
  DryRunPreview p;
  for (const QJsonValue& v : json.value(QStringLiteral("changes")).toArray()) {
    if (!v.isObject()) return Error::validation(QStringLiteral("a dry-run change is not an object"));
    const QJsonObject o = v.toObject();
    Change c;
    c.op = o.value(QStringLiteral("op")).toString();
    c.entityKind = o.value(QStringLiteral("entityKind")).toString();
    c.entityId = o.value(QStringLiteral("entityId")).toString();
    c.before = o.value(QStringLiteral("before")).toObject();
    c.after = o.value(QStringLiteral("after")).toObject();
    if (c.op.isEmpty() || c.entityKind.isEmpty()) {
      return Error::validation(QStringLiteral("a dry-run change has no op or entity kind"));
    }
    p.changes.append(c);
  }
  p.confirmId = json.value(QStringLiteral("confirmId")).toString();
  p.confirmExpiresAt = json.value(QStringLiteral("confirmExpiresAt")).toString();
  return p;
}

int DryRunPreview::otherChanges(const QString& entityKind, const QString& entityId) const {
  int n = 0;
  for (const Change& c : changes) {
    // A create's id is not known yet: the create itself is the entity.
    const bool self = c.entityKind == entityKind &&
                      (c.entityId == entityId || (entityId.isEmpty() && c.op == QLatin1String("create")));
    if (!self) ++n;
  }
  return n;
}

int DryRunPreview::ripples(const QString& entityKind, const QString& entityId) const {
  int n = 0;
  for (const Change& c : changes) {
    const bool self = c.entityKind == entityKind &&
                      (c.entityId == entityId || (entityId.isEmpty() && c.op == QLatin1String("create")));
    if (self) continue;
    QStringList keys = c.after.keys();
    keys.removeAll(QStringLiteral("order"));
    if (c.op == QLatin1String("update") && keys.isEmpty() && c.after.contains(QStringLiteral("order"))) continue;
    ++n;
  }
  return n;
}

bool DryRunPreview::needsPreview(bool destructive, const QString& entityKind, const QString& entityId) const {
  return destructive || otherChanges(entityKind, entityId) > 0;
}

ConflictMerge::ConflictMerge(const QJsonObject& base, const QJsonObject& mine, const QJsonObject& theirs)
    : theirRev_(theirs.value(QStringLiteral("rev")).toInteger()) {
  QStringList keys = mine.keys();
  for (const QString& k : theirs.keys()) {
    if (!keys.contains(k)) keys.append(k);
  }
  for (const QString& k : keys) {
    if (serverManaged().contains(k)) continue;
    Field f{k, base.value(k), mine.contains(k) ? mine.value(k) : base.value(k), theirs.value(k), Kind::Mine};
    const bool iChanged = !same(f.base, f.mine);
    const bool theyChanged = !same(f.base, f.theirs);
    if (!iChanged && !theyChanged) continue;
    if (iChanged && !theyChanged) {
      f.kind = Kind::Mine;
    } else if (!iChanged) {
      f.kind = Kind::Theirs;
    } else {
      f.kind = same(f.mine, f.theirs) ? Kind::Same : Kind::Conflict;
    }
    fields_.append(f);
  }
}

int ConflictMerge::conflicts() const {
  int n = 0;
  for (const Field& f : fields_) {
    if (f.kind == Kind::Conflict) ++n;
  }
  return n;
}

QJsonObject ConflictMerge::resolution(bool keepMine) const {
  QJsonObject out;
  for (const Field& f : fields_) {
    if (f.kind == Kind::Mine || (keepMine && f.kind == Kind::Conflict)) out.insert(f.name, f.mine);
  }
  return out;
}

QString ConflictMerge::kindName(Kind k) {
  switch (k) {
    case Kind::Mine:
      return QStringLiteral("mine");
    case Kind::Theirs:
      return QStringLiteral("theirs");
    case Kind::Same:
      return QStringLiteral("same");
    case Kind::Conflict:
      return QStringLiteral("conflict");
  }
  return {};
}

QString displayValue(const QJsonValue& v) {
  if (v.isNull() || v.isUndefined()) return {};
  if (v.isBool()) return v.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
  if (v.isDouble()) return QString::number(v.toDouble(), 'g', 15);
  if (v.isString()) return v.toString();
  const QJsonDocument doc = v.isArray() ? QJsonDocument(v.toArray()) : QJsonDocument(v.toObject());
  return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

QList<FieldError> validateTaskEdit(const QJsonObject& base, const QJsonObject& edited, bool summary) {
  QJsonObject merged = base;
  for (auto it = edited.constBegin(); it != edited.constEnd(); ++it) merged.insert(it.key(), it.value());
  const Task next = taskFromJson(merged);
  QList<FieldError> errs = validateTask(next);
  if (summary) {
    const QList<FieldError> rolled = checkSummaryEdit(taskFromJson(base), next);
    errs.append(rolled);
    if (next.type == QLatin1String("milestone")) {
      errs.append({QStringLiteral("type"), QStringLiteral("can't be milestone: this task has subtasks")});
    }
  }
  return errs;
}

}  // namespace zproject::client
