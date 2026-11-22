#pragma once

#include <QDialog>
#include <QUrl>

#include <functional>

class QTextBrowser;

namespace zproject::app {

// Help → zproject Help (F1), zwriter's pattern: one scrollable guide from
// the embedded Markdown resource :/help/zproject-help.md. External links
// open only if they are https; in-page #anchors scroll the guide. Modeless:
// opening Help again raises the existing window.
class HelpDialog : public QDialog {
  Q_OBJECT

 public:
  explicit HelpDialog(QWidget* parent = nullptr);

  QTextBrowser* browser() const { return browser_; }
  QString sourceMarkdown() const { return source_; }

  // Tests replace the desktop-open call so links are not launched.
  void setOpenUrlHandler(std::function<void(const QUrl&)> handler) { openUrl_ = std::move(handler); }
  QUrl lastOpenedUrl() const { return lastOpened_; }
  // True if the URL was an in-page jump or an https link (which opens).
  bool handleAnchor(const QUrl& url);

  static QString resourcePath() { return QStringLiteral(":/help/zproject-help.md"); }
  static QString slugify(const QString& heading);

 private:
  void assignHeadingAnchors();

  QTextBrowser* browser_ = nullptr;
  QString source_;
  QUrl lastOpened_;
  std::function<void(const QUrl&)> openUrl_;
};

// Opens an https URL in the browser; anything else is refused (false).
// Shared by Help, About and the Help menu's links.
bool openHttpsUrl(const QUrl& url, const std::function<void(const QUrl&)>& opener = {});

}  // namespace zproject::app
