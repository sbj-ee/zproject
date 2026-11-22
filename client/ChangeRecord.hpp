#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

#include "core/Error.hpp"

namespace zproject::client {

// One row of GET /v1/changes or /v1/tasks/{id}/history (SPEC.md §9.4,
// §16.5). `before`/`after` hold the changed fields only; a create's `after`
// is the whole entity, a delete's is {"deleted": true, "rev": n, ...}.
struct ChangeRecord {
  qint64 seq = 0;
  QString ts;
  QString actor;       // token id, or "system"
  QString entityKind;  // "project", "task", ...
  QString entityId;
  QJsonObject before;
  QJsonObject after;
  QString requestId;
};

// A page of the change feed.
struct ChangePage {
  QList<ChangeRecord> items;
  qint64 nextSince = 0;
  bool more = false;
};

// Strict decoding of untrusted feed JSON (fuzzed later with the other
// decoders, SPEC.md §19): seq > 0 and increasing, ids and kinds present,
// before/after objects. A page that fails is not applied at all.
Result<ChangeRecord> decodeChangeRecord(const QJsonObject& json);
Result<ChangePage> decodeChangePage(const QJsonObject& json);

}  // namespace zproject::client
