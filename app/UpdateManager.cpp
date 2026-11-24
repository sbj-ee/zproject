#include "app/UpdateManager.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>
#include <QWidget>

namespace zproject::app {

namespace {
const QString kInstalledBinary = QStringLiteral("/usr/bin/zproject");
// Absolute paths: never whatever $PATH finds first.
const QString kPkexec = QStringLiteral("/usr/bin/pkexec");
const QString kApt = QStringLiteral("/usr/bin/apt");
const QString kDpkgDeb = QStringLiteral("/usr/bin/dpkg-deb");
const QString kDesktopPackage = QStringLiteral("zproject");
}  // namespace

UpdateManager::UpdateManager(Options options, QWidget* parent)
    : QObject(parent),
      options_(std::move(options)),
      parent_(parent),
      checker_(new UpdateChecker(this)),
      downloader_(new UpdateDownloader(this)),
      nameReader_(&UpdateManager::readDebPackageName) {
  connect(checker_, &UpdateChecker::finished, this, &UpdateManager::onCheckFinished);
  connect(downloader_, &UpdateDownloader::finished, this, &UpdateManager::onDownloadFinished);
  connect(downloader_, &UpdateDownloader::progress, this, [this](const QString& name, qint64 got, qint64 total) {
    if (UpdateDialog* d = dialog_.data()) d->showDownloading(name, got, total);
  });
}

UpdateManager::~UpdateManager() {
  if (QProcess* p = installer_.data()) {
    p->disconnect(this);
    p->kill();
    p->waitForFinished(1000);
  }
}

void UpdateManager::setInstallerForTests(const QString& program, const QStringList& args) {
  testProgram_ = program;
  testArgs_ = args;
}

QStringList UpdateManager::installCommand(const QString& debPath) {
  // An absolute path makes apt treat the argument as a file, not a package name.
  return {kPkexec, kApt, QStringLiteral("install"), QStringLiteral("-y"), QFileInfo(debPath).absoluteFilePath()};
}

QString UpdateManager::stageVerifiedPackage(const QString& path, const QString& sha256, QTemporaryDir& into,
                                            QString* error) {
  auto fail = [error](const QString& why) {
    if (error) *error = why;
    return QString();
  };
  if (!into.isValid()) return fail(tr("Couldn't create a private folder for the package."));
  QFile::setPermissions(into.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  const QString dest = QDir(into.path()).filePath(QFileInfo(path).fileName());
  if (!QFile::copy(path, dest)) return fail(tr("Couldn't copy the package to %1.").arg(into.path()));
  QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup |
                                  QFileDevice::ReadOther);
  if (sha256.isEmpty() || sha256OfFile(dest) != sha256.toLower()) {
    QFile::remove(dest);
    return fail(tr("The package changed after it was verified, so it was not installed."));
  }
  return QFileInfo(dest).absoluteFilePath();
}

QString UpdateManager::readDebPackageName(const QString& debPath) {
  if (!QFileInfo(kDpkgDeb).isExecutable()) return {};
  QProcess p;
  p.start(kDpkgDeb, {QStringLiteral("--field"), QFileInfo(debPath).absoluteFilePath(), QStringLiteral("Package")});
  if (!p.waitForFinished(10000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return {};
  return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

UpdateDialog* UpdateManager::ensureDialog() {
  UpdateDialog* d = dialog_.data();
  if (d != nullptr && d->isHidden()) {
    // Closed and about to be deleted (WA_DeleteOnClose): start a fresh one.
    d->disconnect(this);
    d = nullptr;
  }
  if (d == nullptr) {
    d = new UpdateDialog(options_.currentVersion, parent_.data());
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setPlatform(options_.platform);
    d->setOpenUrlHandler([this](const QUrl& u) {
      if (openUrl_) {
        openUrl_(u);
      } else {
        QDesktopServices::openUrl(u);
      }
    });
    connect(d, &UpdateDialog::retryRequested, this, [this] { startCheck(true); });
    connect(d, &UpdateDialog::downloadRequested, this, &UpdateManager::startDownload);
    connect(d, &UpdateDialog::cancelRequested, downloader_, &UpdateDownloader::cancel);
    connect(d, &UpdateDialog::installRequested, this, &UpdateManager::install);
    connect(d, &UpdateDialog::skipRequested, this, [this] {
      UpdateState st = UpdateState::load(options_.statePath);
      st.skippedVersion = release_.tag;
      st.save(options_.statePath);
    });
    connect(d, &UpdateDialog::restartRequested, this, [this] {
      restartRequested_ = true;
      if (restartHandler_) {
        restartHandler_();
      } else {
        QApplication::quit();  // main() starts the new version after the lock is released
      }
    });
    connect(d, &QDialog::finished, this, [this] {
      // Closing the window abandons a download; a verified package is deleted.
      downloader_->cancel();
      if (!installer_) {
        downloadDir_.reset();
        packagePath_.clear();
      }
    });
    dialog_ = d;
  }
  d->show();
  d->raise();
  d->activateWindow();
  return d;
}

UpdateDialog* UpdateManager::checkNow() {
  UpdateDialog* d = ensureDialog();
  if (!busy_) startCheck(true);
  return d;
}

bool UpdateManager::startupCheckIfDue(bool enabled) {
  if (busy_ || !UpdatePolicy::shouldCheckAtStartup(enabled, UpdateState::load(options_.statePath),
                                                   QDateTime::currentDateTimeUtc())) {
    return false;
  }
  startCheck(false);
  return true;
}

void UpdateManager::startCheck(bool manual) {
  if (checker_->isRunning()) {
    manual_ = manual_ || manual;
    return;
  }
  manual_ = manual;
  busy_ = true;
  if (UpdateDialog* d = dialog_.data()) d->showChecking();
  checker_->check();
}

void UpdateManager::onCheckFinished(const UpdateChecker::Result& r) {
  busy_ = false;
  if (!manual_) {
    // Count failed attempts too, so a flaky network isn't a request per start.
    UpdateState st = UpdateState::load(options_.statePath);
    st.lastCheck = QDateTime::currentDateTimeUtc();
    st.save(options_.statePath);
  }
  if (r.outcome != UpdateChecker::Outcome::Release) {
    if (manual_) {
      UpdateDialog* d = ensureDialog();
      if (r.outcome == UpdateChecker::Outcome::NoRelease) {
        d->showUpToDate(r.error);
      } else {
        d->showCheckFailed(r.error);
      }
    }
    emit checkFinished(false);
    return;
  }
  release_ = r.release;
  if (!UpdatePolicy::shouldOffer(release_, options_.currentVersion, UpdateState::load(options_.statePath), manual_)) {
    if (manual_) {
      const QString text = isNewerVersion(options_.currentVersion, release_.tag)
                               ? tr("You're running zproject %1, newer than the latest release (%2).")
                                     .arg(options_.currentVersion, release_.tag)
                               : tr("You're running the latest version of zproject (%1).").arg(options_.currentVersion);
      ensureDialog()->showUpToDate(text);
    }
    emit checkFinished(false);
    return;
  }
  ensureDialog()->showAvailable(release_);
  emit checkFinished(true);
}

void UpdateManager::startDownload() {
  if (downloader_->isRunning()) return;
  downloadDir_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/zproject-update-XXXXXX"));
  if (!downloadDir_->isValid()) {
    if (UpdateDialog* d = dialog_.data()) d->showRefused(tr("Couldn't create a temporary folder for the download."));
    return;
  }
  QFile::setPermissions(downloadDir_->path(),
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  if (UpdateDialog* d = dialog_.data()) d->showDownloading(QStringLiteral("SHA256SUMS"), 0, -1);
  downloader_->start(release_, downloadDir_->path(), options_.platform);
}

QString UpdateManager::howToInstall() const {
  if (options_.platform == PackagePlatform::MacOS) {
    return tr("Install opens the disk image; drag zproject.app to Applications, then reopen zproject.");
  }
  return tr("Install runs “pkexec apt install” for the zproject desktop package only; you'll be asked for your "
            "password. The zprojectd server package is not touched.");
}

void UpdateManager::onDownloadFinished(bool ok, const QString& path, const QString& error) {
  UpdateDialog* d = dialog_.data();
  if (!ok) {
    downloadDir_.reset();
    packagePath_.clear();
    if (d != nullptr && error != UpdateDownloader::cancelledMessage()) {
      d->showRefused(error);
    } else if (d != nullptr) {
      d->showAvailable(release_);
    }
    emit verificationFinished(false, error);
    return;
  }
  packagePath_ = path;
  if (d != nullptr) d->showVerified(path, downloader_->verifiedSha256(), howToInstall());
  emit verificationFinished(true, path);
}

void UpdateManager::installFailed(const QString& why, bool keepPackage) {
  QString text = why;
  if (!keepPackage) {
    downloadDir_.reset();
    packagePath_.clear();
  }
  if (!packagePath_.isEmpty()) {
    if (downloadDir_) downloadDir_->setAutoRemove(false);  // keep the verified package for a manual install
    text += options_.platform == PackagePlatform::MacOS
                ? tr("\n\nThe verified disk image is at:\n%1\nOpen it and drag zproject.app to Applications.")
                      .arg(packagePath_)
                : tr("\n\nThe verified package is at:\n%1\nInstall it with:\nsudo apt install %1").arg(packagePath_);
  }
  if (UpdateDialog* d = dialog_.data()) d->showInstallFailed(text);
  emit installFinished(false, text);
}

void UpdateManager::install() {
  if (packagePath_.isEmpty() || installer_) return;
  const QString name = QFileInfo(packagePath_).fileName();
  const std::optional<ReleaseAsset> expected = release_.packageAsset(options_.platform);
  if (isServerPackageName(name) || !expected || name != expected->name) {
    installFailed(tr("%1 is not the zproject desktop package, so it was not installed.").arg(name), false);
    return;
  }
  installDir_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/zproject-install-XXXXXX"));
  QString stageError;
  const QString staged = stageVerifiedPackage(packagePath_, downloader_->verifiedSha256(), *installDir_, &stageError);
  if (staged.isEmpty()) {
    installDir_.reset();
    installFailed(stageError, false);
    return;
  }
  QString program;
  QStringList args;
  if (options_.platform == PackagePlatform::MacOS) {
    if (!testProgram_.isEmpty()) {
      program = testProgram_;
      args = testArgs_;
      args << staged;
    } else {
      if (!QDesktopServices::openUrl(QUrl::fromLocalFile(staged))) {
        installFailed(tr("Couldn't open %1.").arg(staged));
        return;
      }
      const QString text = tr("zproject %1 was downloaded and verified. The disk image is open: drag zproject.app "
                              "to Applications, then quit and reopen zproject.")
                               .arg(release_.tag);
      if (UpdateDialog* d = dialog_.data()) d->showInstalled(text, false);
      emit installFinished(true, text);
      return;
    }
  } else {
    // The control file must name the desktop package, never zprojectd.
    const QString pkg = nameReader_ ? nameReader_(staged) : QString();
    if (pkg != kDesktopPackage) {
      installDir_.reset();
      installFailed(pkg.isEmpty() ? tr("The package's name could not be read (dpkg-deb), so it was not installed.")
                                  : tr("The package is “%1”, not “zproject”, so it was deleted and not installed.")
                                        .arg(pkg),
                    pkg.isEmpty());
      return;
    }
    if (!testProgram_.isEmpty()) {
      program = testProgram_;
      args = testArgs_;
      args << staged;
    } else {
      if (QCoreApplication::applicationFilePath() != kInstalledBinary) {
        installFailed(tr("This zproject (%1) wasn't installed from the .deb, so the package can't be installed in "
                         "its place automatically.")
                          .arg(QCoreApplication::applicationFilePath()));
        return;
      }
      const QStringList cmd = installCommand(staged);
      if (!QFileInfo(cmd.at(0)).isExecutable() || !QFileInfo(cmd.at(1)).isExecutable()) {
        installFailed(tr("pkexec or apt isn't available, so the package can't be installed from here."));
        return;
      }
      program = cmd.at(0);
      args = cmd.mid(1);
    }
  }
  if (UpdateDialog* d = dialog_.data()) {
    d->showInstalling(tr("Installing %1. Enter your password in the system prompt.").arg(name));
  }
  auto* proc = new QProcess(this);
  installer_ = proc;
  proc->setWorkingDirectory(installDir_->path());
  proc->setProcessChannelMode(QProcess::MergedChannels);
  connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus st) {
    onInstallerFinished(code, st == QProcess::CrashExit, QString::fromLocal8Bit(proc->readAll()).trimmed());
  });
  connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart) onInstallerFinished(127, false, QString());
  });
  proc->start(program, args);  // argv list, no shell
}

void UpdateManager::onInstallerFinished(int exitCode, bool crashed, const QString& output) {
  QProcess* proc = installer_.data();
  if (proc == nullptr) return;
  installer_ = nullptr;
  proc->deleteLater();
  installDir_.reset();
  if (crashed || exitCode != 0) {
    QString why;
    if (exitCode == 126) {
      why = tr("The password prompt was dismissed, so nothing was installed.");
    } else if (exitCode == 127) {
      why = tr("Not authorized to install packages (pkexec exit 127).");
    } else {
      why = tr("apt couldn't install the package (exit %1).").arg(exitCode);
      if (!output.isEmpty()) why += QLatin1Char('\n') + output.right(600);
    }
    installFailed(why);
    return;
  }
  downloadDir_.reset();
  packagePath_.clear();
  const QString text = tr("zproject %1 was installed. Restart zproject to use it.").arg(release_.tag);
  if (UpdateDialog* d = dialog_.data()) d->showInstalled(text, true);
  emit installFinished(true, text);
}

}  // namespace zproject::app
