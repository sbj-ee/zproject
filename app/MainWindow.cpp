#include "app/MainWindow.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QPainter>
#include <QPixmap>
#include <QTableWidget>
#include <QToolButton>
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
#include <QTreeView>
#include <QVBoxLayout>

#include "app/EditDialogs.hpp"
#include "app/HelpDialog.hpp"
#include "app/Icons.hpp"
#include "app/OutlineModel.hpp"
#include "app/OutlineView.hpp"
#include "app/ProjectActions.hpp"
#include "app/Shortcuts.hpp"
#include "app/SignInDialog.hpp"
#include "app/TaskActions.hpp"
#include "app/TaskInspector.hpp"
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

// Sidebar order (SPEC.md §8.1): active, on hold, then done and cancelled.
int statusRank(const QString& s) {
  if (s == QLatin1String("active")) return 0;
  if (s == QLatin1String("on_hold")) return 1;
  if (s == QLatin1String("done")) return 2;
  if (s == QLatin1String("cancelled")) return 3;
  return 4;
}
bool isClosed(const QString& s) { return statusRank(s) >= 2 && statusRank(s) <= 3; }

QIcon ragIcon(const QString& rag) {
  QColor c(0x9a, 0x9a, 0x9a);
  if (rag == QLatin1String("green")) c = QColor(0x2e, 0x9d, 0x4f);
  if (rag == QLatin1String("amber")) c = QColor(0xe0, 0x9b, 0x1a);
  if (rag == QLatin1String("red")) c = QColor(0xd2, 0x3b, 0x3b);
  QPixmap px(12, 12);
  px.fill(Qt::transparent);
  QPainter painter(&px);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(c);
  painter.setPen(Qt::NoPen);
  painter.drawEllipse(1, 1, 10, 10);
  return QIcon(px);
}

