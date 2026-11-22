#pragma once

#include <QDialog>
#include <QUrl>

#include <functional>

#include "app/Update.hpp"

class QLabel;
class QProgressBar;
class QPushButton;
class QTextBrowser;

namespace zproject::app {

// Help → Check for Updates (SPEC.md §21): one modeless window that walks
// through the update. Nothing is installed until the package and its signed
// SHA256SUMS have verified and the user then chooses Install.
//   Checking → UpToDate | CheckFailed | Available
//   Available (Download and Verify / Later / Skip This Version)
//   → Downloading (Cancel) → Verified (Install… / Later) | Refused
//   Verified → Installing → Installed (Restart Now) | InstallFailed
class UpdateDialog : public QDialog {
  Q_OBJECT

 public:
  enum class State { Checking, UpToDate, CheckFailed, Available, Downloading, Verified, Refused, Installing,
                     Installed, InstallFailed };
  Q_ENUM(State)

  explicit UpdateDialog(const QString& currentVersion, QWidget* parent = nullptr);

  State state() const { return state_; }
  void setPlatform(PackagePlatform p) { platform_ = p; }
  void showChecking();
  void showUpToDate(const QString& text);
  void showCheckFailed(const QString& error);
  void showAvailable(const ReleaseInfo& release);
  void showDownloading(const QString& assetName, qint64 got, qint64 total);
  void showVerified(const QString& packagePath, const QString& sha256, const QString& howToInstall);
  void showRefused(const QString& why);
  void showInstalling(const QString& text);
  void showInstalled(const QString& text, bool canRestart);
  void showInstallFailed(const QString& why);

  QLabel* heading() const { return heading_; }
  QLabel* detail() const { return detail_; }
  QTextBrowser* notes() const { return notes_; }
  QProgressBar* progress() const { return progress_; }
  QPushButton* primaryButton() const { return primary_; }
  QPushButton* laterButton() const { return later_; }
  QPushButton* skipButton() const { return skip_; }
  QPushButton* releasePageButton() const { return page_; }

  // https-only; tests replace the opener.
  void setOpenUrlHandler(std::function<void(const QUrl&)> h) { openUrl_ = std::move(h); }

 signals:
  void downloadRequested();
  void installRequested();
  void skipRequested();
  void cancelRequested();
  void retryRequested();
  void restartRequested();

 private:
  void setState(State s);
  void setButtons(const QString& primary, const QString& later, bool skip);

  QString current_;
  ReleaseInfo release_;
  PackagePlatform platform_ = hostPackagePlatform();
  State state_ = State::Checking;
  QLabel* heading_ = nullptr;
  QLabel* detail_ = nullptr;
  QTextBrowser* notes_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QPushButton* primary_ = nullptr;
  QPushButton* later_ = nullptr;
  QPushButton* skip_ = nullptr;
  QPushButton* page_ = nullptr;
  std::function<void(const QUrl&)> openUrl_;
};

}  // namespace zproject::app
