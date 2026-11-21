#include "client/ApiClient.hpp"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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

ApiResponse ApiClient::send(const QByteArray& method, const QString& path, const QUrlQuery& query,
                            const QJsonObject* body, const QList<QPair<QByteArray, QByteArray>>& headers) {
  QUrl url = base_;
  url.setPath(path);
  if (!query.isEmpty()) url.setQuery(query);
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("zproject-cli/%1").arg(appVersion()));
  req.setRawHeader("Accept", "application/json");
  if (!token_.isEmpty()) req.setRawHeader("Authorization", "Bearer " + token_);
  for (const auto& h : headers) req.setRawHeader(h.first, h.second);
  QByteArray data;
  if (body) {
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    data = QJsonDocument(*body).toJson(QJsonDocument::Compact);
  }

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

  ApiResponse r;
  r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  r.body = reply->readAll();
  for (const auto& h : reply->rawHeaderPairs()) r.headers.append(h);
  if (timedOut) {
    r.status = 0;
    r.networkError = QStringLiteral("no answer from %1 within %2 s").arg(base_.toString()).arg(timeoutMs_ / 1000);
  } else if (r.status == 0) {
    r.networkError = QStringLiteral("cannot reach %1: %2").arg(base_.toString(), reply->errorString());
  }
  if (!r.body.isEmpty()) {
    const QJsonDocument doc = QJsonDocument::fromJson(r.body);
    if (doc.isObject()) r.json = doc.object();
  }
  reply->deleteLater();
  return r;
}

}  // namespace zproject::client
