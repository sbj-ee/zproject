#include "app/MainWindow.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QSpinBox>
#include <QDockWidget>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyleHints>
#include <QTimer>
#include <QVBoxLayout>

#include "app/HelpDialog.hpp"
#include "app/Shortcuts.hpp"
#include "app/SignInDialog.hpp"
#include "app/Theme.hpp"
#include "app/UpdateManager.hpp"
#include "core/Version.hpp"

namespace zproject::app {

namespace {

const char* const kApiDocsUrl = "https://github.com/sbj-ee/zproject/blob/main/api/openapi.yaml";
const char* const kIssuesUrl = "https://github.com/sbj-ee/zproject/issues";

QHash<QString, QString> processEnv() {
  QHash<QString, QString> env;
  const QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
  for (const QString& k : pe.keys()) env.insert(k, pe.value(k));
  return env;
}

bool isMacos() {
#ifdef Q_OS_MACOS
  return true;
#else
  return false;
#endif
}

QString formatWhen(const QDateTime& t) {
  if (!t.isValid()) return {};
  const QDateTime local = t.toLocalTime();
  const QLocale loc;
  if (local.date() == QDate::currentDate()) return loc.toString(local.time(), QLocale::ShortFormat);
  return loc.toString(local, QLocale::ShortFormat);
}

}  // namespace

MainWindow::Options MainWindow::defaultOptions() {
  Options o;
  o.env = processEnv();
  o.macos = isMacos();
  o.paths = client::desktopPaths(o.env, o.macos);
  o.runner = client::runCommand;
  o.systemPalette = QGuiApplication::palette();
  return o;
}

MainWindow::MainWindow(Options options, QWidget* parent) : QMainWindow(parent), options_(std::move(options)) {
  setObjectName(QStringLiteral("mainWindow"));
  setWindowTitle(QStringLiteral("zproject"));
  setAccessibleName(tr("zproject"));
  settings_ = client::loadDesktopSettings(options_.paths);
  settings_.zoomPercent = ViewZoom::clampPercent(settings_.zoomPercent);
  if (!Theme::choices().contains(settings_.theme)) settings_.theme = QStringLiteral("system");

  updater_ = new UpdateManager({updateStatePath(), appVersion(), hostPackagePlatform()}, this);
  updater_->setOpenUrlHandler([this](const QUrl& u) { openUrl(u); });
  buildCentral();
  buildDocks();
  buildMenus();
  buildStatusBar();
  applyAppearance();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
    if (settings_.theme == QLatin1String("system")) applyAppearance();
  });
#endif
  resize(1100, 700);
  if (options_.autoConnect) connectToServer();
  updateConnectionUi();
  if (options_.startupUpdateCheck) {
    QTimer::singleShot(3000, this, [this] {
      if (!signIn_) startUpdates();
    });
  }
}

MainWindow::~MainWindow() {
  session_.reset();
}

QAction* MainWindow::action(const QString& objectName) const { return actions_.value(objectName); }

// ---- layout ----

void MainWindow::buildCentral() {
  auto* central = new QWidget(this);
  auto* v = new QVBoxLayout(central);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);

  banner_ = new QFrame(central);
  banner_->setObjectName(QStringLiteral("offlineBanner"));
  banner_->setAccessibleName(tr("Connection notice"));
  auto* bh = new QHBoxLayout(banner_);
  bh->setContentsMargins(14, 8, 10, 8);
  bannerText_ = new QLabel(banner_);
  bannerText_->setObjectName(QStringLiteral("bannerText"));
  bannerText_->setWordWrap(true);
  bh->addWidget(bannerText_, 1);
  signInButton_ = new QPushButton(tr("Sign in…"), banner_);
  signInButton_->setObjectName(QStringLiteral("bannerSignIn"));
  connect(signInButton_, &QPushButton::clicked, this, [this] { showSignIn(); });
  bh->addWidget(signInButton_);
  retry_ = new QPushButton(tr("Retry now"), banner_);
  retry_->setObjectName(QStringLiteral("bannerRetry"));
  connect(retry_, &QPushButton::clicked, this, [this] {
    if (session_) session_->refresh();
  });
  bh->addWidget(retry_);
  banner_->hide();
  v->addWidget(banner_);

  stack_ = new QStackedWidget(central);
  empty_ = new QLabel(stack_);
  empty_->setObjectName(QStringLiteral("emptyState"));
  empty_->setAlignment(Qt::AlignCenter);
  empty_->setWordWrap(true);
  stack_->addWidget(empty_);

  auto* page = new QWidget(stack_);
  auto* pv = new QVBoxLayout(page);
  pv->setContentsMargins(28, 22, 28, 22);
  projectTitle_ = new QLabel(page);
  projectTitle_->setObjectName(QStringLiteral("projectTitle"));
  projectTitle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  pv->addWidget(projectTitle_);
  projectSummary_ = new QLabel(page);
  projectSummary_->setObjectName(QStringLiteral("projectSummary"));
  projectSummary_->setWordWrap(true);
  projectSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  pv->addWidget(projectSummary_);
  pv->addStretch(1);
  stack_->addWidget(page);
  v->addWidget(stack_, 1);
  setCentralWidget(central);
}