QString ragName(const QString& rag) {
  if (rag == QLatin1String("green")) return QObject::tr("Green");
  if (rag == QLatin1String("amber")) return QObject::tr("Amber");
  if (rag == QLatin1String("red")) return QObject::tr("Red");
  return rag;
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
  projectActions_ = new ProjectActions(this, [this] { return session_.get(); }, this);
  connect(projectActions_, &ProjectActions::notice, this, [this](const QString& text, bool) {
    statusBar()->showMessage(text, 10000);
  });
  connect(projectActions_, &ProjectActions::saved, this, [this](const QString& id, bool created) {
    if (created) pendingSelect_ = id;
  });
  buildCentral();
  taskActions_ = new TaskActions(this, [this] { return session_.get(); }, outline_->model(), this);
  connect(taskActions_, &TaskActions::notice, this, [this](const QString& text, bool error) {
    statusBar()->showMessage(text, 10000);
    outline_->showMessage(text, error);
    updateEditActions();
  });
  connect(taskActions_, &TaskActions::changed, this,
          [this](const QString& id, bool editTitle) { outline_->setPending(id, editTitle); });
  connect(taskActions_, &TaskActions::deletedChanged, this, [this] { updateEditActions(); });
  connect(outline_->model(), &OutlineModel::editRequested, taskActions_, &TaskActions::edit);
  connect(outline_, &OutlineView::currentTaskChanged, this, [this](const QString& id) {
    showTaskInspector(id);
    updateEditActions();
  });
  buildDocks();
  connect(taskInspector_, &TaskInspector::saveRequested, taskActions_, &TaskActions::saveTask);
  connect(taskInspector_, &TaskInspector::dirtyChanged, this, [this] { updateEditActions(); });
  connect(taskActions_, &TaskActions::taskWritten, this, [this](const QString& id) {
    if (id == taskInspector_->taskId()) taskInspector_->reloadFromServer();
  });
  connect(taskActions_, &TaskActions::notice, this, [this](const QString& text, bool error) {
    // The editor says what happened to its own save.
    if (taskActions_->currentTask() == taskInspector_->taskId() && inspectorStack_->currentWidget() == taskInspector_) {
      taskInspector_->setMessage(text, error);
    }
  });
  connect(taskActions_, &TaskActions::fieldError, this, [this](const QString& id, const QString& field, const QString& m) {
    if (id == taskInspector_->taskId()) taskInspector_->showFieldError(field, m);
  });
  connect(taskActions_, &TaskActions::busyChanged, this, [this](bool busy) {
    taskInspector_->setBusy(busy);
    updateEditActions();
  });
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
  detailsPage_ = page;
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
  auto* buttons = new QHBoxLayout;
  editButton_ = new QPushButton(tr("Edit Details…"), page);
  editButton_->setObjectName(QStringLiteral("editProjectButton"));
  connect(editButton_, &QPushButton::clicked, this, [this] { projectActions_->editProject(currentProjectId()); });
  buttons->addWidget(editButton_);
  deleteButton_ = new QPushButton(tr("Delete Project…"), page);
  deleteButton_->setObjectName(QStringLiteral("deleteProjectButton"));
  connect(deleteButton_, &QPushButton::clicked, this, [this] { projectActions_->deleteProject(currentProjectId()); });
  buttons->addWidget(deleteButton_);
  outlineButton_ = new QPushButton(tr("Open Outline"), page);
  outlineButton_->setObjectName(QStringLiteral("openOutlineButton"));
  connect(outlineButton_, &QPushButton::clicked, this, [this] { showOutline(); });
  buttons->addWidget(outlineButton_);
  buttons->addStretch(1);
  pv->addLayout(buttons);
  projectDetails_ = new QLabel(page);
  projectDetails_->setObjectName(QStringLiteral("projectDetails"));
  projectDetails_->setAccessibleName(tr("Project details"));
  projectDetails_->setWordWrap(true);
  projectDetails_->setTextFormat(Qt::RichText);
  projectDetails_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  projectDetails_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  pv->addWidget(projectDetails_, 1);
  stack_->addWidget(page);

  projectTable_ = new QTableWidget(0, 7, stack_);
  projectTable_->setObjectName(QStringLiteral("projectTable"));
  projectTable_->setAccessibleName(tr("All projects"));
  projectTable_->setHorizontalHeaderLabels(
      {tr("Name"), tr("Status"), tr("RAG"), tr("Priority"), tr("Start"), tr("Target finish"), tr("Owner")});
  projectTable_->verticalHeader()->hide();
  projectTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  projectTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
  projectTable_->setSelectionMode(QAbstractItemView::SingleSelection);
  projectTable_->horizontalHeader()->setStretchLastSection(true);
  projectTable_->setShowGrid(false);
  connect(projectTable_, &QTableWidget::cellActivated, this, [this](int row, int) {
    if (QTableWidgetItem* it = projectTable_->item(row, 0)) {
      selectProject(it->data(Qt::UserRole).toString());
      showProjectDetails();
    }
  });
  stack_->addWidget(projectTable_);
  outline_ = new OutlineView(stack_);
  stack_->addWidget(outline_);
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
  projects_->setIconSize(QSize(16, 16));
  connect(projects_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
    showProject(item ? item->data(Qt::UserRole).toString() : QString());
  });
  auto* side = new QWidget(sidebarDock_);
  auto* sv = new QVBoxLayout(side);
  sv->setContentsMargins(0, 0, 0, 0);
  sv->setSpacing(0);
  sv->addWidget(projects_, 1);
  showClosed_ = new QToolButton(side);
  showClosed_->setObjectName(QStringLiteral("showClosedProjects"));
  showClosed_->setCheckable(true);
  showClosed_->setAutoRaise(true);
  showClosed_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  showClosed_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect(showClosed_, &QToolButton::toggled, this, [this] { reloadProjects(); });
  sv->addWidget(showClosed_);
  sidebarDock_->setWidget(side);
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
  // The project (or nothing) as text, or the selected task's editor.
  inspectorStack_ = new QStackedWidget(inspectorDock_);
  inspectorStack_->setObjectName(QStringLiteral("inspectorStack"));
  inspectorStack_->addWidget(inspector_);
  taskInspector_ = new TaskInspector([this] { return session_.get(); }, inspectorStack_);
  inspectorStack_->addWidget(taskInspector_);
  inspectorDock_->setWidget(inspectorStack_);
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
  add(file, QStringLiteral("actionNewProject"), tr("&New Project…"), [this] { projectActions_->newProject(); });
  file->addSeparator();
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

  // Edit: the outline's task actions (SPEC.md §8.1, §8.4). The structural
  // keys belong to the task tree only, so Tab, Enter and Del keep their
  // usual meaning everywhere else.
  QMenu* edit = menuBar()->addMenu(tr("&Edit"));
  auto current = [this] { return outline_->currentTaskId(); };
  add(edit, QStringLiteral("actionInsertTask"), tr("&Insert Task"), [this] {
    // Read the current row first: opening the outline focuses the tree, which
    // picks a current row of its own.
    const QString below = page_ == ProjectPage::Outline ? outline_->currentTaskId() : QString();
    showOutline();
    taskActions_->insertBelow(currentProjectId(), below);
  });
  add(edit, QStringLiteral("actionIndentTask"), tr("In&dent"), [this, current] { taskActions_->indent(current()); });
  add(edit, QStringLiteral("actionOutdentTask"), tr("&Outdent"), [this, current] { taskActions_->outdent(current()); });
  add(edit, QStringLiteral("actionMoveTaskUp"), tr("Move &Up"), [this, current] { taskActions_->moveUp(current()); });
  add(edit, QStringLiteral("actionMoveTaskDown"), tr("Move Do&wn"), [this, current] { taskActions_->moveDown(current()); });
  edit->addSeparator();
  add(edit, QStringLiteral("actionRenameTask"), tr("&Rename Task"), [this, current] { outline_->editTitle(current()); });
  add(edit, QStringLiteral("actionEditTask"), tr("Task D&etails"), [this] {
    // The inspector's editor for the selected task (plan §3.3).
    inspectorDock_->show();
    if (inspectorStack_->currentWidget() != taskInspector_) return;
    taskInspector_->tabs()->setCurrentIndex(0);
    if (QWidget* t = taskInspector_->field(QStringLiteral("title"))) t->setFocus();
  });
  add(edit, QStringLiteral("actionSaveTask"), tr("&Save Task"), [this] { taskInspector_->save(); });
  add(edit, QStringLiteral("actionToggleMilestone"), tr("Convert to &Milestone"),
      [this, current] { taskActions_->toggleMilestone(current()); });
  edit->addSeparator();
  add(edit, QStringLiteral("actionDeleteTask"), tr("Delete &Task…"), [this, current] { taskActions_->deleteTask(current()); });
  add(edit, QStringLiteral("actionRestoreTask"), tr("Re&store Deleted Task…"), [this] { taskActions_->restoreLast(); });
  edit->addSeparator();
  add(edit, QStringLiteral("actionFind"), tr("&Find…"), [this] {
    if (currentProjectId().isEmpty()) return;
    showOutline();
    outline_->showFind();
  });
  add(edit, QStringLiteral("actionFindNext"), tr("Find &Next"), [this] { outline_->findNext(false); });
  add(edit, QStringLiteral("actionFindPrevious"), tr("Find Pre&vious"), [this] { outline_->findNext(true); });
  for (const char* name : {"actionInsertTask", "actionIndentTask", "actionOutdentTask", "actionMoveTaskUp",
                           "actionMoveTaskDown", "actionRenameTask", "actionDeleteTask", "actionEditTask"}) {
    QAction* a = action(QString::fromLatin1(name));
    a->setShortcutContext(Qt::WidgetShortcut);
    outline_->tree()->addAction(a);
  }

  QMenu* project = menuBar()->addMenu(tr("&Project"));
  add(project, QStringLiteral("actionEditProject"), tr("&Edit Project Details…"),
      [this] { projectActions_->editProject(currentProjectId()); });
  add(project, QStringLiteral("actionDeleteProject"), tr("&Delete Project…"),
      [this] { projectActions_->deleteProject(currentProjectId()); });

  QMenu* view = menuBar()->addMenu(tr("&View"));
  add(view, QStringLiteral("actionAllProjects"), tr("All &Projects"), [this] { showProjectTable(); });
  add(view, QStringLiteral("actionProjectDetails"), tr("Project &Details"), [this] { showProjectDetails(); });
  add(view, QStringLiteral("actionOutline"), tr("&Outline"), [this] { showOutline(); });
  view->addSeparator();
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
  // The dock, not this: the signal also fires while ~QWidget runs.
  connect(sidebarDock_, &QDockWidget::visibilityChanged, side, [dock = sidebarDock_, side](bool) {
    side->setChecked(!dock->isHidden());
  });
  auto* insp = add(view, QStringLiteral("actionToggleInspector"), tr("&Inspector"),
                   [this](bool on) { inspectorDock_->setVisible(on); });
  insp->setCheckable(true);
  insp->setChecked(true);
  // The dock, not this: the signal also fires while ~QWidget runs.
  connect(inspectorDock_, &QDockWidget::visibilityChanged, insp, [dock = inspectorDock_, insp](bool) {
    insp->setChecked(!dock->isHidden());
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
  applyIcons(c);
  if (action(QStringLiteral("actionZoomIn")))
    action(QStringLiteral("actionZoomIn"))->setEnabled(settings_.zoomPercent < ViewZoom::kMaxPercent);
  if (action(QStringLiteral("actionZoomOut")))
    action(QStringLiteral("actionZoomOut"))->setEnabled(settings_.zoomPercent > ViewZoom::kMinPercent);
}

// The app's own icons (app/Icons.hpp), redrawn in the theme's colours.
void MainWindow::applyIcons(const ThemeColors& c) {
  using G = Icons::Glyph;
  const std::pair<const char*, G> glyphs[] = {
      {"actionNewProject", G::NewProject},     {"actionRefresh", G::Refresh},
      {"actionEditProject", G::EditProject},   {"actionDeleteProject", G::DeleteProject},
      {"actionAllProjects", G::AllProjects},   {"actionProjectDetails", G::ProjectDetails},
      {"actionOutline", G::Outline},           {"actionInsertTask", G::InsertTask},
      {"actionEditTask", G::TaskDetails},      {"actionSaveTask", G::SaveTask},
      {"actionDeleteTask", G::DeleteTask},     {"actionFind", G::Find},
  };
  for (const auto& [name, g] : glyphs) {
    if (QAction* a = action(QString::fromLatin1(name))) a->setIcon(Icons::glyph(g, c));
  }
  if (outlineButton_) outlineButton_->setIcon(Icons::glyph(G::Outline, c));
  if (projects_) {
    for (int i = 0; i < projects_->count(); ++i) {
      QListWidgetItem* item = projects_->item(i);
      item->setIcon(Icons::project(c, item->data(Qt::UserRole + 2).toString(), item->data(Qt::UserRole + 1).toString()));
    }
  }
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
  updateEditActions();
  if (editing != lastEditing_) {
    lastEditing_ = editing;
    emit editingAllowedChanged(editing);
  }
  if (PreferencesDialog* prefs = preferences_.data()) prefs->setConnectionInfo(connectionInfo());
}

QString MainWindow::currentProjectId() const {
  return projects_->currentItem() ? projects_->currentItem()->data(Qt::UserRole).toString() : QString();
}

void MainWindow::selectProject(const QString& id) {
  for (int i = 0; i < projects_->count(); ++i) {
    QListWidgetItem* item = projects_->item(i);
    if (item->data(Qt::UserRole).toString() != id) continue;
    if (item->isHidden()) {
      showClosed_->setChecked(true);  // reloads; the item is visible now
      selectProject(id);
      return;
    }
    projects_->setCurrentItem(item);
    return;
  }
}

void MainWindow::showProjectTable() {
  stack_->setCurrentWidget(projectTable_);
  projectTable_->setFocus(Qt::ShortcutFocusReason);
  updateEditActions();
}

void MainWindow::showProjectDetails() {
  page_ = ProjectPage::Details;
  showProject(currentProjectId());
}

void MainWindow::showOutline() {
  if (currentProjectId().isEmpty()) return;
  const bool was = stack_->currentWidget() == outline_;
  page_ = ProjectPage::Outline;
  showProject(currentProjectId());
  if (!was) outline_->tree()->setFocus(Qt::OtherFocusReason);
}

void MainWindow::reloadProjects() {
  QString current = currentProjectId();
  if (!pendingSelect_.isEmpty()) current = pendingSelect_;
  const bool tableShown = stack_->currentWidget() == projectTable_;
  QList<QJsonObject> list;
  if (session_ && session_->cache()) list = session_->cache()->projects();
  std::stable_sort(list.begin(), list.end(), [](const QJsonObject& a, const QJsonObject& b) {
    return statusRank(a.value(QStringLiteral("status")).toString()) < statusRank(b.value(QStringLiteral("status")).toString());
  });
  int closed = 0;
  for (const QJsonObject& p : list) {
    if (isClosed(p.value(QStringLiteral("status")).toString())) ++closed;
  }
  {
    const QSignalBlocker block(projects_);
    projects_->clear();
    for (const QJsonObject& p : list) {
      const QString status = p.value(QStringLiteral("status")).toString();
      auto* item = new QListWidgetItem(p.value(QStringLiteral("name")).toString(), projects_);
      const QString id = p.value(QStringLiteral("id")).toString();
      item->setData(Qt::UserRole, id);
      item->setData(Qt::UserRole + 1, status);
      item->setData(Qt::UserRole + 2, p.value(QStringLiteral("rag")).toString());
      item->setIcon(Icons::project(Theme::colors(resolvedTheme_), item->data(Qt::UserRole + 2).toString(), status));
      item->setToolTip(projectStatusName(status));
      if (status != QLatin1String("active")) {
        QFont f = item->font();
        f.setItalic(true);
        item->setFont(f);
        item->setText(item->text() + QStringLiteral(" (%1)").arg(projectStatusName(status).toLower()));
        item->setData(Qt::AccessibleTextRole, item->text());
      }
      item->setHidden(isClosed(status) && !showClosed_->isChecked());
      if (id == current && !item->isHidden()) projects_->setCurrentItem(item);
      if (id == pendingSelect_) pendingSelect_.clear();
    }
    if (!projects_->currentItem()) {
      for (int i = 0; i < projects_->count(); ++i) {
        if (!projects_->item(i)->isHidden()) {
          projects_->setCurrentRow(i);
          break;
        }
      }
    }
  }
  showClosed_->setVisible(closed > 0);
  showClosed_->setText(showClosed_->isChecked() ? tr("Hide done and cancelled (%1)").arg(closed)
                                                : tr("Show done and cancelled (%1)").arg(closed));
  showClosed_->setAccessibleName(showClosed_->text());
  fillProjectTable(list);
  showProject(currentProjectId());
  if (tableShown) stack_->setCurrentWidget(projectTable_);
  updateEditActions();
}

void MainWindow::fillProjectTable(const QList<QJsonObject>& projects) {
  const QString selected = projectTable_->currentRow() >= 0 && projectTable_->item(projectTable_->currentRow(), 0)
                               ? projectTable_->item(projectTable_->currentRow(), 0)->data(Qt::UserRole).toString()
                               : currentProjectId();
  projectTable_->setRowCount(0);
  for (const QJsonObject& p : projects) {
    const int row = projectTable_->rowCount();
    projectTable_->insertRow(row);
    auto put = [this, row](int col, const QString& text, const QString& tip = {}) {
      auto* item = new QTableWidgetItem(text);
      item->setToolTip(tip.isEmpty() ? text : tip);
      projectTable_->setItem(row, col, item);
      return item;
    };
    const QString rag = p.value(QStringLiteral("rag")).toString();
    QTableWidgetItem* name = put(0, p.value(QStringLiteral("name")).toString());
    name->setData(Qt::UserRole, p.value(QStringLiteral("id")).toString());
    put(1, projectStatusName(p.value(QStringLiteral("status")).toString()));
    QTableWidgetItem* r = put(2, ragLetter(rag), ragName(rag));
    r->setIcon(ragIcon(rag));
    r->setData(Qt::AccessibleTextRole, tr("RAG %1").arg(ragName(rag)));
    put(3, QStringLiteral("P%1").arg(p.value(QStringLiteral("priority")).toInt(2)),
        tr("Priority %1 (1 = highest)").arg(p.value(QStringLiteral("priority")).toInt(2)));
    put(4, p.value(QStringLiteral("start")).toString());
    put(5, p.value(QStringLiteral("finish")).toString());
    put(6, p.value(QStringLiteral("owner")).toString());
    if (name->data(Qt::UserRole).toString() == selected) projectTable_->setCurrentCell(row, 0);
  }
  projectTable_->resizeColumnsToContents();
}

void MainWindow::updateEditActions() {
  const bool editing = editingAllowed();
  const bool haveProject = !currentProjectId().isEmpty();
  auto enable = [this](const char* name, bool on) {
    if (QAction* a = action(QString::fromLatin1(name))) a->setEnabled(on);
  };
  enable("actionNewProject", editing);
  enable("actionEditProject", editing && haveProject);
  enable("actionDeleteProject", editing && haveProject);
  enable("actionProjectDetails", haveProject);
  enable("actionOutline", haveProject);
  if (outlineButton_) outlineButton_->setEnabled(haveProject);
  const bool inOutline = stack_ && stack_->currentWidget() == outline_ && haveProject;
  const QString task = inOutline ? outline_->currentTaskId() : QString();
  const bool canEdit = editing && inOutline && taskActions_ != nullptr && !taskActions_->busy();
  const bool haveTask = !task.isEmpty();
  if (outline_) outline_->setEditable(editing);
  enable("actionInsertTask", editing && haveProject);
  enable("actionIndentTask", canEdit && haveTask && taskActions_->canIndent(task));
  enable("actionOutdentTask", canEdit && haveTask && taskActions_->canOutdent(task));
  enable("actionMoveTaskUp", canEdit && haveTask && taskActions_->canMoveUp(task));
  enable("actionMoveTaskDown", canEdit && haveTask && taskActions_->canMoveDown(task));
  enable("actionRenameTask", canEdit && haveTask);
  enable("actionEditTask", inOutline && haveTask);
  enable("actionSaveTask", taskInspector_ != nullptr && taskInspector_->canSave());
  if (taskInspector_) taskInspector_->setEditable(editing);
  enable("actionToggleMilestone", canEdit && haveTask && taskActions_->canToggleMilestone(task));
  if (QAction* m = action(QStringLiteral("actionToggleMilestone"))) {
    m->setText(haveTask && outline_->model()->isMilestone(task) ? tr("Convert to &Task") : tr("Convert to &Milestone"));
  }
  enable("actionDeleteTask", canEdit && haveTask);
  const bool canRestore = editing && taskActions_ != nullptr && !taskActions_->deleted().isEmpty();
  enable("actionRestoreTask", canRestore);
  if (QAction* r = action(QStringLiteral("actionRestoreTask"))) {
    r->setText(canRestore ? tr("Re&store “%1”…").arg(taskActions_->deleted().last().label)
                          : tr("Re&store Deleted Task…"));
  }
  enable("actionFind", haveProject);
  enable("actionFindNext", inOutline);
  enable("actionFindPrevious", inOutline);
  if (editButton_) editButton_->setEnabled(editing && haveProject);
  if (deleteButton_) deleteButton_->setEnabled(editing && haveProject);
  const QString why = editing ? QString() : tr("Editing is off until the server answers");
  if (editButton_) editButton_->setToolTip(why);
  if (deleteButton_) deleteButton_->setToolTip(why);
}

void MainWindow::showProject(const QString& id) {
  std::optional<QJsonObject> p;
  if (session_ && session_->cache() && !id.isEmpty()) p = session_->cache()->project(id);
  if (!p) {
    if (!session_) {
      empty_->setText(tr("Connect to a zproject server to see your projects.\nFile → Connect to Server…"));
    } else if (projects_->count() == 0) {
      empty_->setText(session_->health() == AppSession::Health::Connecting
                          ? tr("Loading projects…")
                          : tr("No projects yet.\nFile → New Project… (%1) creates one.")
                                .arg(shortcutFor(QStringLiteral("actionNewProject")).toString(QKeySequence::NativeText)));
    } else {
      empty_->setText(tr("Select a project."));
    }
    stack_->setCurrentIndex(0);
    showInspectorText(tr("Nothing selected."));
    updateEditActions();
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
                           tr("\nOpen Outline shows its tasks; the Gantt and resource views arrive in later releases."));
  auto get = [&p](const char* k) { return p->value(QLatin1String(k)).toString(); };
  const QString rag = get("rag");
  QString html = QStringLiteral("<table cellspacing='0' cellpadding='3'>");
  auto row = [&html](const QString& k, const QString& v) {
    html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
                .arg(k.toHtmlEscaped(), v.isEmpty() ? QStringLiteral("—") : v.toHtmlEscaped());
  };
  row(tr("Status"), projectStatusName(get("status")));
  row(tr("RAG"), ragName(rag).isEmpty() ? QString() : QStringLiteral("%1 · %2").arg(ragLetter(rag), ragName(rag)));
  row(tr("Priority"), QStringLiteral("P%1").arg(p->value(QStringLiteral("priority")).toInt(2)));
  row(tr("Start"), get("start"));
  row(tr("Target finish"), get("finish"));
  row(tr("Owner"), get("owner"));
  row(tr("Sponsor"), get("sponsor"));
  const QJsonValue budget = p->value(QStringLiteral("budget"));
  row(tr("Budget"), budget.isDouble() ? QLocale().toString(budget.toDouble(), 'f', 2) : QString());
  html += QStringLiteral("</table>");
  for (const char* k : {"description", "objectives", "scope", "outOfScope", "successCriteria"}) {
    const QString text = get(k).trimmed();
    if (text.isEmpty()) continue;
    html += QStringLiteral("<h4>%1</h4><p>%2</p>")
                .arg(fieldLabel(QString::fromLatin1(k)).toHtmlEscaped(),
                     text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
  }
  projectDetails_->setText(html);
  if (page_ == ProjectPage::Outline) {
    outline_->setProject(id, name);
    outline_->reload(tasks, session_->cache()->wbs(id));
    stack_->setCurrentWidget(outline_);
  } else {
    stack_->setCurrentWidget(detailsPage_);
  }
  QString side = QStringLiteral("<b>%1</b><br><br>").arg(name.toHtmlEscaped());
  auto line = [&side](const QString& k, const QString& v) {
    if (!v.isEmpty()) side += QStringLiteral("%1: %2<br>").arg(k.toHtmlEscaped(), v.toHtmlEscaped());
  };
  line(tr("Status"), projectStatusName(get("status")));
  line(tr("Owner"), get("owner"));
  line(tr("Start"), get("start"));
  line(tr("Finish"), get("finish"));
  // Only a snapshot carries the rev; rows built from the change feed don't.
  if (const qint64 rev = p->value(QStringLiteral("rev")).toInteger(); rev > 0) line(tr("Revision"), QString::number(rev));
  line(tr("Id"), id);
  if (page_ == ProjectPage::Outline && !outline_->currentTaskId().isEmpty()) {
    inspector_->setText(side);
    showTaskInspector(outline_->currentTaskId());
  } else {
    showInspectorText(side);
  }
  updateEditActions();
}

void MainWindow::showTaskInspector(const QString& taskId) {
  if (stack_->currentWidget() != outline_ || taskId.isEmpty()) return;
  if (taskInspector_->isDirty() && taskId != taskInspector_->taskId()) {
    askUnsaved([this] { showTaskInspector(outline_->currentTaskId()); });
    return;
  }
  const OutlineModel* m = outline_->model();
  const QJsonObject t = m->task(taskId);
  if (t.isEmpty()) return;
  taskInspector_->setEditable(editingAllowed());
  taskInspector_->showTask(taskId, t, m->wbsCode(taskId), m->liveChildren(taskId));
  inspectorStack_->setCurrentWidget(taskInspector_);
  updateEditActions();
}

void MainWindow::showInspectorText(const QString& html) {
  inspector_->setText(html);
  if (inspectorStack_->currentWidget() == taskInspector_ && taskInspector_->isDirty()) {
    // Leaving a task with unsaved edits (another project, or Details).
    askUnsaved([this] { showProject(currentProjectId()); });
    return;
  }
  inspectorStack_->setCurrentWidget(inspector_);
}

void MainWindow::askUnsaved(std::function<void()> then) {
  afterUnsaved_ = std::move(then);
  if (unsavedBox_) return;  // already asking; the newest `then` wins
  auto* box = new QMessageBox(QMessageBox::Question, tr("Unsaved Changes"),
                              tr("Save your changes to %1?").arg(taskInspector_->taskLabel()),
                              QMessageBox::Save | QMessageBox::Discard, this);
  box->setObjectName(QStringLiteral("unsavedTaskBox"));
  box->setInformativeText(tr("Discard puts the task back as the server has it."));
  box->setDefaultButton(QMessageBox::Save);
  box->setAttribute(Qt::WA_DeleteOnClose);
  box->setWindowModality(Qt::WindowModal);
  unsavedBox_ = box;
  connect(box, &QMessageBox::finished, this, [this, box] {
    const bool save = box->clickedButton() == box->button(QMessageBox::Save);
    std::function<void()> next = std::move(afterUnsaved_);
    afterUnsaved_ = nullptr;
    if (save && !taskInspector_->save()) {
      // Its checks failed (or it can't be saved now): stay on the task.
      outline_->selectTask(taskInspector_->taskId());
      inspectorStack_->setCurrentWidget(taskInspector_);
      return;
    }
    // Saved (the write is on its way) or discarded: show the server's copy,
    // then go where the user was going.
    taskInspector_->revert();
    if (next) next();
  });
  box->open();
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
  if (!closeAnyway_ && taskInspector_ != nullptr && taskInspector_->isDirty()) {
    // Never lose typed edits silently.
    event->ignore();
    auto* box = new QMessageBox(QMessageBox::Question, tr("Unsaved Changes"),
                                tr("Quit without saving your changes to %1?").arg(taskInspector_->taskLabel()),
                                QMessageBox::Discard | QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("quitUnsavedBox"));
    box->setDefaultButton(QMessageBox::Cancel);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    connect(box, &QMessageBox::finished, this, [this, box] {
      if (box->clickedButton() != box->button(QMessageBox::Discard)) return;
      closeAnyway_ = true;
      QTimer::singleShot(0, this, [this] { close(); });
    });
    box->open();
    return;
  }
  if (session_ && session_->syncer()) session_->syncer()->stop();
  QMainWindow::closeEvent(event);
}

}  // namespace zproject::app
