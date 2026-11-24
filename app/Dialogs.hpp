#pragma once

#include <QDialog>
#include <QString>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace zproject::app {

// What About and Preferences show about the connection. Labels only: the
// token secret never reaches a dialog.
struct ConnectionInfo {
  QString serverUrl;
  QString urlSource;
  QString tokenLabel;   // "zpt_<id>"
  QString tokenSource;  // "file <path>", "keychain", "ZPROJECT_TOKEN"
  QString serverVersion;
  QString status;
  QString cachePath;
};

// Help → About zproject (zwriter's pattern): versions, license, links.
class AboutDialog : public QDialog {
  Q_OBJECT
 public:
  AboutDialog(const ConnectionInfo& info, QWidget* parent = nullptr);
  QLabel* details() const { return details_; }
  // https-only; tests replace the opener.
  void setOpenUrlHandler(std::function<void(const QUrl&)> h) { openUrl_ = std::move(h); }

 private:
  QLabel* details_ = nullptr;
  std::function<void(const QUrl&)> openUrl_;
};

// Help → Keyboard Shortcuts: the list from Shortcuts.hpp.
class ShortcutsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit ShortcutsDialog(QWidget* parent = nullptr);
  QTableWidget* table() const { return table_; }

 private:
  QTableWidget* table_ = nullptr;
};

// File → Preferences (Ctrl+,; zproject → Settings on macOS). Changes apply
// at once; the window saves them to zproject.ini.
class PreferencesDialog : public QDialog {
  Q_OBJECT
 public:
  PreferencesDialog(const ConnectionInfo& info, const QString& theme, int zoomPercent, bool cacheEnabled,
                    bool updateCheckDaily, QWidget* parent = nullptr);

  void setConnectionInfo(const ConnectionInfo& info);
  void setClearResult(const QString& text);

  QComboBox* themeBox() const { return theme_; }
  QSpinBox* zoomBox() const { return zoom_; }
  QCheckBox* cacheBox() const { return cache_; }
  QCheckBox* updatesBox() const { return updates_; }
  QPushButton* checkNowButton() const { return checkNow_; }
  QPushButton* clearCacheButton() const { return clear_; }
  QPushButton* signInButton() const { return signIn_; }
  QLabel* clearResult() const { return clearResult_; }
  QLabel* connectionLabel() const { return connection_; }

 signals:
  void themeChosen(const QString& theme);
  void zoomChosen(int percent);
  void cacheEnabledChosen(bool enabled);
  void updateCheckChosen(bool daily);
  void checkForUpdatesRequested();
  void clearCacheRequested();
  void signInRequested();

 private:
  QComboBox* theme_ = nullptr;
  QSpinBox* zoom_ = nullptr;
  QCheckBox* cache_ = nullptr;
  QCheckBox* updates_ = nullptr;
  QPushButton* checkNow_ = nullptr;
  QPushButton* clear_ = nullptr;
  QPushButton* signIn_ = nullptr;
  QLabel* clearResult_ = nullptr;
  QLabel* connection_ = nullptr;
};

}  // namespace zproject::app
