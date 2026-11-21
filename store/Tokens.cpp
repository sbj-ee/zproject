#include "Tokens.hpp"

#include <QJsonArray>
#include <QJsonDocument>

namespace zproject::store {
namespace {

const QString kColumns = QStringLiteral(
    "id, createdAt, createdBy, name, kind, secretHash, scopes, projects, expiresAt, lastUsedAt, "
    "revokedAt, requireConfirm, rateLimit");

QString jsonList(const QStringList& l) {
  return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(l)).toJson(QJsonDocument::Compact));
}

QStringList parseList(const QString& text) {
  QStringList out;
  for (const auto& v : QJsonDocument::fromJson(text.toUtf8()).array()) out.append(v.toString());
  return out;
}

TokenRecord readToken(const Statement& s) {
  TokenRecord t;
  t.id = s.text(0);
  t.createdAt = s.text(1);
  t.createdBy = s.text(2);
  t.name = s.text(3);
  t.kind = s.text(4);
  t.secretHash = s.text(5);
  t.scopes = parseList(s.text(6));
  t.projects = parseList(s.text(7));
  t.expiresAt = s.text(8);
  t.lastUsedAt = s.text(9);
  t.revokedAt = s.text(10);
  t.requireConfirm = s.int64(11) != 0;
  t.rateLimit = QJsonDocument::fromJson(s.text(12).toUtf8()).object();
  return t;
}

}  // namespace

Status TokenStore::insert(const TokenRecord& t) {
  auto st = db_.prepare(QStringLiteral("INSERT INTO api_token (%1) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, "
                                       "?9, ?10, ?11, ?12, ?13)").arg(kColumns));
  if (!st) return st.error();
  st.value()
      .bind(1, t.id)
      .bind(2, t.createdAt)
      .bindNullable(3, t.createdBy)
      .bind(4, t.name)
      .bind(5, t.kind)
      .bind(6, t.secretHash)
      .bind(7, jsonList(t.scopes))
      .bind(8, jsonList(t.projects))
      .bindNullable(9, t.expiresAt)
      .bindNullable(10, t.lastUsedAt)
      .bindNullable(11, t.revokedAt)
      .bind(12, t.requireConfirm)
      .bindNullable(13, t.rateLimit.isEmpty()
                            ? QString()
                            : QString::fromUtf8(QJsonDocument(t.rateLimit).toJson(QJsonDocument::Compact)));
  return st.value().run();
}

Result<TokenRecord> TokenStore::find(const QString& id) {
  auto st = db_.prepare(QStringLiteral("SELECT %1 FROM api_token WHERE id = ?1").arg(kColumns));
  if (!st) return st.error();
  st.value().bind(1, id);
  Error err;
  if (!st.value().step(&err)) {
    if (!err.code.isEmpty()) return err;
    return Error::notFound(QStringLiteral("token %1 not found").arg(id));
  }
  return readToken(st.value());
}

Result<QList<TokenRecord>> TokenStore::list() {
  auto st = db_.prepare(QStringLiteral("SELECT %1 FROM api_token ORDER BY createdAt, id").arg(kColumns));
  if (!st) return st.error();
  QList<TokenRecord> out;
  Error err;
  while (st.value().step(&err)) out.append(readToken(st.value()));
  if (!err.code.isEmpty()) return err;
  return out;
}

Status TokenStore::revoke(const QString& id, const QString& at) {
  auto st = db_.prepare(QStringLiteral("UPDATE api_token SET revokedAt = ?2 WHERE id = ?1 AND revokedAt IS NULL"));
  if (!st) return st.error();
  st.value().bind(1, id).bind(2, at);
  return st.value().run();
}

Status TokenStore::touch(const QString& id, const QString& at) {
  auto st = db_.prepare(QStringLiteral("UPDATE api_token SET lastUsedAt = ?2 WHERE id = ?1"));
  if (!st) return st.error();
  st.value().bind(1, id).bind(2, at);
  return st.value().run();
}

Status TokenStore::addConfirm(const QString& id, const QString& tokenId, const QString& requestHash,
                              const QString& createdAt, const QString& expiresAt) {
  // Expired confirmations are of no further use.
  auto purge = db_.prepare(QStringLiteral("DELETE FROM confirm WHERE expiresAt < ?1"));
  if (!purge) return purge.error();
  purge.value().bind(1, createdAt);
  if (auto s = purge.value().run(); !s) return s;
  auto st = db_.prepare(QStringLiteral(
      "INSERT INTO confirm (id, token, requestHash, createdAt, expiresAt) VALUES (?1, ?2, ?3, ?4, ?5)"));
  if (!st) return st.error();
  st.value().bind(1, id).bind(2, tokenId).bind(3, requestHash).bind(4, createdAt).bind(5, expiresAt);
  return st.value().run();
}

Result<bool> TokenStore::consumeConfirm(const QString& id, const QString& tokenId, const QString& requestHash,
                                        const QString& now) {
  auto st = db_.prepare(QStringLiteral(
      "UPDATE confirm SET used = 1 WHERE id = ?1 AND token = ?2 AND requestHash = ?3 AND used = 0 "
      "AND expiresAt >= ?4"));
  if (!st) return st.error();
  st.value().bind(1, id).bind(2, tokenId).bind(3, requestHash).bind(4, now);
  if (auto s = st.value().run(); !s) return s.error();
  auto changed = db_.scalar(QStringLiteral("SELECT changes()"));
  if (!changed) return changed.error();
  return changed->toLongLong() == 1;
}

}  // namespace zproject::store
