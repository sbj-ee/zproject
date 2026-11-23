// The accessibility pass (plan PR 11; SPEC.md §18), offscreen, against the
// real zprojectd API on loopback with an in-memory store. On every view of
// the main window (details, all projects, outline, Gantt, milestones,
// activity) and in the task editor:
//  - every control Tab can reach has a name a screen reader can read;
//  - every menu item has text, every toolbar button an accessible name;
//  - every shortcut in Help → Keyboard Shortcuts belongs to a real action,
//    and no two actions share a key.
#include <QAccessible>
#include <QApplication>
#include <QListWidget>
#include <QMenuBar>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

#include "app/MainWindow.hpp"
#include "app/OutlineView.hpp"
#include "app/Shortcuts.hpp"
#include "app/TaskInspector.hpp"
#include "client/TokenStore.hpp"
#include "server/Api.hpp"
#include "server/http/HttpServer.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::app;
using Health = AppSession::Health;

namespace {

const store::WriteContext kOther{QStringLiteral("robin01"), QStringLiteral("req-other")};

client::ChangeFeedSyncer::Timing fast() {
  client::ChangeFeedSyncer::Timing t;
  t.activeMs = 60'000;
  t.inactiveMs = 120'000;
  t.listPollMs = 90'000;
  t.backoffMinMs = 50;
  t.backoffMaxMs = 200;
  return t;
}

QString describeWidget(const QWidget* w) {
  return QStringLiteral("%1 \"%2\" (parent %3)")
      .arg(QString::fromLatin1(w->metaObject()->className()), w->objectName(),
           w->parentWidget() ? w->parentWidget()->objectName() : QStringLiteral("-"));
}

// Controls Tab reaches that a screen reader would announce without a name.
QStringList unnamed(QWidget* root) {
  QStringList out;
  for (QWidget* w : root->findChildren<QWidget*>()) {
    if (!w->isVisibleTo(root) || !w->isEnabled()) continue;
    if (!(w->focusPolicy() & Qt::TabFocus)) continue;
    // A focus proxy's target is the control that gets announced.
    if (w->focusProxy() != nullptr) continue;
    QAccessibleInterface* a = QAccessible::queryAccessibleInterface(w);
    if (a == nullptr || a->text(QAccessible::Name).trimmed().isEmpty()) out << describeWidget(w);
  }
  return out;
}

}  // namespace

class TestAccessibility : public QObject {
  Q_OBJECT

  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<store::SqliteStore> store_;
  std::unique_ptr<server::Api> api_;
  std::unique_ptr<server::http::Server> http_;
  QString owner_;
  QString url_;
  QString kitchen_;
  QString survey_;

  QHash<QString, QString> env() const {
    return {{QStringLiteral("HOME"), dir_->path()},
            {QStringLiteral("ZPROJECT_CONFIG_DIR"), dir_->filePath(QStringLiteral("config"))},
            {QStringLiteral("ZPROJECT_CACHE_DIR"), dir_->filePath(QStringLiteral("cache"))}};
  }

 private slots:
  void initTestCase() {
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  }

  void init() {
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());
    store_ = std::move(store::SqliteStore::open(QStringLiteral(":memory:"))).value();
    http_ = std::make_unique<server::http::Server>([this](const server::http::Request& r) { return api_->handle(r); });
    QVERIFY(http_->listen(QHostAddress::LocalHost, 0));
    server::ApiOptions o;
    o.allowedHosts = {QStringLiteral("127.0.0.1:%1").arg(http_->port())};
    api_ = std::make_unique<server::Api>(*store_, o);
    owner_ = api_->createOwnerToken(QStringLiteral("owner")).value();
    url_ = QStringLiteral("http://127.0.0.1:%1").arg(http_->port());
    Project p;
    p.name = QStringLiteral("Kitchen remodel");
    kitchen_ = store_->createProject(p, kOther).value().id;
    Task t;
    t.project = kitchen_;
    t.title = QStringLiteral("Survey");
    t.mode = QStringLiteral("manual");
    t.start = QStringLiteral("2026-09-07");
    t.finish = QStringLiteral("2026-09-08");
    t.duration = 2;
    survey_ = store_->createTask(t, kOther).value().id;
    t.title = QStringLiteral("Handover");
    t.type = QStringLiteral("milestone");
    t.duration = 0;
    t.start = t.finish = QStringLiteral("2026-11-20");
    QVERIFY(store_->createTask(t, kOther).ok());

