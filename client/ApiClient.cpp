#include "client/ApiClient.hpp"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtNetwork/qtnetworkglobal.h>
#if QT_CONFIG(ssl)
#include <QSslConfiguration>
#include <QSslSocket>
#endif
#include <QTimer>

#include "core/Version.hpp"

namespace zproject::client {

QByteArray ApiResponse::header(const QByteArray& name) const {
  for (const auto& h : headers) {
    if (h.first.compare(name, Qt::CaseInsensitive) == 0) return h.second;
  }
  return {};
}

QString ApiResponse::errorMessage() const {
  if (status == 0) return networkError;
  const QString e = json.value(QStringLiteral("error")).toString();
  if (!e.isEmpty()) return e;
  return QStringLiteral("HTTP %1").arg(status);
}

ApiClient::ApiClient(QUrl base, QByteArray token, int timeoutMs)
    : base_(std::move(base)), token_(std::move(token)), timeoutMs_(timeoutMs),
      nam_(std::make_unique<QNetworkAccessManager>()) {
  nam_->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

ApiClient::~ApiClient() = default;

ApiResponse ApiClient::get(const QString& path, const QUrlQuery& query) { return send("GET", path, query); }

QNetworkRequest buildApiRequest(const QUrl& base, const QString& path, const QUrlQuery& query, const QByteArray& token,
                                const QString& userAgent, bool hasBody,
                                const QList<QPair<QByteArray, QByteArray>>& headers) {
  QUrl url = base;
  url.setPath(path);
  if (!query.isEmpty()) url.setQuery(query);
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  req.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
  req.setRawHeader("Accept", "application/json");
  if (!token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + token);
  for (const auto& h : headers) req.setRawHeader(h.first, h.second);
  if (hasBody) req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
#if QT_CONFIG(ssl)
  if (url.scheme() == QLatin1String("https")) {
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    req.setSslConfiguration(tls);
  }
#endif
  return req;
}

bool isTlsError(int networkErrorCode) {
  // Certificate errors are never ignored, so they end the handshake.
  return networkErrorCode == QNetworkReply::SslHandshakeFailedError;
}

ApiResponse readApiReply(QNetworkReply* reply, const QUrl& base, bool timedOut, int timeoutMs) {
  ApiResponse r;
  r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (reply->isOpen()) r.body = reply->readAll();
  for (const auto& h : reply->rawHeaderPairs()) r.headers.append(h);
  if (timedOut) {
    r.status = 0;
    r.timedOut = true;
    r.networkErrorCode = QNetworkReply::TimeoutError;
    r.networkError = QStringLiteral("no answer from %1 within %2 s").arg(base.toString()).arg(timeoutMs / 1000);
  } else if (r.status == 0) {
    r.networkErrorCode = reply->error();
    r.tlsFailure = isTlsError(r.networkErrorCode);
    r.networkError = QStringLiteral("cannot reach %1: %2").arg(base.toString(), reply->errorString());
  }
  if (!r.body.isEmpty()) {
    const QJsonDocument doc = QJsonDocument::fromJson(r.body);
    if (doc.isObject()) r.json = doc.object();
  }
  return r;
}

ApiResponse ApiClient::send(const QByteArray& method, const QString& path, const QUrlQuery& query,
                            const QJsonObject* body, const QList<QPair<QByteArray, QByteArray>>& headers) {
  const QNetworkRequest req = buildApiRequest(base_, path, query, token_,
                                              QStringLiteral("zproject-cli/%1").arg(appVersion()), body != nullptr, headers);
  QByteArray data;
  if (body) data = QJsonDocument(*body).toJson(QJsonDocument::Compact);

  QNetworkReply* reply = body ? nam_->sendCustomRequest(req, method, data) : nam_->sendCustomRequest(req, method);
  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  bool timedOut = false;
  QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
    timedOut = true;
    reply->abort();
  });
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  timer.start(timeoutMs_);
  if (!reply->isFinished()) loop.exec();
  timer.stop();

  ApiResponse r = readApiReply(reply, base_, timedOut, timeoutMs_);
  reply->deleteLater();
  return r;
}

}  // namespace zproject::client
