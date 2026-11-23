#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "core/Error.hpp"

namespace zproject::client {

// The desktop's write flow (plan §4.4-§4.5, SPEC.md §9.3), without any GUI:
// what a dry run says will change, and how a 412 is merged field by field.

// The fields of `edited` that differ from `base` (keys of `edited` only).
QStringList changedFields(const QJsonObject& base, const QJsonObject& edited);
// `edited` reduced to its changed fields: the body of a PATCH.
QJsonObject changedBody(const QJsonObject& base, const QJsonObject& edited);

// A ?dryRun=true answer: {dryRun, changes[{op, entityKind, entityId, before,
// after}], ripple, confirmId?, confirmExpiresAt?}.
struct DryRunPreview {
  struct Change {
    QString op;  // create, update, delete, move, restore
    QString entityKind;
    QString entityId;
    QJsonObject before;
    QJsonObject after;
  };
  QList<Change> changes;
  QString confirmId;  // destructive routes only
  QString confirmExpiresAt;

  static Result<DryRunPreview> parse(const QJsonObject& json);
  // Changes to anything but this entity (summaries rolling up, a project's
  // tasks going with it).
  int otherChanges(const QString& entityKind, const QString& entityId) const;
  // Q6: preview when the write is destructive or ripples to other entities;
  // a plain edit applies straight after a clean dry run.
  bool needsPreview(bool destructive, const QString& entityKind, const QString& entityId) const;
  // Like otherChanges, but without siblings that only get a new `order`
  // (a move renumbers them; that is not a ripple, plan Q6).
  int ripples(const QString& entityKind, const QString& entityId) const;
};

// A 412 (plan §4.4): the user edited `mine` starting from `base`; the server
// now has `theirs`. Per field:
//   Mine     only you changed it: kept automatically
//   Theirs   only the server changed it: the server's value stays
//   Same     both changed it to the same value: nothing to decide
//   Conflict both changed it differently: Keep mine or Take theirs
class ConflictMerge {
 public:
  enum class Kind { Mine, Theirs, Same, Conflict };
  struct Field {
    QString name;
    QJsonValue base;
    QJsonValue mine;
    QJsonValue theirs;
    Kind kind = Kind::Mine;
  };

  // Server-managed fields are left out: id, createdAt, updatedAt, rev,
  // deleted, order.
  ConflictMerge(const QJsonObject& base, const QJsonObject& mine, const QJsonObject& theirs);

  const QList<Field>& fields() const { return fields_; }
  int conflicts() const;
  qint64 theirRev() const { return theirRev_; }
  // The PATCH to send on the server's revision. keepMine: every field you
  // changed; otherwise only the ones the server didn't (Take theirs). Empty:
  // nothing left to send.
  QJsonObject resolution(bool keepMine) const;

  static QString kindName(Kind k);

 private:
  QList<Field> fields_;
  qint64 theirRev_ = 0;
};

// A JSON value as the user reads it in a table: "" for null, numbers without
// trailing zeros.
QString displayValue(const QJsonValue& v);

}  // namespace zproject::client
