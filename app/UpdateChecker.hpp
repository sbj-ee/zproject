#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include "app/Update.hpp"

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace zproject::app {

// Asks GitHub for the latest release (public API, no token):
// GET https://api.github.com/repos/sbj-ee/zproject/releases/latest
// finished() fires exactly once per check(). zterminal's checker.
class UpdateChecker : public QObject {
  Q_OBJECT

 public:
  enum class Outcome { Release, NoRelease, Error };
  struct Result {
    Outcome outcome = Outcome::Error;
    ReleaseInfo release;  // when outcome == Release
    QString error;        // for NoRelease / Error
    bool rateLimited = false;
  };

  static QString defaultApiUrl();
  explicit UpdateChecker(QObject* parent = nullptr);
  ~UpdateChecker() override;
  UpdateChecker(const UpdateChecker&) = delete;
  UpdateChecker& operator=(const UpdateChecker&) = delete;

  // Tests point this at a loopback mock; anything else must be https.
  void setApiUrl(const QUrl& url) { url_ = url; }
  QUrl apiUrl() const { return url_; }
  void setTimeoutMs(int ms) { timeoutMs_ = ms; }
  bool isRunning() const { return !reply_.isNull(); }

  void check();
  void abort();

  static QString failureReason(QNetworkReply* reply, bool* rateLimited = nullptr);

 signals:
  void finished(const zproject::app::UpdateChecker::Result& result);

 private:
  void onFinished();

  QNetworkAccessManager* nam_;
  QPointer<QNetworkReply> reply_;
  QUrl url_;
  int timeoutMs_ = 10000;
};

// https, or http only to a loopback address (the tests' mock).
bool isAllowedUpdateUrl(const QUrl& url);

// Downloads a release's SHA256SUMS and SHA256SUMS.minisig, verifies the
// signature (release key, trusted comment "zproject <tag> SHA256SUMS")
// before fetching the package, then the package and its SHA-256. Any
// mismatch: finished(false, ...) and the unverified file is deleted.
class UpdateDownloader : public QObject {
  Q_OBJECT

 public:
  explicit UpdateDownloader(QObject* parent = nullptr);
  ~UpdateDownloader() override;
  UpdateDownloader(const UpdateDownloader&) = delete;
  UpdateDownloader& operator=(const UpdateDownloader&) = delete;

  void start(const ReleaseInfo& release, const QString& dir, PackagePlatform platform = hostPackagePlatform());
  void cancel();
  bool isRunning() const { return !reply_.isNull(); }
  void setStallTimeoutMs(int ms) { stallMs_ = ms; }
  // After a successful finished(): the package's SHA-256 from the signed
  // SHA256SUMS, held in memory to re-check right before install.
  QString verifiedSha256() const { return verifiedSha_; }

  static QString cancelledMessage();

 signals:
  void progress(const QString& assetName, qint64 bytesReceived, qint64 bytesTotal);  // total -1: unknown
  void finished(bool ok, const QString& packagePath, const QString& error);

 private:
  void fetch(const ReleaseAsset& asset, const QString& path);
  void onFinished();
  void fail(const QString& error);
  void failLater(const QString& error);

  QNetworkAccessManager* nam_;
  QPointer<QNetworkReply> reply_;
  QFile* file_ = nullptr;
  ReleaseAsset pkg_;
  ReleaseAsset sums_;
  ReleaseAsset sig_;
  QByteArray sumsBytes_;
  QString verifiedSha_;
  QString dir_;
  QString current_;
  QString tag_;
  QString blockedRedirect_;
  int stage_ = 0;  // 0 idle, 1 SHA256SUMS, 2 SHA256SUMS.minisig, 3 package
  int stallMs_ = 30000;
  bool cancelled_ = false;
};

}  // namespace zproject::app
