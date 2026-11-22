#pragma once

#include <QDialog>
#include <QString>

#include "client/ClientConfig.hpp"
#include "client/DesktopSettings.hpp"
#include "client/TokenStore.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace zproject::app {

// First run, or after the server refused the token (SPEC.md §8.1, §16.5).
// The server URL comes prefilled from zproject.ini or cli.ini. If this
// machine's zproject-cli already has a token, the dialog offers it by label
// only ("zpt_<id>"); otherwise the user pastes a device token made with
// `zproject-cli token create --kind device`. The secret is typed into a
// password field and never shown, logged or put in a label: only its label is.
class SignInDialog : public QDialog {
  Q_OBJECT

 public:
  // existingLabel: the CLI token's label, or empty when there is none (or it
  // was refused). reason: why the dialog is shown (first run, 401, ...).
  SignInDialog(const QString& url, const QString& existingLabel, const QString& store, const QString& reason,
               QWidget* parent = nullptr);

  QString url() const;
  bool useExistingToken() const;
  // The pasted secret. Callers store it and drop it; never display it.
  QString pastedToken() const;
  QString store() const;  // "file" or "keychain"

  QLineEdit* urlEdit() const { return url_; }
  QLineEdit* tokenEdit() const { return token_; }
  QPushButton* connectButton() const { return connect_; }
  QLabel* tokenStatus() const { return tokenStatus_; }

 private:
  void validate();

  QLineEdit* url_ = nullptr;
  QLabel* urlStatus_ = nullptr;
  QRadioButton* useExisting_ = nullptr;
  QRadioButton* pasteNew_ = nullptr;
  QLineEdit* token_ = nullptr;
  QLabel* tokenStatus_ = nullptr;
  QComboBox* store_ = nullptr;
  QPushButton* connect_ = nullptr;
};

// Writes what the dialog collected where zproject-cli reads it, so the CLI
// and the app keep one device identity: the pasted token into the chosen
// store (0600 file or Keychain, on stdin), `[auth] store` and, when cli.ini
// has no URL yet, `[server] url` into cli.ini; otherwise a different URL goes
// to zproject.ini. Never writes the token anywhere else.
Status saveSignIn(const SignInDialog& dialog, client::ClientConfig& cli, const client::TokenStore& store,
                  const client::DesktopPaths& paths, client::DesktopSettings& settings);

}  // namespace zproject::app
