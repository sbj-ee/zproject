#include "app/Dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtGlobal>

#include "app/HelpDialog.hpp"
#include "app/Shortcuts.hpp"
#include "app/Theme.hpp"
#include "core/Version.hpp"

namespace zproject::app {

namespace {
QString orDash(const QString& s) { return s.isEmpty() ? QStringLiteral("—") : s.toHtmlEscaped(); }

QString connectionHtml(const ConnectionInfo& i) {
  return QObject::tr("<table cellspacing='2'>"
                     "<tr><td>Server:</td><td>%1 <span style='color:gray'>(%2)</span></td></tr>"
                     "<tr><td>Status:</td><td>%3</td></tr>"
                     "<tr><td>Server version:</td><td>%4</td></tr>"
                     "<tr><td>Device token:</td><td>%5… <span style='color:gray'>(%6)</span></td></tr>"
                     "<tr><td>Cache:</td><td>%7</td></tr></table>")
      .arg(orDash(i.serverUrl), orDash(i.urlSource), orDash(i.status), orDash(i.serverVersion), orDash(i.tokenLabel),
           orDash(i.tokenSource), orDash(i.cachePath));
}
}  // namespace

AboutDialog::AboutDialog(const ConnectionInfo& info, QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("aboutDialog"));
  setWindowTitle(tr("About zproject"));
  auto* layout = new QVBoxLayout(this);
  auto* top = new QHBoxLayout;
  auto* icon = new QLabel(this);
  icon->setPixmap(QPixmap(QStringLiteral(":/icons/zproject-256.png"))
                      .scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  icon->setAccessibleName(tr("zproject icon"));
  top->addWidget(icon, 0, Qt::AlignTop);
  auto* title = new QLabel(tr("<h2>zproject %1</h2><p>Project scheduling for one owner and a few people, "
                              "served over your tailnet.</p><p>MIT licence · Qt %2 (built with %3)</p>")
                               .arg(appVersion(), QString::fromLatin1(qVersion()),
                                    QStringLiteral(QT_VERSION_STR)),
                           this);
  title->setObjectName(QStringLiteral("aboutTitle"));
  title->setWordWrap(true);
  top->addWidget(title, 1);
  layout->addLayout(top);

  details_ = new QLabel(connectionHtml(info), this);
  details_->setObjectName(QStringLiteral("aboutDetails"));
  details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(details_);

  auto* links = new QLabel(tr("<a href='https://github.com/sbj-ee/zproject'>Source code</a> · "
                              "<a href='https://github.com/sbj-ee/zproject/blob/main/CHANGELOG.md'>Changes</a>"),
                           this);
  links->setObjectName(QStringLiteral("aboutLinks"));
  links->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
  links->setOpenExternalLinks(false);
  connect(links, &QLabel::linkActivated, this,
          [this](const QString& link) { openHttpsUrl(QUrl(link), openUrl_); });
  layout->addWidget(links);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  layout->addWidget(buttons);
}

ShortcutsDialog::ShortcutsDialog(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("shortcutsDialog"));
  setWindowTitle(tr("Keyboard Shortcuts"));
  auto* layout = new QVBoxLayout(this);
  const auto list = shellShortcuts();
  table_ = new QTableWidget(static_cast<int>(list.size()), 2, this);
  table_->setObjectName(QStringLiteral("shortcutsTable"));
  table_->setAccessibleName(tr("Keyboard shortcuts"));
  table_->setHorizontalHeaderLabels({tr("Action"), tr("Keys")});
  table_->verticalHeader()->hide();
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  for (int r = 0; r < list.size(); ++r) {
    QStringList keys;
    for (const auto& k : list[r].keys) keys << k.toString(QKeySequence::NativeText);
    table_->setItem(r, 0, new QTableWidgetItem(list[r].label));
    table_->setItem(r, 1, new QTableWidgetItem(keys.join(QStringLiteral(", "))));
  }
  layout->addWidget(table_);
  auto* note = new QLabel(tr("Ctrl is ⌘ on macOS. Gantt zoom (later versions) uses Ctrl+Shift+= / − and Ctrl+wheel."),
                          this);
  note->setWordWrap(true);
  layout->addWidget(note);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  layout->addWidget(buttons);
  resize(520, 460);
}

