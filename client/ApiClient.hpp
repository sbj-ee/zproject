#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

class QNetworkAccessManager;

namespace zproject::client {

struct ApiResponse {
  int status = 0;             // 0: no HTTP response (see networkError)
  QString networkError;       // connection, TLS or timeout failure
  QByteArray body;
  QJsonObject json;           // body when it is a JSON object
  QList<QPair<QByteArray, QByteArray>> headers;

  bool ok() const { return status >= 200 && status < 300; }
  QByteArray header(const QByteArray& name) const;
  // The server's {"error","code","details"} message, or a fallback.
  QString errorMessage() const;
  QString errorCode() const { return json.value(QStringLiteral("code")).toString(); }
};

// A small synchronous client of zprojectd's /v1 (SPEC.md §9): one request at
// a time, bearer token, JSON in and out, no redirects (so the token is never
// sent elsewhere), TLS verified by Qt, a timeout per request. It runs a
// local event loop while waiting.
class ApiClient {
 public:
  ApiClient(QUrl base, QByteArray token, int timeoutMs = 20'000);
  ~ApiClient();
  ApiClient(const ApiClient&) = delete;
  ApiClient& operator=(const ApiClient&) = delete;

  ApiResponse get(const QString& path, const QUrlQuery& query = {});
  ApiResponse send(const QByteArray& method, const QString& path, const QUrlQuery& query = {},
                   const QJsonObject* body = nullptr, const QList<QPair<QByteArray, QByteArray>>& headers = {});

  const QUrl& base() const { return base_; }

 private:
  QUrl base_;
  QByteArray token_;
  int timeoutMs_;
  std::unique_ptr<QNetworkAccessManager> nam_;
};

}  // namespace zproject::client
