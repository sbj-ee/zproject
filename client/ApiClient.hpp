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
class QNetworkReply;
class QNetworkRequest;

namespace zproject::client {

struct ApiResponse {
  int status = 0;             // 0: no HTTP response (see networkError)
  QString networkError;       // connection, TLS or timeout failure
  int networkErrorCode = 0;   // QNetworkReply::NetworkError when status is 0
  bool timedOut = false;      // no answer within the timeout
  bool tlsFailure = false;    // certificate or handshake failure (never ignored)
  QByteArray body;
  QJsonObject json;           // body when it is a JSON object
  QList<QPair<QByteArray, QByteArray>> headers;

  bool ok() const { return status >= 200 && status < 300; }
  QByteArray header(const QByteArray& name) const;
  // The server's {"error","code","details"} message, or a fallback.
  QString errorMessage() const;
  QString errorCode() const { return json.value(QStringLiteral("code")).toString(); }
};

// Shared by ApiClient (CLI) and AsyncApiClient (desktop): one /v1 request
// with the bearer token, JSON Accept/Content-Type, the given User-Agent, no
// redirects (so the token is never sent elsewhere) and peer verification
// required (there is no option to turn it off).
QNetworkRequest buildApiRequest(const QUrl& base, const QString& path, const QUrlQuery& query, const QByteArray& token,
                                const QString& userAgent, bool hasBody,
                                const QList<QPair<QByteArray, QByteArray>>& headers);
// Status, headers and JSON body of a finished reply. `timedOut` when the
// caller aborted it on its timer.
ApiResponse readApiReply(QNetworkReply* reply, const QUrl& base, bool timedOut, int timeoutMs);
// True for QNetworkReply errors that come from TLS (handshake, certificate).
bool isTlsError(int networkErrorCode);

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