PreferencesDialog::PreferencesDialog(const ConnectionInfo& info, const QString& theme, int zoomPercent,
                                     bool cacheEnabled, QWidget* parent)
    : QDialog(parent) {
  setObjectName(QStringLiteral("preferencesDialog"));
  setWindowTitle(tr("Preferences"));
  auto* layout = new QVBoxLayout(this);

  auto* look = new QGroupBox(tr("Appearance"), this);
  auto* lookForm = new QFormLayout(look);
  theme_ = new QComboBox(look);
  theme_->setObjectName(QStringLiteral("themeBox"));
  theme_->setAccessibleName(tr("Theme"));
  for (const QString& t : Theme::choices()) theme_->addItem(Theme::displayName(t), t);
  theme_->setCurrentIndex(qMax(0, theme_->findData(theme)));
  lookForm->addRow(tr("&Theme:"), theme_);
  zoom_ = new QSpinBox(look);
  zoom_->setObjectName(QStringLiteral("zoomBox"));
  zoom_->setAccessibleName(tr("Text size"));
  zoom_->setRange(ViewZoom::kMinPercent, ViewZoom::kMaxPercent);
  zoom_->setSingleStep(10);
  zoom_->setSuffix(QStringLiteral(" %"));
  zoom_->setValue(zoomPercent);
  lookForm->addRow(tr("Text &size:"), zoom_);
  layout->addWidget(look);

  auto* cacheGroup = new QGroupBox(tr("Offline cache"), this);
  auto* cacheLayout = new QVBoxLayout(cacheGroup);
  cache_ = new QCheckBox(tr("Keep a read-only copy for offline use"), cacheGroup);
  cache_->setObjectName(QStringLiteral("cacheBox"));
  cache_->setChecked(cacheEnabled);
  cacheLayout->addWidget(cache_);
  auto* row = new QHBoxLayout;
  clear_ = new QPushButton(tr("&Clear cache"), cacheGroup);
  clear_->setObjectName(QStringLiteral("clearCacheButton"));
  clear_->setAccessibleDescription(tr("Deletes the local copy and loads everything again from the server"));
  row->addWidget(clear_);
  clearResult_ = new QLabel(cacheGroup);
  clearResult_->setObjectName(QStringLiteral("clearResult"));
  row->addWidget(clearResult_, 1);
  cacheLayout->addLayout(row);
  layout->addWidget(cacheGroup);

  auto* conn = new QGroupBox(tr("Connection"), this);
  auto* connLayout = new QVBoxLayout(conn);
  connection_ = new QLabel(conn);
  connection_->setObjectName(QStringLiteral("connectionInfo"));
  connection_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  connLayout->addWidget(connection_);
  signIn_ = new QPushButton(tr("Change server or token…"), conn);
  signIn_->setObjectName(QStringLiteral("signInButton"));
  connLayout->addWidget(signIn_, 0, Qt::AlignLeft);
  layout->addWidget(conn);
  setConnectionInfo(info);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  layout->addWidget(buttons);

  connect(theme_, &QComboBox::currentIndexChanged, this,
          [this](int) { emit themeChosen(theme_->currentData().toString()); });
  connect(zoom_, &QSpinBox::valueChanged, this, &PreferencesDialog::zoomChosen);
  connect(cache_, &QCheckBox::toggled, this, &PreferencesDialog::cacheEnabledChosen);
  connect(clear_, &QPushButton::clicked, this, &PreferencesDialog::clearCacheRequested);
  connect(signIn_, &QPushButton::clicked, this, &PreferencesDialog::signInRequested);
}

void PreferencesDialog::setConnectionInfo(const ConnectionInfo& info) { connection_->setText(connectionHtml(info)); }
void PreferencesDialog::setClearResult(const QString& text) { clearResult_->setText(text); }

}  // namespace zproject::app
