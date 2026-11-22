#include "app/HelpDialog.hpp"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QPushButton>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

namespace zproject::app {

bool openHttpsUrl(const QUrl& url, const std::function<void(const QUrl&)>& opener) {
  if (url.scheme().toLower() != QLatin1String("https") || url.host().isEmpty()) return false;
  if (opener) {
    opener(url);
  } else {
    QDesktopServices::openUrl(url);
  }
  return true;
}

HelpDialog::HelpDialog(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("helpDialog"));
  setWindowTitle(tr("zproject Help"));
  setModal(false);
  auto* layout = new QVBoxLayout(this);

  browser_ = new QTextBrowser(this);
  browser_->setObjectName(QStringLiteral("helpBrowser"));
  browser_->setAccessibleName(tr("zproject Help"));
  browser_->setReadOnly(true);
  browser_->setOpenLinks(false);
  browser_->setOpenExternalLinks(false);
  browser_->document()->setDocumentMargin(20);
  connect(browser_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) { handleAnchor(url); });
  layout->addWidget(browser_, 1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  if (QPushButton* close = buttons->button(QDialogButtonBox::Close)) close->setDefault(true);
  layout->addWidget(buttons);

  QFile f(resourcePath());
  if (f.open(QIODevice::ReadOnly | QIODevice::Text)) source_ = QString::fromUtf8(f.readAll());
  browser_->document()->setMarkdown(source_.isEmpty() ? tr("*The help guide could not be loaded.*") : source_,
                                    QTextDocument::MarkdownDialectGitHub);
  assignHeadingAnchors();
  browser_->moveCursor(QTextCursor::Start);
  resize(720, 640);
}

QString HelpDialog::slugify(const QString& heading) {
  QString s;
  for (const QChar ch : heading.trimmed().toLower()) {
    if (ch.isLetterOrNumber()) {
      s += ch;
    } else if (!s.isEmpty() && !s.endsWith(QLatin1Char('-'))) {
      s += QLatin1Char('-');
    }
  }
  while (s.endsWith(QLatin1Char('-'))) s.chop(1);
  return s;
}

void HelpDialog::assignHeadingAnchors() {
  QTextDocument* doc = browser_->document();
  for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
    if (block.blockFormat().headingLevel() <= 0) continue;
    const QString name = slugify(block.text());
    const int len = block.length() - 1;
    if (name.isEmpty() || len <= 0) continue;
    QTextCursor c(doc);
    c.setPosition(block.position());
    c.setPosition(block.position() + len, QTextCursor::KeepAnchor);
    QTextCharFormat fmt;
    fmt.setAnchor(true);
    fmt.setAnchorNames({name});
    c.mergeCharFormat(fmt);
  }
}

bool HelpDialog::handleAnchor(const QUrl& url) {
  lastOpened_ = QUrl();
  const QString scheme = url.scheme().toLower();
  if (scheme == QLatin1String("https")) {
    if (!openHttpsUrl(url, openUrl_)) return false;
    lastOpened_ = url;
    return true;
  }
  if (url.hasFragment() && (scheme.isEmpty() || scheme == QLatin1String("qrc"))) {
    browser_->scrollToAnchor(url.fragment());
    return true;
  }
  return false;
}

}  // namespace zproject::app