void MainWindow::buildDocks() {
  sidebarDock_ = new QDockWidget(tr("Projects"), this);
  sidebarDock_->setObjectName(QStringLiteral("sidebarDock"));
  sidebarDock_->setFeatures(QDockWidget::DockWidgetClosable);
  projects_ = new QListWidget(sidebarDock_);
  projects_->setObjectName(QStringLiteral("sidebar"));
  projects_->setAccessibleName(tr("Projects"));
  projects_->setMinimumWidth(220);
  connect(projects_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
    showProject(item ? item->data(Qt::UserRole).toString() : QString());
  });
  sidebarDock_->setWidget(projects_);
  addDockWidget(Qt::LeftDockWidgetArea, sidebarDock_);

  inspectorDock_ = new QDockWidget(tr("Inspector"), this);
  inspectorDock_->setObjectName(QStringLiteral("inspectorDock"));
  inspectorDock_->setFeatures(QDockWidget::DockWidgetClosable);
  inspector_ = new QLabel(inspectorDock_);
  inspector_->setObjectName(QStringLiteral("inspector"));
  inspector_->setAccessibleName(tr("Inspector"));
  inspector_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  inspector_->setWordWrap(true);
  inspector_->setMargin(12);
  inspector_->setMinimumWidth(240);
  inspector_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  inspectorDock_->setWidget(inspector_);
  addDockWidget(Qt::RightDockWidgetArea, inspectorDock_);
}

