#include "client/ChangeRecord.hpp"

#include <QJsonArray>

namespace zproject::client {

namespace {

Error bad(const QString& why) { return Error::validation(QStringLiteral("change feed: %1").arg(why)); }

// JSON numbers are doubles: accept only exact non-negative integers.
bool toSeq(const QJsonValue& v, qint64* out) {
  if (!v.isDouble()) return false;
  const double d = v.toDouble();
  if (d < 0 || d > 9007199254740991.0) return false;  // 2^53 - 1
  const auto i = static_cast<qint64>(d);
  if (static_cast<double>(i) != d) return false;
  *out = i;
  return true;
}

bool optionalObject(const QJsonObject& json, const QString& key, QJsonObject* out) {
  const QJsonValue v = json.value(key);
  if (v.isUndefined() || v.isNull()) return true;
  if (!v.isObject()) return false;
  *out = v.toObject();
  return true;
}

}  // namespace

Result<ChangeRecord> decodeChangeRecord(const QJsonObject& json) {
  ChangeRecord c;
  if (!toSeq(json.value(QStringLiteral("seq")), &c.seq) || c.seq < 1) return bad(QStringLiteral("bad seq"));
  c.ts = json.value(QStringLiteral("ts")).toString();
  c.actor = json.value(QStringLiteral("actor")).toString();
  c.entityKind = json.value(QStringLiteral("entityKind")).toString();
  c.entityId = json.value(QStringLiteral("entityId")).toString();
  c.requestId = json.value(QStringLiteral("requestId")).toString();
  if (c.ts.isEmpty() || c.entityKind.isEmpty() || c.entityId.isEmpty()) {
    return bad(QStringLiteral("record %1 lacks ts, entityKind or entityId").arg(c.seq));
  }
  if (!optionalObject(json, QStringLiteral("before"), &c.before) ||
      !optionalObject(json, QStringLiteral("after"), &c.after)) {
    return bad(QStringLiteral("record %1: before/after are not objects").arg(c.seq));
  }
  const QJsonValue id = c.after.value(QStringLiteral("id"));
  if (!id.isUndefined() && id.toString() != c.entityId) {
    return bad(QStringLiteral("record %1: after.id differs from entityId").arg(c.seq));
  }
  return c;
}

Result<ChangePage> decodeChangePage(const QJsonObject& json) {
  ChangePage p;
  const QJsonValue items = json.value(QStringLiteral("items"));
  if (!items.isArray()) return bad(QStringLiteral("no items array"));
  if (!toSeq(json.value(QStringLiteral("nextSince")), &p.nextSince)) return bad(QStringLiteral("bad nextSince"));
  const QJsonValue more = json.value(QStringLiteral("more"));
  if (!more.isBool()) return bad(QStringLiteral("bad more"));
  p.more = more.toBool();
  qint64 last = 0;
  for (const QJsonValue& v : items.toArray()) {
    if (!v.isObject()) return bad(QStringLiteral("an item is not an object"));
    auto c = decodeChangeRecord(v.toObject());
    if (!c) return c.error();
    if (c->seq <= last) return bad(QStringLiteral("seq %1 is not increasing").arg(c->seq));
    last = c->seq;
    p.items.append(std::move(c).value());
  }
  if (!p.items.isEmpty() && p.nextSince != last) return bad(QStringLiteral("nextSince is not the last seq"));
  return p;
}

}  // namespace zproject::client
