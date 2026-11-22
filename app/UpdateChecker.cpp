#include "app/UpdateChecker.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "app/Minisign.hpp"
#include "core/Version.hpp"

namespace zproject::app {

namespace {

QNetworkRequest makeRequest(const QUrl& url, int timeoutMs, const QByteArray& accept) {
  QNetworkRequest req(url);
  // GitHub's API rejects requests without a User-Agent.
  req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("zproject/") + appVersion().toLatin1());
  req.setRawHeader("Accept", accept);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  req.setTransferTimeout(timeoutMs);
  return req;
}

QByteArray readBack(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}  // namespace

bool isAllowedUpdateUrl(const QUrl& url) {
  if (!url.isValid() || !url.userInfo().isEmpty()) return false;
  if (url.scheme() == QLatin1String("https")) return !url.host().isEmpty();
  if (url.scheme() == QLatin1String("http") && releaseTestModeActive()) {
    const QHostAddress a(url.host());
    return a.isLoopback() || url.host() == QLatin1String("localhost");
  }
  return false;
}

QString UpdateChecker::defaultApiUrl() {
  return QStringLiteral("https://api.github.com/repos/sbj-ee/zproject/releases/latest");
}

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)), url_(defaultApiUrl()) {}

UpdateChecker::~UpdateChecker() {
  if (QNetworkReply* r = reply_.data()) {
    r->disconnect(this);
    r->abort();
  }
}

void UpdateChecker::check() {
  if (reply_) return;  // one at a time; the running check reports
  if (!isAllowedUpdateUrl(url_)) {
    Result r;
    r.error = tr("The update check address is not https, so zproject did not use it.");
    QMetaObject::invokeMethod(this, [this, r] { emit finished(r); }, Qt::QueuedConnection);
    return;
  }
  QNetworkReply* reply = nam_->get(makeRequest(url_, timeoutMs_, "application/vnd.github+json"));
  reply_ = reply;
  connect(reply, &QNetworkReply::finished, this, &UpdateChecker::onFinished);
}

void UpdateChecker::abort() {
  if (QNetworkReply* r = reply_.data()) r->abort();  // finished() still fires
}

QString UpdateChecker::failureReason(QNetworkReply* reply, bool* rateLimited) {
  if (rateLimited) *rateLimited = false;
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (status == 403 || status == 429) {
    if (reply->rawHeader("x-ratelimit-remaining") == "0" || status == 429) {
      if (rateLimited) *rateLimited = true;
      const qint64 reset = reply->rawHeader("x-ratelimit-reset").toLongLong();
      if (reset > 0) {
        return tr("GitHub's rate limit for update checks was reached. Try again after %1.")
            .arg(QDateTime::fromSecsSinceEpoch(reset).toLocalTime().toString(QStringLiteral("HH:mm")));
      }
      return tr("GitHub's rate limit for update checks was reached. Try again later.");
    }
    return tr("GitHub refused the update check (HTTP %1).").arg(status);
  }
  if (status == 404) return tr("No published zproject release was found on GitHub.");
  if (status >= 400) return tr("GitHub answered the update check with HTTP %1.").arg(status);
  switch (reply->error()) {
    case QNetworkReply::OperationCanceledError:
    case QNetworkReply::TimeoutError:
      return tr("The update check timed out. Check the network connection and try again.");
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
      return tr("Couldn't reach GitHub to check for updates (no network?).");
    case QNetworkReply::SslHandshakeFailedError:
      return tr("The secure connection to GitHub failed: %1").arg(reply->errorString());
    default:
      break;
  }
  return tr("The update check failed: %1").arg(reply->errorString());
}

void UpdateChecker::onFinished() {
  QNetworkReply* reply = reply_.data();
  reply_ = nullptr;
  if (reply == nullptr) return;
  reply->deleteLater();
  Result r;
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (reply->error() != QNetworkReply::NoError || status >= 400) {
    r.error = failureReason(reply, &r.rateLimited);
    r.outcome = status == 404 ? Outcome::NoRelease : Outcome::Error;
    emit finished(r);
    return;
  }
  QString err;
  const QByteArray body = reply->isOpen() ? reply->readAll() : QByteArray();
  auto info = ReleaseInfo::fromJson(body, &err);
  if (!info) {
    r.error = err;
    emit finished(r);
    return;
  }
  r.outcome = Outcome::Release;
  r.release = std::move(*info);
  emit finished(r);
}

// ---- downloader ----

UpdateDownloader::UpdateDownloader(QObject* parent) : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

UpdateDownloader::~UpdateDownloader() {
  if (QNetworkReply* r = reply_.data()) {
    r->disconnect(this);
    r->abort();
  }
  delete file_;
}

QString UpdateDownloader::cancelledMessage() { return tr("Download cancelled."); }

void UpdateDownloader::failLater(const QString& error) {
  QMetaObject::invokeMethod(this, [this, error] { emit finished(false, QString(), error); }, Qt::QueuedConnection);
}

