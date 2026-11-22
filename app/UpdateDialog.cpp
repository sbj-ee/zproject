#include "app/UpdateDialog.hpp"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "app/HelpDialog.hpp"
#include "app/Minisign.hpp"

namespace zproject::app {

namespace {
QString signingKeyId() {
  const auto key = MinisignPublicKey::parse(updateSigningPublicKey());
  return key ? key->keyIdHex() : QStringLiteral("?");
}
}  // namespace

UpdateDialog::UpdateDialog(const QString& currentVersion, QWidget* parent)
    : QDialog(parent), current_(currentVersion) {
  setObjectName(QStringLiteral("updateDialog"));
  setWindowTitle(tr("Software Update"));
  setModal(false);
  auto* layout = new QVBoxLayout(this);

  auto* top = new QHBoxLayout;
  auto* icon = new QLabel(this);
  icon->setPixmap(QPixmap(QStringLiteral(":/icons/zproject-256.png"))
                      .scaled(56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  icon->setAccessibleName(tr("zproject icon"));
  top->addWidget(icon, 0, Qt::AlignTop);
  auto* text = new QVBoxLayout;
  heading_ = new QLabel(this);
  heading_->setObjectName(QStringLiteral("updateHeading"));
  heading_->setWordWrap(true);
  text->addWidget(heading_);
  detail_ = new QLabel(this);
  detail_->setObjectName(QStringLiteral("updateDetail"));
  detail_->setWordWrap(true);
  detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  detail_->setMinimumWidth(460);
  text->addWidget(detail_);
  top->addLayout(text, 1);
  layout->addLayout(top);

  notes_ = new QTextBrowser(this);
  notes_->setObjectName(QStringLiteral("releaseNotes"));
  notes_->setAccessibleName(tr("Release notes"));
  notes_->setOpenLinks(false);
  connect(notes_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) { openHttpsUrl(url, openUrl_); });
  notes_->setMinimumSize(520, 200);
  layout->addWidget(notes_, 1);

  progress_ = new QProgressBar(this);
  progress_->setObjectName(QStringLiteral("updateProgress"));
  progress_->setAccessibleName(tr("Download progress"));
  layout->addWidget(progress_);

  auto* buttons = new QDialogButtonBox(this);
  page_ = buttons->addButton(tr("Release Page"), QDialogButtonBox::HelpRole);
  page_->setAutoDefault(false);
  page_->setObjectName(QStringLiteral("updateReleasePage"));
  skip_ = buttons->addButton(tr("&Skip This Version"), QDialogButtonBox::DestructiveRole);
  skip_->setObjectName(QStringLiteral("updateSkip"));
  later_ = buttons->addButton(tr("&Later"), QDialogButtonBox::RejectRole);
  later_->setObjectName(QStringLiteral("updateLater"));
  primary_ = buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
  primary_->setObjectName(QStringLiteral("updatePrimary"));
  layout->addWidget(buttons);

  connect(page_, &QPushButton::clicked, this, [this] {
    if (isTrustedReleasePageUrl(release_.htmlUrl)) openHttpsUrl(QUrl(release_.htmlUrl), openUrl_);
  });
  connect(skip_, &QPushButton::clicked, this, [this] {
    emit skipRequested();
    close();
  });
  connect(later_, &QPushButton::clicked, this, [this] {
    if (state_ == State::Downloading) {
      emit cancelRequested();
    } else {
      close();
    }
  });
  connect(primary_, &QPushButton::clicked, this, [this] {
    switch (state_) {
      case State::Available:
        emit downloadRequested();
        break;
      case State::Verified:
        emit installRequested();
        break;
      case State::CheckFailed:
        emit retryRequested();
        break;
      case State::Installed:
        emit restartRequested();
        break;
      default:
        close();
        break;
    }
  });
  showChecking();
  resize(620, 300);
}

void UpdateDialog::setState(State s) {
  state_ = s;
  const bool notes = s == State::Available || s == State::Downloading || s == State::Verified;
  notes_->setVisible(notes);
  progress_->setVisible(s == State::Checking || s == State::Downloading || s == State::Installing);
  page_->setVisible(isTrustedReleasePageUrl(release_.htmlUrl) && s != State::Checking && s != State::UpToDate &&
                    s != State::CheckFailed);
  adjustSize();
  // Focus (and so the default) on the main choice, never the notes or the page.
  if (!primary_->isHidden() && primary_->isEnabled()) {
    primary_->setFocus(Qt::OtherFocusReason);
  } else if (!later_->isHidden()) {
    later_->setFocus(Qt::OtherFocusReason);
  }
}

void UpdateDialog::setButtons(const QString& primary, const QString& later, bool skip) {
  primary_->setVisible(!primary.isEmpty());
  primary_->setText(primary);
  primary_->setEnabled(true);
  primary_->setDefault(!primary.isEmpty());
  later_->setVisible(!later.isEmpty());
  later_->setText(later);
  later_->setDefault(primary.isEmpty());
  skip_->setVisible(skip);
  skip_->setAutoDefault(false);
}

void UpdateDialog::showChecking() {
  heading_->setText(tr("<h3>Checking for updates…</h3>"));
  detail_->setText(tr("Asking GitHub for the latest zproject release. You have %1.").arg(current_.toHtmlEscaped()));
  progress_->setRange(0, 0);
  setButtons(QString(), tr("Close"), false);
  setState(State::Checking);
}

void UpdateDialog::showUpToDate(const QString& text) {
  heading_->setText(tr("<h3>zproject is up to date</h3>"));
  detail_->setText(text.toHtmlEscaped());
  setButtons(tr("OK"), QString(), false);
  setState(State::UpToDate);
}

void UpdateDialog::showCheckFailed(const QString& error) {
  heading_->setText(tr("<h3>Couldn't check for updates</h3>"));
  detail_->setText(error.toHtmlEscaped());
  setButtons(tr("Try Again"), tr("Close"), false);
  setState(State::CheckFailed);
}

void UpdateDialog::showAvailable(const ReleaseInfo& release) {
  release_ = release;
  heading_->setText(tr("<h3>zproject %1 is available</h3>").arg(release.tag.toHtmlEscaped()));
  QString sub = tr("You have %1.").arg(current_.toHtmlEscaped());
  if (release.publishedAt.isValid()) {
    sub += tr(" Released %1.").arg(QLocale().toString(release.publishedAt.toLocalTime().date(), QLocale::ShortFormat));
  }
  if (release.prerelease) sub += tr(" <b>This is a pre-release.</b>");
  const auto pkg = release.packageAsset(platform_);
  const bool installable = pkg && release.checksumAsset() && release.signatureAsset();
  if (installable) {
    sub += tr("<br><small>Download and Verify fetches %1 and the release's SHA256SUMS, checks the minisign "
              "signature (key %2) and the SHA-256, and only then offers to install. Only the "
              "desktop app is updated; the zprojectd server is never touched.</small>")
               .arg(pkg->name.toHtmlEscaped(), signingKeyId());
  } else {
    sub += tr("<br><small>This release has no %1 with a signed SHA256SUMS, so it can't be installed from here. "
              "Use the release page.</small>")
               .arg(ReleaseInfo::packageName(release.versionString(), platform_).toHtmlEscaped());
  }
  detail_->setText(sub);
  notes_->setMarkdown(release.notes.trimmed().isEmpty() ? tr("*No release notes.*") : release.notes);
  setButtons(tr("&Download and Verify"), tr("&Later"), true);
  primary_->setEnabled(installable);
  if (!installable) later_->setDefault(true);
  setState(State::Available);
}

void UpdateDialog::showDownloading(const QString& assetName, qint64 got, qint64 total) {
  heading_->setText(tr("<h3>Downloading zproject %1…</h3>").arg(release_.tag.toHtmlEscaped()));
  detail_->setText(tr("%1 — the signature is checked before the package is downloaded.").arg(assetName.toHtmlEscaped()));
  if (total > 0) {
    progress_->setRange(0, 1000);
    progress_->setValue(static_cast<int>(got * 1000 / total));
  } else {
    progress_->setRange(0, 0);
  }
  if (state_ != State::Downloading) {
    setButtons(QString(), tr("Cancel"), false);
    setState(State::Downloading);
  }
}

void UpdateDialog::showVerified(const QString& packagePath, const QString& sha256, const QString& howToInstall) {
  heading_->setText(tr("<h3>zproject %1 is verified and ready</h3>").arg(release_.tag.toHtmlEscaped()));
  detail_->setText(tr("✔ SHA256SUMS signature verified (minisign key %1, “%2”)<br>"
                      "✔ SHA-256 of %3 matches: <tt>%4</tt><br><br>%5")
                       .arg(signingKeyId(), expectedSignedComment(release_.tag).toHtmlEscaped(),
                            QFileInfo(packagePath).fileName().toHtmlEscaped(), sha256.left(16) + QStringLiteral("…"),
                            howToInstall.toHtmlEscaped()));
  setButtons(tr("&Install…"), tr("&Later"), false);
  setState(State::Verified);
}

void UpdateDialog::showRefused(const QString& why) {
  heading_->setText(tr("<h3>Update refused</h3>"));
  detail_->setText(tr("%1<br><br>Any downloaded files were deleted.")
                       .arg(why.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
  setButtons(QString(), tr("Close"), false);
  setState(State::Refused);
}

void UpdateDialog::showInstalling(const QString& text) {
  heading_->setText(tr("<h3>Installing zproject %1…</h3>").arg(release_.tag.toHtmlEscaped()));
  detail_->setText(text.toHtmlEscaped());
  progress_->setRange(0, 0);
  setButtons(QString(), QString(), false);
  setState(State::Installing);
}

void UpdateDialog::showInstalled(const QString& text, bool canRestart) {
  heading_->setText(tr("<h3>zproject %1 is installed</h3>").arg(release_.tag.toHtmlEscaped()));
  detail_->setText(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
  setButtons(canRestart ? tr("&Restart Now") : tr("OK"), canRestart ? tr("&Later") : QString(), false);
  setState(State::Installed);
}

void UpdateDialog::showInstallFailed(const QString& why) {
  heading_->setText(tr("<h3>The update was not installed</h3>"));
  detail_->setText(why.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
  setButtons(QString(), tr("Close"), false);
  setState(State::InstallFailed);
}

}  // namespace zproject::app
