#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

#include "store/Tokens.hpp"

namespace zproject::server {

// Bearer tokens "zpt_<id>.<secret>" (SPEC.md §9.2): <id> is the 20-hex token
// id, <secret> 32 random bytes in unpadded base64url (256 bits). Only
// SHA-256(secret) is stored; comparison is constant-time.
struct ParsedToken {
  QString id;
  QByteArray secret;
};

struct NewToken {
  QString id;
  QByteArray secret;
  QString secretHash;  // hex SHA-256 of secret
  QString text() const;  // zpt_<id>.<secret>
};

NewToken generateToken();
std::optional<ParsedToken> parseToken(const QByteArray& text);
std::optional<ParsedToken> parseBearer(const QByteArray& authorizationHeader);
QString hashSecret(const QByteArray& secret);
bool constantTimeEquals(const QByteArray& a, const QByteArray& b);

// Scopes and bundles.
const QStringList& knownScopes();       // every individual scope + read/write bundles + admin
QStringList expandScopes(const QStringList& scopes);  // bundles -> individual scopes
bool hasScope(const store::TokenRecord& t, const QString& scope);
bool projectAllowed(const store::TokenRecord& t, const QString& projectId);
bool allProjects(const store::TokenRecord& t);

}  // namespace zproject::server
