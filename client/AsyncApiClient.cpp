#include "client/AsyncApiClient.hpp"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

#include <algorithm>
#include <memory>

#include "core/Version.hpp"

namespace zproject::client {

AsyncApiClient::AsyncApiClient(QUrl base, QByteArray token, QObject* parent)
    : QObject(parent), base_(std::move(base)), token_(std::move(token)), nam_(new QNetworkAccessManager(this)) {
  nam_->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
  resumeTimer_.setSingleShot(true);
  connect(&resumeTimer_, &QTimer::timeout, this, &AsyncApiClient::pump);
  clock_.start();
}

AsyncApiClient::~AsyncApiClient() = default;

quint64 AsyncApiClient::get(const QString& path, const QUrlQuery& query, Callback done) {
  ApiRequest r;
  r.path = path;
  r.query = query;
  return send(std::move(r), std::move(done));
}

quint64 AsyncApiClient::patch(const QString& path, const QJsonObject& body, qint64 rev, Callback done, bool dryRun,
                              const QByteArray& confirmId) {
  ApiRequest r;
  r.method = "PATCH";
  r.path = path;
  r.body = body;
  r.ifMatch = rev;
  r.dryRun = dryRun;
  r.confirmId = confirmId;
  return send(std::move(r), std::move(done));
}

quint64 AsyncApiClient::send(ApiRequest request, Callback done) {
  Pending p;
  p.id = ++nextId_;
  if (request.isWrite() && request.serializeKey.isEmpty()) request.serializeKey = request.path;
  p.request = std::move(request);
  p.done = std::move(done);
  const quint64 id = p.id;
  queue_.push_back(std::move(p));
  // Start from the event loop, so a callback never runs inside send().
  QMetaObject::invokeMethod(this, &AsyncApiClient::pump, Qt::QueuedConnection);
  return id;
}

qint64 AsyncApiClient::rateLimitRemainingMs() const {
  const qint64 left = pausedUntilMs_ - clock_.elapsed();
  return left > 0 ? left : 0;
}

bool AsyncApiClient::canStart(const Pending& p) const {
  if (p.request.isWrite()) return !busyKeys_.contains(p.request.serializeKey);
  return runningReads_ < maxReads_;
}

void AsyncApiClient::pump() {
  if (const qint64 wait = rateLimitRemainingMs(); wait > 0) {
    resumeTimer_.start(static_cast<int>(wait));
    return;
  }
  // FIFO, but a blocked request doesn't hold back later independent ones.
  // A write waiting on its key keeps later writes to that key behind it.
  QSet<QString> blockedKeys;
  for (auto it = queue_.begin(); it != queue_.end();) {
    const bool write = it->request.isWrite();
    if (write && blockedKeys.contains(it->request.serializeKey)) {
      ++it;
      continue;
    }
    if (!canStart(*it)) {
      if (write) blockedKeys.insert(it->request.serializeKey);
      ++it;
      continue;
    }
    Pending p = std::move(*it);
    it = queue_.erase(it);
    start(std::move(p));
  }
}

void AsyncApiClient::start(Pending p) {
  const ApiRequest& rq = p.request;
  QUrlQuery query = rq.query;
  if (rq.dryRun) query.addQueryItem(QStringLiteral("dryRun"), QStringLiteral("true"));
  QList<QPair<QByteArray, QByteArray>> headers{{"X-Zproject-Client-Request", QByteArray::number(p.id)}};
  if (rq.ifMatch) headers.append({"If-Match", QByteArray::number(*rq.ifMatch)});
  if (!rq.confirmId.isEmpty()) headers.append({"X-Zproject-Confirm", rq.confirmId});
  const QNetworkRequest req = buildApiRequest(base_, rq.path, query, token_,
                                              QStringLiteral("zproject/%1").arg(appVersion()), rq.body.has_value(),
                                              headers);
  QNetworkReply* reply = nullptr;
  if (rq.body) {
    reply = nam_->sendCustomRequest(req, rq.method, QJsonDocument(*rq.body).toJson(QJsonDocument::Compact));
  } else {
    reply = nam_->sendCustomRequest(req, rq.method);
  }
  // Certificate problems end the handshake: sslErrors is deliberately not
  // answered with ignoreSslErrors() anywhere in zproject.

  const bool write = rq.isWrite();
  const QString key = rq.serializeKey;
  if (write) {
    busyKeys_.insert(key);
  } else {
    ++runningReads_;
  }

  auto* timer = new QTimer(reply);
  timer->setSingleShot(true);
  auto timedOut = std::make_shared<bool>(false);
  connect(timer, &QTimer::timeout, reply, [reply, timedOut] {
    *timedOut = true;
    reply->abort();
  });
  timer->start(timeoutMs_);

  const quint64 id = p.id;
  Callback done = std::move(p.done);
  connect(reply, &QNetworkReply::finished, this, [this, reply, timedOut, write, key, id, done] {
    ApiResponse r = readApiReply(reply, base_, *timedOut, timeoutMs_);
    reply->deleteLater();
    if (write) {
      busyKeys_.remove(key);
    } else {
      --runningReads_;
    }
    if (r.status == 429) {
      bool ok = false;
      const int s = QString::fromLatin1(r.header("Retry-After")).trimmed().toInt(&ok);
      const qint64 until = clock_.elapsed() + 1000LL * (ok && s >= 0 ? s : 60);
      pausedUntilMs_ = std::max(pausedUntilMs_, until);
    }
    const QPointer<AsyncApiClient> self(this);
    if (done) done(r);
    if (!self) return;
    emit finished(id, r.status);
    pump();
  });
}

void AsyncApiClient::cancelAll() {
  std::deque<Pending> dropped;
  dropped.swap(queue_);
  for (QNetworkReply* reply : nam_->findChildren<QNetworkReply*>()) reply->abort();
  for (Pending& p : dropped) {
    ApiResponse r;
    r.networkErrorCode = QNetworkReply::OperationCanceledError;
    r.networkError = QStringLiteral("canceled");
    if (p.done) p.done(r);
  }
}

}  // namespace zproject::client
