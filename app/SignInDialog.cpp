#include "app/SignInDialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace zproject::app {

SignInDialog::SignInDialog(const QString& url, const QString& existingLabel, const QString& store,
                           const QString& reason, QWidget* parent)
    : QDialog(parent) {
  setObjectName(QStringLiteral("signInDialog"));
  setWindowTitle(tr("Connect to zproject"));
  auto* layout = new QVBoxLayout(this);

  auto* intro = new QLabel(reason.isEmpty() ? tr("Connect this computer to your zproject server.") : reason, this);
  intro->setWordWrap(true);
  intro->setObjectName(QStringLiteral("signInReason"));
  layout->addWidget(intro);

  auto* form = new QFormLayout;
  url_ = new QLineEdit(url, this);
  url_->setObjectName(QStringLiteral("serverUrl"));
  url_->setPlaceholderText(QStringLiteral("https://server-host.example.ts.net"));
  url_->setAccessibleName(tr("Server URL"));
  url_->setAccessibleDescription(tr("The https address published by tailscale serve"));
  form->addRow(tr("&Server URL:"), url_);
  urlStatus_ = new QLabel(this);
  urlStatus_->setObjectName(QStringLiteral("urlStatus"));
  urlStatus_->setWordWrap(true);
  form->addRow(QString(), urlStatus_);
  layout->addLayout(form);

  if (!existingLabel.isEmpty()) {
    useExisting_ = new QRadioButton(tr("Use this machine's zproject token (%1…)").arg(existingLabel), this);
    useExisting_->setObjectName(QStringLiteral("useExistingToken"));
    useExisting_->setChecked(true);
    useExisting_->setAccessibleName(tr("Use this machine's zproject token"));
    pasteNew_ = new QRadioButton(tr("Paste a different device token"), this);
    pasteNew_->setObjectName(QStringLiteral("pasteNewToken"));
    pasteNew_->setAccessibleName(tr("Paste a different device token"));
    layout->addWidget(useExisting_);
    layout->addWidget(pasteNew_);
    connect(useExisting_, &QRadioButton::toggled, this, [this] { validate(); });
  }

  auto* tokenForm = new QFormLayout;
  token_ = new QLineEdit(this);
  token_->setObjectName(QStringLiteral("deviceToken"));
  token_->setEchoMode(QLineEdit::Password);  // never shown; copy and cut are off too
  token_->setPlaceholderText(tr("zpt_…"));
  token_->setAccessibleName(tr("Device token"));
  token_->setAccessibleDescription(
      tr("Create one on the server with: zproject-cli token create --kind device. It is never shown."));
  tokenForm->addRow(tr("Device &token:"), token_);
  tokenStatus_ = new QLabel(this);
  tokenStatus_->setObjectName(QStringLiteral("tokenStatus"));
  tokenStatus_->setWordWrap(true);
  tokenForm->addRow(QString(), tokenStatus_);
  store_ = new QComboBox(this);
  store_->setObjectName(QStringLiteral("tokenStore"));
  store_->addItem(tr("File (0600, shared with zproject-cli)"), QStringLiteral("file"));
  store_->addItem(tr("Keychain"), QStringLiteral("keychain"));
  store_->setCurrentIndex(store == QLatin1String("keychain") ? 1 : 0);
  store_->setAccessibleName(tr("Where to keep the token"));
  tokenForm->addRow(tr("Keep it in:"), store_);
  layout->addLayout(tokenForm);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  connect_ = buttons->addButton(tr("Connect"), QDialogButtonBox::AcceptRole);
  connect_->setObjectName(QStringLiteral("connectButton"));
  connect_->setDefault(true);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  connect(url_, &QLineEdit::textChanged, this, [this] { validate(); });
  connect(token_, &QLineEdit::textChanged, this, [this] {
    if (pasteNew_ && !token_->text().isEmpty()) pasteNew_->setChecked(true);
    validate();
  });
  validate();
  resize(520, sizeHint().height());
}

QString SignInDialog::url() const { return url_->text().trimmed(); }
bool SignInDialog::useExistingToken() const { return useExisting_ != nullptr && useExisting_->isChecked(); }
QString SignInDialog::pastedToken() const { return token_->text().trimmed(); }
QString SignInDialog::store() const { return store_->currentData().toString(); }

void SignInDialog::validate() {
  bool ok = true;
  auto u = client::parseServerUrl(url());
  if (url().isEmpty()) {
    urlStatus_->setText(tr("Enter the server's https:// address."));
    ok = false;
  } else if (!u) {
    urlStatus_->setText(u.error().message);
    ok = false;
  } else {
    urlStatus_->clear();
  }
  const bool existing = useExistingToken();
  token_->setEnabled(!existing);
  store_->setEnabled(!existing);
  if (!existing) {
    const QString t = pastedToken();
    if (t.isEmpty()) {
      tokenStatus_->setText(tr("Paste a device token. It is never shown."));
      ok = false;
    } else if (!client::looksLikeToken(t)) {
      tokenStatus_->setText(tr("That is not a zproject token (zpt_…)."));
      ok = false;
    } else {
      tokenStatus_->setText(tr("Token %1… will be kept in the %2 store.").arg(client::tokenLabel(t), store()));
    }
  } else {
    tokenStatus_->clear();
  }
  connect_->setEnabled(ok);
}

Status saveSignIn(const SignInDialog& dialog, client::ClientConfig& cli, const client::TokenStore& store,
                  const client::DesktopPaths& paths, client::DesktopSettings& settings) {
  auto u = client::parseServerUrl(dialog.url());
  if (!u) return u.error();
  const QString url = u->toString();
  if (!dialog.useExistingToken()) {
    const QString token = dialog.pastedToken();
    const Status s = dialog.store() == QLatin1String("keychain") ? store.writeKeychain(token) : store.writeFile(token);
    if (!s) return s;
    cli.store = dialog.store();
  }
  client::ClientConfig toSave = cli;
  if (cli.urlSource.isEmpty()) {
    // zproject-cli has no server yet: both use this one.
    cli.url = url;
    cli.urlSource = QStringLiteral("cli.ini");
    toSave.url = url;
    settings.serverUrl.clear();
  } else {
    // Keep cli.ini's (or ZPROJECT_URL's) URL; a different one is the app's own.
    if (cli.urlSource != QLatin1String("cli.ini")) toSave.url.clear();
    auto current = client::parseServerUrl(cli.url);
    if (current && current->toString() == url) {
      settings.serverUrl.clear();
    } else {
      settings.serverUrl = url;
    }
  }
  if (auto s = client::saveClientConfig(toSave); !s) return s;
  return client::saveDesktopSettings(paths, settings);
}

}  // namespace zproject::app
