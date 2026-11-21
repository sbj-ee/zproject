#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "core/Error.hpp"
#include "store/Database.hpp"

namespace zproject::store {

// One row of `api_token` (SPEC.md §5.1, §9.2). Only the SHA-256 of the
// secret is stored.
struct TokenRecord {
  QString id;
  QString createdAt;
  QString createdBy;  // token id; empty for zprojectd --create-owner-token
  QString name;
  QString kind;  // owner, device, app, agent
  QString secretHash;
  QStringList scopes;
  QStringList projects;  // project ids, or {"*"}
  QString expiresAt;     // empty = never
  QString lastUsedAt;
  QString revokedAt;
  bool requireConfirm = false;
  QJsonObject rateLimit;  // per-token override, empty = kind default
};

// Persistence for tokens and dry-run confirmations. Policy (scopes, defaults,
// hashing) lives in the server; this class only stores rows.
class TokenStore {
 public:
  explicit TokenStore(Database& db) : db_(db) {}

  Status insert(const TokenRecord& t);
  Result<TokenRecord> find(const QString& id);
  Result<QList<TokenRecord>> list();
  Status revoke(const QString& id, const QString& at);
  Status touch(const QString& id, const QString& at);

  Status addConfirm(const QString& id, const QString& tokenId, const QString& requestHash,
                    const QString& createdAt, const QString& expiresAt);
  // Marks the confirmation used if it exists, belongs to `tokenId`, matches
  // `requestHash`, is unused and has not expired at `now`. Returns false
  // (no error) when it does not qualify.
  Result<bool> consumeConfirm(const QString& id, const QString& tokenId, const QString& requestHash,
                              const QString& now);

 private:
  Database& db_;
};

}  // namespace zproject::store