    client::ClientConfig c = client::loadClientConfig(env(), QString());
    c.url = url_;
    c.store = QStringLiteral("file");
    QVERIFY(client::saveClientConfig(c).ok());
    client::TokenStore store(c.tokenFile, client::runCommand, false);
    QVERIFY(store.writeFile(owner_).ok());
  }
  void cleanup() {
    http_.reset();
    api_.reset();
    store_.reset();
    dir_.reset();
  }

  void everyViewNamesItsControls() {
    MainWindow::Options o;
    o.env = env();
    o.paths = client::desktopPaths(o.env, false);
    o.runner = [](const QString&, const QStringList&, const QByteArray&) { return client::CommandResult{-1, {}, {}}; };
    o.timing = fast();
    o.startupUpdateCheck = false;
    MainWindow w(o);
    w.resize(1280, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY(QTest::qWaitFor([&] { return w.session() && w.session()->health() == Health::Online; }, 5000));
    QVERIFY(QTest::qWaitFor([&] { return w.projectList()->count() == 1; }, 5000));
    w.selectProject(kitchen_);

    const QStringList pages = {QStringLiteral("actionProjectDetails"), QStringLiteral("actionAllProjects"),
                               QStringLiteral("actionOutline"),        QStringLiteral("actionGantt"),
                               QStringLiteral("actionMilestones"),     QStringLiteral("actionActivity")};
    QStringList problems;
    for (const QString& page : pages) {
      w.action(page)->trigger();
      QCoreApplication::processEvents();
      for (const QString& p : unnamed(&w)) problems << page + QStringLiteral(": ") + p;
    }
    // The task editor, with a task selected in the outline.
    w.action(QStringLiteral("actionOutline"))->trigger();
    QVERIFY(w.outline()->selectTask(survey_));
    QVERIFY(QTest::qWaitFor([&] { return w.taskInspector()->taskId() == survey_; }, 3000));
    QCoreApplication::processEvents();
    for (const QString& p : unnamed(&w)) problems << QStringLiteral("task editor: ") + p;
    problems.removeDuplicates();
    QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("\n") + problems.join(QLatin1Char('\n'))));
  }

  void menusToolbarAndShortcuts() {
    MainWindow::Options o;
    o.env = env();
    o.paths = client::desktopPaths(o.env, false);
    o.runner = [](const QString&, const QStringList&, const QByteArray&) { return client::CommandResult{-1, {}, {}}; };
    o.timing = fast();
    o.autoConnect = false;
    o.startupUpdateCheck = false;
    MainWindow w(o);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    // Menu items have text (separators aside), submenus included.
    QList<QMenu*> menus;
    for (QAction* top : w.menuBar()->actions()) {
      if (top->menu()) menus << top->menu();
    }
    for (int i = 0; i < menus.size(); ++i) {
      for (QAction* a : menus.at(i)->actions()) {
        if (a->isSeparator()) continue;
        if (a->menu()) menus << a->menu();
        QVERIFY2(!a->text().remove(QLatin1Char('&')).trimmed().isEmpty(), qPrintable(a->objectName()));
      }
    }
    // Toolbar buttons are named (icon-only buttons say nothing otherwise).
    w.setToolbarShown(true);
    for (QAction* a : w.toolBar()->actions()) {
      if (a->isSeparator()) continue;
      QWidget* b = w.toolBar()->widgetForAction(a);
      QVERIFY(b);
      QVERIFY2(!b->accessibleName().isEmpty(), qPrintable(a->objectName()));
    }
    // Every listed shortcut is a real action, and keys are not shared.
    QHash<QString, QString> owner;
    for (const ShortcutInfo& s : shellShortcuts()) {
      QVERIFY2(w.action(s.action) != nullptr, qPrintable(s.action));
      QVERIFY2(!s.label.isEmpty(), qPrintable(s.action));
      for (const QKeySequence& k : s.keys) {
        const QString key = k.toString(QKeySequence::PortableText);
        QVERIFY2(!owner.contains(key), qPrintable(QStringLiteral("%1 is on %2 and %3").arg(key, owner.value(key), s.action)));
        owner.insert(key, s.action);
      }
    }
  }
};

QTEST_MAIN(TestAccessibility)
#include "tst_accessibility.moc"
