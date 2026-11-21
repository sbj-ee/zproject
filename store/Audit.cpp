#include "Audit.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

namespace zproject::store {
namespace {

const QString kColumns = QStringLiteral(
    "seq, ts, tokenId, tokenName, tokenKind, tsUser, tsNode, method, path, mcpTool, entityKind, "
    "entityId, before, after, dryRun, confirmId, status, requestId, prevHash, hash");

QJsonValue orNull(const QString& s) { return s.isEmpty() ? QJsonValue() : QJsonValue(s); }

AuditEntry readEntry(const Statement& s) {
  AuditEntry e;
  e.seq = s.int64(0);
  e.ts = s.text(1);
  e.tokenId = s.text(2);
  e.tokenName = s.text(3);
  e.tokenKind = s.text(4);
  e.tsUser = s.text(5);
  e.tsNode = s.text(6);
  e.method = s.text(7);
  e.path = s.text(8);
  e.mcpTool = s.text(9);
  e.entityKind = s.text(10);
  e.entityId = s.text(11);
  e.before = s.text(12);
  e.after = s.text(13);
  e.dryRun = s.int64(14) != 0;
  e.confirmId = s.text(15);
  e.status = static_cast<int>(s.int64(16));
  e.requestId = s.text(17);
  e.prevHash = s.text(18);
  e.hash = s.text(19);
  return e;
}

}  // namespace

QString AuditLog::genesisHash() { return QString(64, QLatin1Char('0')); }

QString AuditLog::computeHash(const AuditEntry& e) {
  // A fixed-order JSON array of every field except seq and hash itself.
  const QJsonArray fields{
      QStringLiteral("zproject-audit-v1"), e.ts, orNull(e.tokenId), orNull(e.tokenName),
      orNull(e.tokenKind), orNull(e.tsUser), orNull(e.tsNode), orNull(e.method), orNull(e.path),
      orNull(e.mcpTool), orNull(e.entityKind), orNull(e.entityId), orNull(e.before), orNull(e.after),
      e.dryRun, orNull(e.confirmId), e.status, orNull(e.requestId), e.prevHash};
  const QByteArray canonical = QJsonDocument(fields).toJson(QJsonDocument::Compact);
  return QString::fromLatin1(QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

Status AuditLog::append(AuditEntry& e) {
  Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  auto last = db_.scalar(QStringLiteral("SELECT hash FROM audit ORDER BY seq DESC LIMIT 1"));
  if (!last) return last.error();
  e.prevHash = last->isNull() ? genesisHash() : last->toString();
  e.hash = computeHash(e);
  auto st = db_.prepare(QStringLiteral(
      "INSERT INTO audit (ts, tokenId, tokenName, tokenKind, tsUser, tsNode, method, path, mcpTool, "
      "entityKind, entityId, before, after, dryRun, confirmId, status, requestId, prevHash, hash) "
      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19)"));
  if (!st) return st.error();
  st.value()
      .bind(1, e.ts)
      .bindNullable(2, e.tokenId)
      .bindNullable(3, e.tokenName)
      .bindNullable(4, e.tokenKind)
      .bindNullable(5, e.tsUser)
      .bindNullable(6, e.tsNode)
      .bindNullable(7, e.method)
      .bindNullable(8, e.path)
      .bindNullable(9, e.mcpTool)
      .bindNullable(10, e.entityKind)
      .bindNullable(11, e.entityId)
      .bindNullable(12, e.before)
      .bindNullable(13, e.after)
      .bind(14, e.dryRun)
      .bindNullable(15, e.confirmId)
      .bind(16, e.status)
      .bindNullable(17, e.requestId)
      .bind(18, e.prevHash)
      .bind(19, e.hash);
  if (auto s = st.value().run(); !s) return s;
  auto seq = db_.scalar(QStringLiteral("SELECT last_insert_rowid()"));
  if (!seq) return seq.error();
  e.seq = seq->toLongLong();
  return tx.commit();
}

Result<QList<AuditEntry>> AuditLog::since(qint64 since, int limit, const QString& tokenId) {
  limit = std::clamp(limit, 1, 500);
  auto st = db_.prepare(QStringLiteral("SELECT %1 FROM audit WHERE seq > ?1 %2 ORDER BY seq LIMIT ?3")
                            .arg(kColumns, tokenId.isEmpty() ? QString() : QStringLiteral("AND tokenId = ?2")));
  if (!st) return st.error();
  st.value().bind(1, since).bind(3, limit);
  if (!tokenId.isEmpty()) st.value().bind(2, tokenId);
  QList<AuditEntry> out;
  Error err;
  while (st.value().step(&err)) out.append(readEntry(st.value()));
  if (!err.code.isEmpty()) return err;
  return out;
}

Result<qint64> AuditLog::verify() {
  auto st = db_.prepare(QStringLiteral("SELECT %1 FROM audit ORDER BY seq").arg(kColumns));
  if (!st) return st.error();
  QString prev = genesisHash();
  qint64 count = 0;
  Error err;
  while (st.value().step(&err)) {
    const AuditEntry e = readEntry(st.value());
    if (e.prevHash != prev) {
      return Error{QStringLiteral("audit_broken"),
                   QStringLiteral("audit row %1: prevHash does not match the previous row").arg(e.seq),
                   QJsonObject{{QStringLiteral("seq"), e.seq}}};
    }
    if (computeHash(e) != e.hash) {
      return Error{QStringLiteral("audit_broken"),
                   QStringLiteral("audit row %1: hash does not match its contents").arg(e.seq),
                   QJsonObject{{QStringLiteral("seq"), e.seq}}};
    }
    prev = e.hash;
    ++count;
  }
  if (!err.code.isEmpty()) return err;
  return count;
}

}  // namespace zproject::store