void MainWindow::buildMenus() {
  auto add = [this](QMenu* menu, const QString& name, const QString& text, auto slot) {
    auto* a = new QAction(text, this);
    a->setObjectName(name);
    for (const auto& s : shellShortcuts()) {
      if (s.action == name) a->setShortcuts(s.keys);
    }
    connect(a, &QAction::triggered, this, slot);
    menu->addAction(a);
    actions_.insert(name, a);
    return a;
  };

  QMenu* file = menuBar()->addMenu(tr("&File"));
  add(file, QStringLiteral("actionRefresh"), tr("&Refresh"), [this] {
    if (session_) session_->refresh();
  });
  file->addSeparator();
  auto* signIn = add(file, QStringLiteral("actionSignIn"), tr("&Connect to Server…"), [this] { showSignIn(); });
  signIn->setMenuRole(QAction::NoRole);
  auto* prefs = add(file, QStringLiteral("actionPreferences"), tr("&Preferences…"), [this] { showPreferences(); });
  prefs->setMenuRole(QAction::PreferencesRole);
  file->addSeparator();
  auto* quit = add(file, QStringLiteral("actionQuit"), tr("&Quit"), [this] { close(); });
  quit->setMenuRole(QAction::QuitRole);

  QMenu* view = menuBar()->addMenu(tr("&View"));
  QMenu* themes = view->addMenu(tr("&Theme"));
  themeGroup_ = new QActionGroup(this);
  for (const QString& t : Theme::choices()) {
    auto* a = themes->addAction(Theme::displayName(t));
    a->setObjectName(QStringLiteral("actionTheme-") + t);
    a->setCheckable(true);
    a->setData(t);
    themeGroup_->addAction(a);
    actions_.insert(a->objectName(), a);
    connect(a, &QAction::triggered, this, [this, t] { setTheme(t); });
  }
  view->addSeparator();
  add(view, QStringLiteral("actionZoomIn"), tr("Zoom &In"), [this] { zoomIn(); });
  add(view, QStringLiteral("actionZoomOut"), tr("Zoom &Out"), [this] { zoomOut(); });
  add(view, QStringLiteral("actionZoomReset"), tr("&Actual Size"), [this] { zoomReset(); });
  view->addSeparator();
  auto* side = add(view, QStringLiteral("actionToggleSidebar"), tr("&Sidebar"),
                   [this](bool on) { sidebarDock_->setVisible(on); });
  side->setCheckable(true);
  side->setChecked(true);
  connect(sidebarDock_, &QDockWidget::visibilityChanged, side, [this, side](bool) {
    side->setChecked(!sidebarDock_->isHidden());
  });
  auto* insp = add(view, QStringLiteral("actionToggleInspector"), tr("&Inspector"),
                   [this](bool on) { inspectorDock_->setVisible(on); });
  insp->setCheckable(true);
  insp->setChecked(true);
  connect(inspectorDock_, &QDockWidget::visibilityChanged, insp, [this, insp](bool) {
    insp->setChecked(!inspectorDock_->isHidden());
  });
  add(view, QStringLiteral("actionFocusSidebar"), tr("Go to &Projects"), [this] {
    sidebarDock_->show();
    projects_->setFocus(Qt::ShortcutFocusReason);
  });

  QMenu* help = menuBar()->addMenu(tr("&Help"));
  add(help, QStringLiteral("actionHelp"), tr("zproject &Help"), [this] { showHelp(); });
  add(help, QStringLiteral("actionShortcuts"), tr("&Keyboard Shortcuts"), [this] { showShortcuts(); });
  help->addSeparator();
  add(help, QStringLiteral("actionApiDocs"), tr("&API Reference"),
      [this] { openUrl(QUrl(QString::fromLatin1(kApiDocsUrl))); });
  add(help, QStringLiteral("actionReportIssue"), tr("&Report an Issue"),
      [this] { openUrl(QUrl(QString::fromLatin1(kIssuesUrl))); });
  help->addSeparator();
  auto* updates = add(help, QStringLiteral("actionCheckUpdates"), tr("Check for &Updates…"), [this] { checkForUpdates(); });
  updates->setMenuRole(QAction::ApplicationSpecificRole);
  auto* about = add(help, QStringLiteral("actionAbout"), tr("&About zproject"), [this] { showAbout(); });
  about->setMenuRole(QAction::AboutRole);
}

void MainWindow::buildStatusBar() {
  status_ = new QLabel(this);
  status_->setObjectName(QStringLiteral("connectionStatus"));
  status_->setAccessibleName(tr("Connection status"));
  statusBar()->addWidget(status_);
  refreshed_ = new QLabel(this);
  refreshed_->setObjectName(QStringLiteral("refreshStatus"));
  refreshed_->setAccessibleName(tr("Last refresh"));
  statusBar()->addWidget(refreshed_);
  readOnly_ = new QLabel(tr("Read-only"), this);
  readOnly_->setObjectName(QStringLiteral("readOnlyStatus"));
  readOnly_->setAccessibleName(tr("Editing is off"));
  statusBar()->addPermanentWidget(readOnly_);
  statusBar()->setSizeGripEnabled(false);
}

// ---- appearance ----

void MainWindow::applyAppearance() {
  resolvedTheme_ = Theme::resolve(settings_.theme, options_.systemPalette);
  const ThemeColors c = Theme::colors(resolvedTheme_);
  QApplication::setPalette(Theme::palette(c));
  qApp->setStyleSheet(Theme::styleSheet(c, ViewZoom::pointSize(settings_.zoomPercent)));
  for (QAction* a : themeGroup_->actions()) a->setChecked(a->data().toString() == settings_.theme);
  if (action(QStringLiteral("actionZoomIn")))
    action(QStringLiteral("actionZoomIn"))->setEnabled(settings_.zoomPercent < ViewZoom::kMaxPercent);
  if (action(QStringLiteral("actionZoomOut")))
    action(QStringLiteral("actionZoomOut"))->setEnabled(settings_.zoomPercent > ViewZoom::kMinPercent);
}

