#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <functional>
#include <memory>

#include "app/Update.hpp"
#include "app/UpdateChecker.hpp"
#include "app/UpdateDialog.hpp"

class QProcess;
class QWidget;

namespace zproject::app {

// The desktop app's updater (SPEC.md §21), zterminal's flow: check →
// offer → download SHA256SUMS + .minisig → verify signature → download the
// desktop package → verify SHA-256 → the user chooses Install → Linux:
// `pkexec apt install` of a private, re-verified copy of zproject_<v>_amd64.deb
// (only after its control file says Package: zproject); macOS: open the
// verified .dmg. It never downloads, checks or installs zprojectd's package.
class UpdateManager : public QObject {
  Q_OBJECT

 public:
  struct Options {
    QString statePath;       // update-state.ini
    QString currentVersion;  // appVersion()
    PackagePlatform platform = hostPackagePlatform();
  };
  using PackageNameReader = std::function<QString(const QString& debPath)>;

  explicit UpdateManager(Options options, QWidget* parent);
  ~UpdateManager() override;
  UpdateManager(const UpdateManager&) = delete;
  UpdateManager& operator=(const UpdateManager&) = delete;

  // Help → Check for Updates: always shows the dialog.
  UpdateDialog* checkNow();
  // At startup: quietly, if enabled and the last check is a day old; the
  // dialog appears only when there is something to offer.
  bool startupCheckIfDue(bool enabled);

  UpdateDialog* dialog() const { return dialog_.data(); }
  UpdateChecker* checker() const { return checker_; }
  UpdateDownloader* downloader() const { return downloader_; }
  const ReleaseInfo& release() const { return release_; }
  QString verifiedPackage() const { return packagePath_; }
  bool restartRequested() const { return restartRequested_; }

  // The command for a verified .deb; absolute paths only.
  static QStringList installCommand(const QString& debPath);
  // Copies the verified package into a fresh 0700 directory and re-checks
  // its SHA-256 just before the installer gets it. Empty (with *error) if not.
  static QString stageVerifiedPackage(const QString& path, const QString& sha256, QTemporaryDir& into,
                                      QString* error);
  // `dpkg-deb --field <deb> Package`; must be "zproject".
  static QString readDebPackageName(const QString& debPath);

  // Tests.
  void setInstallerForTests(const QString& program, const QStringList& args);
  void setPackageNameReaderForTests(PackageNameReader reader) { nameReader_ = std::move(reader); }
  void setRestartHandlerForTests(std::function<void()> fn) { restartHandler_ = std::move(fn); }
  void setOpenUrlHandler(std::function<void(const QUrl&)> h) { openUrl_ = std::move(h); }

 signals:
  void checkFinished(bool offered);
  void verificationFinished(bool ok, const QString& message);
  void installFinished(bool ok, const QString& message);

 private:
  UpdateDialog* ensureDialog();
  void startCheck(bool manual);
  void onCheckFinished(const UpdateChecker::Result& r);
  void startDownload();
  void onDownloadFinished(bool ok, const QString& path, const QString& error);
  void install();
  void onInstallerFinished(int exitCode, bool crashed, const QString& output);
  // keepPackage: leave the verified package for a manual install (and say
  // where); otherwise it is deleted.
  void installFailed(const QString& why, bool keepPackage = true);
  QString howToInstall() const;

  Options options_;
  QPointer<QWidget> parent_;
  UpdateChecker* checker_;
  UpdateDownloader* downloader_;
  QPointer<UpdateDialog> dialog_;
  QPointer<QProcess> installer_;
  std::unique_ptr<QTemporaryDir> downloadDir_;
  std::unique_ptr<QTemporaryDir> installDir_;
  ReleaseInfo release_;
  QString packagePath_;
  QString testProgram_;
  QStringList testArgs_;
  PackageNameReader nameReader_;
  std::function<void()> restartHandler_;
  std::function<void(const QUrl&)> openUrl_;
  bool manual_ = false;
  bool busy_ = false;
  bool restartRequested_ = false;
};

}  // namespace zproject::app
