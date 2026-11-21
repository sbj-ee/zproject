#include "Auth.hpp"

#include <QCryptographicHash>
#include <QRandomGenerator>

#include <array>

#include "core/Id.hpp"

namespace zproject::server {
namespace {

constexpr int kSecretBytes = 32;
constexpr int kSecretChars = 43;  // unpadded base64url of 32 bytes

const QStringList& readScopes() {
  static const QStringList s{
      QStringLiteral("projects:read"),  QStringLiteral("tasks:read"),    QStringLiteral("schedule:read"),
      QStringLiteral("resources:read"), QStringLiteral("raid:read"),     QStringLiteral("reports:read"),
      QStringLiteral("calendar:read"),  QStringLiteral("alerts:read")};
  return s;
}

const QStringList& writeOnlyScopes() {
  static const QStringList s{
      QStringLiteral("projects:write"),  QStringLiteral("tasks:write"),     QStringLiteral("resources:write"),
      QStringLiteral("baselines:write"), QStringLiteral("raid:write"),      QStringLiteral("templates:write"),
      QStringLiteral("alerts:write")};
  return s;
}

bool isBase64UrlChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

}  // namespace

QString NewToken::text() const {
  return QStringLiteral("zpt_") + id + QLatin1Char('.') + QString::fromLatin1(secret);
}

NewToken generateToken() {
  std::array<quint32, kSecretBytes / 4> words{};
  QRandomGenerator::system()->fillRange(words.data(), words.size());
  const QByteArray raw(reinterpret_cast<const char*>(words.data()), kSecretBytes);
  NewToken t;
  t.id = newId();
  t.secret = raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  t.secretHash = hashSecret(t.secret);
  return t;
}

QString hashSecret(const QByteArray& secret) {
  return QString::fromLatin1(QCryptographicHash::hash(secret, QCryptographicHash::Sha256).toHex());
}

bool constantTimeEquals(const QByteArray& a, const QByteArray& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (qsizetype i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

std::optional<ParsedToken> parseToken(const QByteArray& text) {
  if (!text.startsWith("zpt_")) return std::nullopt;
  const QByteArray rest = text.mid(4);
  const qsizetype dot = rest.indexOf('.');
  if (dot != kIdLength) return std::nullopt;
  const QString id = QString::fromLatin1(rest.left(dot));
  const QByteArray secret = rest.mid(dot + 1);
  if (!isValidId(id) || secret.size() != kSecretChars) return std::nullopt;
  for (char c : secret) {
    if (!isBase64UrlChar(c)) return std::nullopt;
  }
  return ParsedToken{id, secret};
}

std::optional<ParsedToken> parseBearer(const QByteArray& header) {
  const QByteArray h = header.trimmed();
  if (h.size() < 7 || h.left(7).toLower() != "bearer ") return std::nullopt;
  return parseToken(h.mid(7).trimmed());
}

const QStringList& knownScopes() {
  static const QStringList s = [] {
    QStringList all = readScopes() + writeOnlyScopes();
    all << QStringLiteral("audit:read") << QStringLiteral("admin") << QStringLiteral("read")
        << QStringLiteral("write");
    return all;
  }();
  return s;
}

QStringList expandScopes(const QStringList& scopes) {
  QStringList out;
  for (const QString& s : scopes) {
    if (s == QLatin1String("read")) {
      out += readScopes();
    } else if (s == QLatin1String("write")) {
      out += readScopes();
      out += writeOnlyScopes();
    } else {
      out.append(s);
    }
  }
  out.removeDuplicates();
  return out;
}

bool hasScope(const store::TokenRecord& t, const QString& scope) {
  if (scope.isEmpty()) return true;
  const QStringList expanded = expandScopes(t.scopes);
  return expanded.contains(QStringLiteral("admin")) || expanded.contains(scope);
}

bool allProjects(const store::TokenRecord& t) { return t.projects.contains(QStringLiteral("*")); }

bool projectAllowed(const store::TokenRecord& t, const QString& projectId) {
  return allProjects(t) || t.projects.contains(projectId);
}

}  // namespace zproject::server