void MainWindow::setTheme(const QString& choice) {
  if (!Theme::choices().contains(choice)) return;
  settings_.theme = choice;
  applyAppearance();
  saveSettings();
  if (PreferencesDialog* prefs = preferences_.data()) {
    QComboBox* box = prefs->themeBox();
    const QSignalBlocker block(box);
    box->setCurrentIndex(box->findData(choice));
  }
}

void MainWindow::setZoomPercent(int percent) {
  const int p = ViewZoom::clampPercent(percent);
  if (p == settings_.zoomPercent) return;
  settings_.zoomPercent = p;
  applyAppearance();
  saveSettings();
  if (PreferencesDialog* prefs = preferences_.data()) {
    QSpinBox* box = prefs->zoomBox();
    const QSignalBlocker block(box);
    box->setValue(p);
  }
}

void MainWindow::zoomIn() { setZoomPercent(ViewZoom::stepUp(settings_.zoomPercent)); }
void MainWindow::zoomOut() { setZoomPercent(ViewZoom::stepDown(settings_.zoomPercent)); }
void MainWindow::zoomReset() { setZoomPercent(ViewZoom::kDefaultPercent); }

void MainWindow::saveSettings() {
  if (auto s = client::saveDesktopSettings(options_.paths, settings_); !s) {
    statusBar()->showMessage(tr("Could not save preferences: %1").arg(s.error().message), 8000);
  }
}

// ---- connection ----

client::ClientConfig MainWindow::cliConfig() const { return client::loadClientConfig(options_.env, QString()); }

client::TokenStore MainWindow::tokenStore(const client::ClientConfig& cli) const {
  return client::TokenStore(cli.tokenFile, options_.runner, options_.macos);
}

QString MainWindow::cachePath() const {
  return settings_.cacheEnabled ? options_.paths.cacheFile() : QStringLiteral(":memory:");
}

bool MainWindow::connectToServer() {
  const client::ClientConfig cli = cliConfig();
  auto conn = client::resolveConnection(options_.env, settings_, cli, tokenStore(cli));
  if (!conn) {
    session_.reset();
    signInNeeded_ = true;
    updateConnectionUi();
    reloadProjects();
    return false;
  }
  signInNeeded_ = false;
  startSession(std::move(conn).value());
  return true;
}

bool MainWindow::needsSignIn() const {
  return signInNeeded_ || (session_ && session_->health() == AppSession::Health::SignInNeeded);
}

void MainWindow::startSession(client::Connection connection) {
  session_.reset();
  session_ = std::make_unique<AppSession>(std::move(connection), cachePath());
  session_->setTiming(options_.timing);
  QString error;
  if (!session_->open(&error)) {
    statusBar()->showMessage(tr("The cache could not be opened: %1").arg(error), 10000);
  } else if (!error.isEmpty()) {
    statusBar()->showMessage(tr("The cache could not be opened (%1); using memory only.").arg(error), 10000);
  }
  connect(session_.get(), &AppSession::statusChanged, this, [this] { updateConnectionUi(); });
  connect(session_.get(), &AppSession::dataChanged, this, [this] { reloadProjects(); });
  session_->setActive(isActiveWindow() || !isVisible());
  reloadProjects();
  session_->start();
  updateConnectionUi();
}

SignInDialog* MainWindow::showSignIn(const QString& reason) {
  if (SignInDialog* open = signIn_.data()) {
    open->raise();
    open->activateWindow();
    return open;
  }
  const client::ClientConfig cli = cliConfig();
  const client::TokenStore store = tokenStore(cli);
  // Offer the CLI's token by label, unless the server just refused it.
  QString existing;
  const bool refused = session_ && session_->health() == AppSession::Health::SignInNeeded;
  if (!refused) {
    if (auto t = client::resolveDeviceToken(options_.env, cli.store, store)) existing = client::tokenLabel(t->token);
  }
  QString url = settings_.serverUrl.isEmpty() ? cli.url : settings_.serverUrl;
  QString why = reason;
  if (why.isEmpty() && refused) {
    why = tr("The server refused the device token %1…. Paste a new device token "
             "(zproject-cli token create --kind device on the server).")
              .arg(session_->connection().tokenLabel);
  }
  auto* dialog = new SignInDialog(url, existing, cli.store.isEmpty() ? QStringLiteral("file") : cli.store, why, this);
  signIn_ = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::WindowModal);
  connect(dialog, &QDialog::accepted, this, [this, dialog] {
    client::ClientConfig c = cliConfig();
    if (auto s = saveSignIn(*dialog, c, tokenStore(c), options_.paths, settings_); !s) {
      statusBar()->showMessage(tr("Could not save the sign-in: %1").arg(s.error().message), 10000);
      return;
    }
    connectToServer();
    if (PreferencesDialog* prefs = preferences_.data()) prefs->setConnectionInfo(connectionInfo());
  });
  dialog->open();
  return dialog;
}