void UpdateDownloader::start(const ReleaseInfo& release, const QString& dir, PackagePlatform platform) {
  if (reply_ || stage_ != 0) return;
  cancelled_ = false;
  dir_ = dir;
  sumsBytes_.clear();
  verifiedSha_.clear();
  const auto pkg = release.packageAsset(platform);
  const auto sums = release.checksumAsset();
  const auto sig = release.signatureAsset();
  if (updateSigningPublicKey().isEmpty()) {
    failLater(tr("This build of zproject has no update-signing key, so it can't verify downloads and won't "
                 "install them."));
    return;
  }
  if (!pkg || !sums || !sig) {
    QStringList missing;
    if (!pkg) missing << ReleaseInfo::packageName(release.versionString(), platform);
    if (!sums) missing << QStringLiteral("SHA256SUMS");
    if (!sig) missing << tr("SHA256SUMS.minisig (its signature)");
    failLater(tr("Release %1 is missing %2, so it can't be installed from here.")
                  .arg(release.tag, missing.join(tr(" and "))));
    return;
  }
  // Only this release on github.com, never a host the API reply names.
  for (const ReleaseAsset* a : {&*pkg, &*sums, &*sig}) {
    if (!isTrustedAssetUrl(a->url, release.tag, a->name)) {
      failLater(tr("Release %1 lists %2 at %3, which is not on github.com/sbj-ee/zproject/releases/download/%1/, "
                   "so nothing was downloaded or installed.")
                    .arg(release.tag, a->name, a->url));
      return;
    }
  }
  tag_ = release.tag;
  pkg_ = *pkg;
  sums_ = *sums;
  sig_ = *sig;
  stage_ = 1;
  fetch(sums_, QDir(dir_).filePath(sums_.name));
}

void UpdateDownloader::fetch(const ReleaseAsset& asset, const QString& path) {
  delete file_;
  file_ = new QFile(path);
  if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    fail(tr("Couldn't write %1: %2").arg(path, file_->errorString()));
    return;
  }
  current_ = asset.name;
  blockedRedirect_.clear();
  QNetworkRequest req = makeRequest(QUrl(asset.url), stallMs_, "application/octet-stream");
  // Follow a redirect only to GitHub's release-asset hosts.
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
  QNetworkReply* reply = nam_->get(req);
  reply_ = reply;
  connect(reply, &QNetworkReply::redirected, this, [this, reply](const QUrl& to) {
    if (isTrustedAssetRedirect(to)) {
      emit reply->redirectAllowed();
    } else {
      blockedRedirect_ = to.toString();
      reply->abort();
    }
  });
  connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
    if (file_ != nullptr) file_->write(reply->readAll());
  });
  const qint64 announced = asset.size;
  const QString name = asset.name;
  connect(reply, &QNetworkReply::downloadProgress, this, [this, name, announced](qint64 got, qint64 total) {
    emit progress(name, got, total > 0 ? total : (announced > 0 ? announced : -1));
  });
  connect(reply, &QNetworkReply::finished, this, &UpdateDownloader::onFinished);
}

void UpdateDownloader::cancel() {
  cancelled_ = true;
  if (QNetworkReply* r = reply_.data()) r->abort();
}

void UpdateDownloader::fail(const QString& error) {
  stage_ = 0;
  sumsBytes_.clear();
  if (file_ != nullptr) {
    file_->close();
    file_->remove();  // never leave an unverified file behind
    delete file_;
    file_ = nullptr;
  }
  emit finished(false, QString(), error);
}

void UpdateDownloader::onFinished() {
  QNetworkReply* reply = reply_.data();
  reply_ = nullptr;
  if (reply == nullptr || file_ == nullptr) return;
  reply->deleteLater();
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (!blockedRedirect_.isEmpty()) {
    fail(tr("The download of %1 was redirected to %2, which is not a GitHub release-asset host, so nothing was "
            "installed.")
             .arg(current_, blockedRedirect_));
    return;
  }
  if (cancelled_) {
    fail(cancelledMessage());
    return;
  }
  if (reply->error() != QNetworkReply::NoError || status >= 400) {
    if (status == 404) {
      fail(tr("%1 is no longer on the release page.").arg(current_));
    } else if (reply->error() == QNetworkReply::OperationCanceledError) {
      fail(tr("The download of %1 stalled. Check the network connection and try again.").arg(current_));
    } else {
      fail(tr("Downloading %1 failed: %2").arg(current_, reply->errorString()));
    }
    return;
  }
  if (reply->isOpen()) file_->write(reply->readAll());
  file_->close();
  if (stage_ == 1) {
    sumsBytes_ = readBack(file_->fileName());
    stage_ = 2;
    fetch(sig_, QDir(dir_).filePath(sig_.name));
    return;
  }
  if (stage_ == 2) {
    // The signature is checked before the package is fetched at all.
    QString keyError;
    const auto key = MinisignPublicKey::parse(updateSigningPublicKey(), &keyError);
    if (!key) {
      fail(tr("The built-in update-signing key is invalid (%1), so nothing was installed.").arg(keyError));
      return;
    }
    const MinisignResult sig = verifyMinisign(sumsBytes_, readBack(file_->fileName()), *key);
    if (!sig.ok) {
      fail(tr("The release's SHA256SUMS signature is not valid (%1), so nothing was downloaded or installed.")
               .arg(sig.error));
      return;
    }
    // Bound to this tag: an older release's signed SHA256SUMS can't be replayed.
    if (sig.trustedComment != expectedSignedComment(tag_)) {
      fail(tr("The release's SHA256SUMS is signed for \"%1\", not \"%2\", so nothing was downloaded or installed.")
               .arg(sig.trustedComment, expectedSignedComment(tag_)));
      return;
    }
    stage_ = 3;
    fetch(pkg_, QDir(dir_).filePath(pkg_.name));
    return;
  }
  const QString path = file_->fileName();
  const ChecksumResult check = verifyChecksum(path, sumsBytes_, pkg_.name);
  if (!check.ok()) {
    fail(check.message());  // also deletes the unverified package
    return;
  }
  verifiedSha_ = check.expected;
  delete file_;
  file_ = nullptr;
  stage_ = 0;
  emit finished(true, path, QString());
}

}  // namespace zproject::app
