#include "Notifier.hpp"

#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace zproject::server {

Notifier::Notifier(Settings settings, QObject* parent)
    : QObject(parent), settings_(std::move(settings)), net_(new QNetworkAccessManager(this)) {}

Notifier::~Notifier() = default;

bool Notifier::sendBlocking(const QString& title, const QString& body, int priority, const QStringList& tags,
                            QString* error) {
  if (!enabled()) return true;
  QByteArray token;
  if (!settings_.tokenFile.isEmpty()) {
    QFile f(settings_.tokenFile);
    if (!f.open(QIODevice::ReadOnly)) {
      if (error) *error = QStringLiteral("cannot read the notify hub token file %1").arg(settings_.tokenFile);
      return false;
    }
    token = f.readAll().trimmed();
  }
  QString base = settings_.url;
  while (base.endsWith(QLatin1Char('/'))) base.chop(1);
  QNetworkRequest req(QUrl(base + QStringLiteral("/v1/publish/") + settings_.topic));
  req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  if (!token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + token);
  req.setTransferTimeout(settings_.timeoutMs);
  const QJsonObject payload{{QStringLiteral("title"), title},
                            {QStringLiteral("body"), body},
                            {QStringLiteral("priority"), std::clamp(priority, 1, 5)},
                            {QStringLiteral("tags"), QJsonArray::fromStringList(tags)}};
  QNetworkReply* reply = net_->post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
  QEventLoop loop;
  connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QTimer::singleShot(settings_.timeoutMs + 1000, &loop, &QEventLoop::quit);
  loop.exec();
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const bool ok = reply->isFinished() && reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
  if (!ok && error) {
    *error = reply->isFinished() ? QStringLiteral("notify hub answered %1 (%2)").arg(status).arg(reply->errorString())
                                 : QStringLiteral("notify hub did not answer in time");
  }
  reply->abort();
  reply->deleteLater();
  return ok;
}

}  // namespace zproject::server