bool MainWindow::editingAllowed() const { return session_ && session_->editingAllowed(); }

void MainWindow::updateConnectionUi() {
  using H = AppSession::Health;
  const QString when = session_ ? formatWhen(session_->lastRefresh()) : QString();
  const bool haveData = session_ && session_->cache() && !session_->cache()->isEmpty();
  const QString from = haveData && !when.isEmpty() ? tr("showing data from %1").arg(when)
                                                   : tr("no saved data on this computer yet");
  QString text;
  bool showSignIn = false;
  bool showRetry = true;
  if (!session_) {
    text = signInNeeded_ ? tr("Not connected — connect this computer to your zproject server to begin.")
                         : QString();
    showSignIn = signInNeeded_;
    showRetry = false;
    status_->setText(signInNeeded_ ? tr("Not connected") : QString());
  } else {
    switch (session_->health()) {
      case H::Connecting:
      case H::Online:
        break;
      case H::Offline:
        text = tr("Offline — %1. Editing is off until the server answers; retrying automatically.").arg(from);
        break;
      case H::SignInNeeded:
        text = tr("The server refused device token %1… — %2. Editing is off.")
                   .arg(session_->connection().tokenLabel, from);
        showSignIn = true;
        break;
      case H::TlsError:
        text = tr("The server's certificate could not be verified — %1. Editing is off.").arg(from);
        break;
      case H::Unsupported:
        text = tr("%1 — %2. Editing is off.").arg(session_->compatibilityNote().isEmpty()
                                                     ? session_->lastErrorText()
                                                     : session_->compatibilityNote(),
                                                 from);
        showRetry = false;
        break;
    }
    status_->setText(session_->statusText());
    status_->setToolTip(session_->compatibilityNote());
  }
  banner_->setVisible(!text.isEmpty());
  bannerText_->setText(text);
  bannerText_->setAccessibleName(text);
  signInButton_->setVisible(showSignIn);
  retry_->setVisible(showRetry);
  refreshed_->setText(when.isEmpty() ? QString() : tr("Updated %1").arg(when));

  const bool editing = editingAllowed();
  readOnly_->setVisible(!editing);
  if (QAction* r = action(QStringLiteral("actionRefresh"))) r->setEnabled(session_ != nullptr);
  if (editing != lastEditing_) {
    lastEditing_ = editing;
    emit editingAllowedChanged(editing);
  }
  if (PreferencesDialog* prefs = preferences_.data()) prefs->setConnectionInfo(connectionInfo());
}

void MainWindow::reloadProjects() {
  const QString current = projects_->currentItem() ? projects_->currentItem()->data(Qt::UserRole).toString() : QString();
  const QSignalBlocker block(projects_);
  projects_->clear();
  if (session_ && session_->cache()) {
    for (const QJsonObject& p : session_->cache()->projects()) {
      auto* item = new QListWidgetItem(p.value(QStringLiteral("name")).toString(), projects_);
      item->setData(Qt::UserRole, p.value(QStringLiteral("id")).toString());
      if (item->data(Qt::UserRole).toString() == current) projects_->setCurrentItem(item);
    }
  }
  if (!projects_->currentItem() && projects_->count() > 0) projects_->setCurrentRow(0);
  showProject(projects_->currentItem() ? projects_->currentItem()->data(Qt::UserRole).toString() : QString());
}

void MainWindow::showProject(const QString& id) {
  std::optional<QJsonObject> p;
  if (session_ && session_->cache() && !id.isEmpty()) p = session_->cache()->project(id);
  if (!p) {
    if (!session_) {
      empty_->setText(tr("Connect to a zproject server to see your projects.\nFile → Connect to Server…"));
    } else if (projects_->count() == 0) {
      empty_->setText(session_->health() == AppSession::Health::Connecting ? tr("Loading projects…")
                                                                            : tr("No projects yet."));
    } else {
      empty_->setText(tr("Select a project."));
    }
    stack_->setCurrentIndex(0);
    inspector_->setText(tr("Nothing selected."));
    return;
  }
  const QList<QJsonObject> tasks = session_->cache()->tasks(id);
  int done = 0;
  int milestones = 0;
  for (const QJsonObject& t : tasks) {
    if (t.value(QStringLiteral("percentComplete")).toDouble() >= 100.0) ++done;
    if (t.value(QStringLiteral("type")).toString() == QLatin1String("milestone")) ++milestones;
  }
  const QString name = p->value(QStringLiteral("name")).toString();
  projectTitle_->setText(name);
  projectTitle_->setAccessibleName(name);
  const int n = static_cast<int>(tasks.size());
  projectSummary_->setText((n == 1 ? tr("1 task") : tr("%1 tasks").arg(n)) + tr(" · %1 complete").arg(done) +
                           (milestones == 1 ? tr(" · 1 milestone") : tr(" · %1 milestones").arg(milestones)) +
                           tr("\n\nThe outline, Gantt and resource views arrive in the next releases."));
  stack_->setCurrentIndex(1);
  QString html = QStringLiteral("<b>%1</b><br><br>").arg(name.toHtmlEscaped());
  auto row = [&html](const QString& k, const QString& v) {
    if (!v.isEmpty()) html += QStringLiteral("%1: %2<br>").arg(k.toHtmlEscaped(), v.toHtmlEscaped());
  };
  row(tr("Status"), p->value(QStringLiteral("status")).toString());
  row(tr("Owner"), p->value(QStringLiteral("owner")).toString());
  row(tr("Start"), p->value(QStringLiteral("start")).toString());
  row(tr("Finish"), p->value(QStringLiteral("finish")).toString());
  row(tr("Revision"), QString::number(p->value(QStringLiteral("rev")).toInteger()));
  row(tr("Id"), id);
  inspector_->setText(html);
}

// ---- cache ----

bool MainWindow::clearCache(QString* message) {
  auto say = [message](const QString& m) {
    if (message) *message = m;
  };
  if (!session_) {
    say(tr("Not connected."));
    return false;
  }
  if (!session_->editingAllowed()) {
    say(tr("Can't clear the cache while the server can't be reached: it is the only copy you can see."));
    return false;
  }
  QString error;
  if (!session_->clearCache(&error)) {
    say(tr("Could not clear the cache: %1").arg(error));
    return false;
  }
  reloadProjects();
  updateConnectionUi();
  say(error.isEmpty() ? tr("Cache cleared; reloading from the server.") : error);
  return error.isEmpty();
}

void MainWindow::setCacheEnabled(bool enabled) {
  if (settings_.cacheEnabled == enabled) return;
  settings_.cacheEnabled = enabled;
  saveSettings();
  if (!session_) return;
  client::Connection c = session_->connection();
  session_.reset();
  if (!enabled) {
    // Off means nothing on disk: the existing copy goes too.
    if (auto s = client::Cache::removeFiles(options_.paths.cacheFile()); !s) {
      statusBar()->showMessage(s.error().message, 8000);
    }
  }
  startSession(std::move(c));
}

// ---- dialogs ----

ConnectionInfo MainWindow::connectionInfo() const {
  ConnectionInfo i;
  if (session_) {
    const auto& c = session_->connection();
    i.serverUrl = c.url.toString();
    i.urlSource = c.urlSource;
    i.tokenLabel = c.tokenLabel;
    i.tokenSource = c.tokenSource;
    i.serverVersion = session_->serverVersion();
    i.status = session_->statusText();
    i.cachePath = session_->cachePath() == QLatin1String(":memory:") ? tr("off (memory only)") : session_->cachePath();
  } else {
    i.status = tr("Not connected");
  }
  return i;
}

HelpDialog* MainWindow::showHelp() {
  HelpDialog* d = help_.data();
  if (d == nullptr) {
    d = new HelpDialog(this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setOpenUrlHandler([this](const QUrl& u) { openUrl(u); });
    help_ = d;
  }
  d->show();
  d->raise();
  d->activateWindow();
  return d;
}

AboutDialog* MainWindow::showAbout() {
  AboutDialog* d = about_.data();
  if (d == nullptr) {
    d = new AboutDialog(connectionInfo(), this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setOpenUrlHandler([this](const QUrl& u) { openUrl(u); });
    about_ = d;
  }
  d->show();
  d->raise();
  d->activateWindow();
  return d;
}

ShortcutsDialog* MainWindow::showShortcuts() {
  ShortcutsDialog* d = shortcuts_.data();
  if (d == nullptr) {
    d = new ShortcutsDialog(this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    shortcuts_ = d;
  }
  d->show();
  d->raise();
  d->activateWindow();
  return d;
}

PreferencesDialog* MainWindow::showPreferences() {
  PreferencesDialog* d = preferences_.data();
  if (d == nullptr) {
    d = new PreferencesDialog(connectionInfo(), settings_.theme, settings_.zoomPercent, settings_.cacheEnabled,
                              settings_.updateCheckDaily, this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    connect(d, &PreferencesDialog::themeChosen, this, &MainWindow::setTheme);
    connect(d, &PreferencesDialog::zoomChosen, this, &MainWindow::setZoomPercent);
    connect(d, &PreferencesDialog::cacheEnabledChosen, this, &MainWindow::setCacheEnabled);
    connect(d, &PreferencesDialog::clearCacheRequested, this, [this] {
      QString m;
      clearCache(&m);
      if (PreferencesDialog* prefs = preferences_.data()) prefs->setClearResult(m);
    });
    connect(d, &PreferencesDialog::signInRequested, this, [this] { showSignIn(); });
    connect(d, &PreferencesDialog::updateCheckChosen, this, &MainWindow::setUpdateCheckDaily);
    connect(d, &PreferencesDialog::checkForUpdatesRequested, this, [this] { checkForUpdates(); });
    preferences_ = d;
  }
  d->show();
  d->raise();
  d->activateWindow();
  return d;
}

void MainWindow::openUrl(const QUrl& url) { openHttpsUrl(url, openUrl_); }

// ---- updates ----

QString MainWindow::updateStatePath() const {
  return QFileInfo(options_.paths.settingsFile).absolutePath() + QStringLiteral("/update-state.ini");
}

UpdateDialog* MainWindow::checkForUpdates() { return updater_->checkNow(); }

void MainWindow::setUpdateCheckDaily(bool daily) {
  settings_.updateCheckDaily = daily;
  settings_.updateAsked = true;
  saveSettings();
}

QMessageBox* MainWindow::startUpdates() {
  if (settings_.updateAsked) {
    updater_->startupCheckIfDue(settings_.updateCheckDaily);
    return nullptr;
  }
  // Ask once (SPEC.md §21, plan Q12): default on.
  auto* box = new QMessageBox(QMessageBox::Question, tr("Software Updates"),
                              tr("Should zproject check GitHub once a day for a new version?\n\n"
                                 "Updates are signed and verified, and nothing is installed without your OK. "
                                 "You can change this in Preferences."),
                              QMessageBox::NoButton, this);
  box->setObjectName(QStringLiteral("updateQuestion"));
  box->setAttribute(Qt::WA_DeleteOnClose);
  QPushButton* yes = box->addButton(tr("Check Daily"), QMessageBox::YesRole);
  yes->setObjectName(QStringLiteral("updateQuestionYes"));
  QPushButton* no = box->addButton(tr("Don't Check"), QMessageBox::NoRole);
  no->setObjectName(QStringLiteral("updateQuestionNo"));
  box->setDefaultButton(yes);
  connect(box, &QMessageBox::buttonClicked, this, [this, yes](QAbstractButton* b) {
    setUpdateCheckDaily(b == yes);
    if (PreferencesDialog* p = preferences_.data()) {
      const QSignalBlocker block(p->updatesBox());
      p->updatesBox()->setChecked(settings_.updateCheckDaily);
    }
    updater_->startupCheckIfDue(settings_.updateCheckDaily);
  });
  box->open();
  return box;
}

// ---- events ----

void MainWindow::changeEvent(QEvent* event) {
  if (event->type() == QEvent::ActivationChange && session_) session_->setActive(isActiveWindow());
  QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (session_ && session_->syncer()) session_->syncer()->stop();
  QMainWindow::closeEvent(event);
}

}  // namespace zproject::app
