// The desktop app shell (SPEC.md §8, §18), offscreen only: themes and their
// contrast, the System fallback, UI zoom, menus and shortcuts, accessible
// names, the status bar and offline banner against the fault-injecting mock
// server, first-run sign-in (the token is never displayed), Preferences →
// Clear cache, Help, About and the single-instance lock. With
// ZPROJECT_SCREENSHOT_DIR set, screenshots() renders the docs/screenshots
// images from the same mock data. Tokens are generated per run.
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStyleHints>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>

#include <sys/stat.h>

#include "MockServer.hpp"
#include "app/Dialogs.hpp"
#include "app/HelpDialog.hpp"
#include "app/MainWindow.hpp"
#include "app/Shortcuts.hpp"
#include "app/SignInDialog.hpp"
#include "app/SingleInstance.hpp"
#include "app/Theme.hpp"
#include "app/UpdateChecker.hpp"
#include "app/UpdateDialog.hpp"
#include "app/UpdateManager.hpp"
#include "client/TokenStore.hpp"
#include "core/Version.hpp"

using namespace zproject;
using namespace zproject::app;
using zproject::test::MockReply;
using zproject::test::MockRequest;
using zproject::test::MockServer;
using Health = AppSession::Health;

namespace {

QString randomToken() {
  QString id;
  QString secret;
  const QString hex = QStringLiteral("0123456789abcdef");
  const QString b64 = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
  auto* rng = QRandomGenerator::global();
  for (int i = 0; i < 20; ++i) id += hex.at(static_cast<int>(rng->bounded(16)));
  for (int i = 0; i < 43; ++i) secret += b64.at(static_cast<int>(rng->bounded(64)));
  return QStringLiteral("zpt_") + id + QLatin1Char('.') + secret;
}

// Example data: three projects with a handful of tasks, as a change feed.
struct ExampleServer {
  QJsonArray feed;
  int status = 0;  // when set, every request gets this error
  QString errorCode;

  static QJsonObject rec(qint64 seq, const QString& kind, const QString& id, const QJsonObject& after) {
    return {{QStringLiteral("seq"), seq},
            {QStringLiteral("ts"), QStringLiteral("2026-11-20T15:%1:00Z").arg(seq % 60, 2, 10, QLatin1Char('0'))},
            {QStringLiteral("actor"), QStringLiteral("owner")},
            {QStringLiteral("entityKind"), kind},
            {QStringLiteral("entityId"), id},
            {QStringLiteral("before"), QJsonValue()},
            {QStringLiteral("after"), after},
            {QStringLiteral("requestId"), QJsonValue()}};
  }
  void project(const QString& id, const QString& name, const QString& owner, const QString& start,
               const QString& finish) {
    feed.append(rec(feed.size() + 1, QStringLiteral("project"), id,
                    {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("rev"), 3},
                     {QStringLiteral("deleted"), false}, {QStringLiteral("order"), feed.size() + 1},
                     {QStringLiteral("status"), QStringLiteral("active")}, {QStringLiteral("owner"), owner},
                     {QStringLiteral("start"), start}, {QStringLiteral("finish"), finish}}));
  }
  void task(const QString& id, const QString& project, const QString& title, double pct, bool milestone = false) {
    feed.append(rec(feed.size() + 1, QStringLiteral("task"), id,
                    {{QStringLiteral("id"), id}, {QStringLiteral("project"), project},
                     {QStringLiteral("title"), title}, {QStringLiteral("rev"), 1}, {QStringLiteral("deleted"), false},
                     {QStringLiteral("order"), feed.size() + 1}, {QStringLiteral("percentComplete"), pct},
                     {QStringLiteral("type"), milestone ? QStringLiteral("milestone") : QStringLiteral("task")}}));
  }
  ExampleServer() {
    project(QStringLiteral("p1"), QStringLiteral("Kitchen remodel"), QStringLiteral("Alex Rivera"),
            QStringLiteral("2026-09-07"), QStringLiteral("2026-12-18"));
    task(QStringLiteral("t1"), QStringLiteral("p1"), QStringLiteral("Design and permits"), 100);
    task(QStringLiteral("t2"), QStringLiteral("p1"), QStringLiteral("Demolition"), 100);
    task(QStringLiteral("t3"), QStringLiteral("p1"), QStringLiteral("Cabinets installed"), 40);
    task(QStringLiteral("t4"), QStringLiteral("p1"), QStringLiteral("Final inspection"), 0, true);
    project(QStringLiteral("p2"), QStringLiteral("Website relaunch"), QStringLiteral("Sam Lee"),
            QStringLiteral("2026-10-01"), QStringLiteral("2027-01-29"));
    task(QStringLiteral("t5"), QStringLiteral("p2"), QStringLiteral("Content audit"), 100);
    task(QStringLiteral("t6"), QStringLiteral("p2"), QStringLiteral("New templates"), 60);
    task(QStringLiteral("t7"), QStringLiteral("p2"), QStringLiteral("Launch"), 0, true);
    project(QStringLiteral("p3"), QStringLiteral("Q1 board report"), QStringLiteral("Alex Rivera"),
            QStringLiteral("2027-01-04"), QStringLiteral("2027-02-12"));
    task(QStringLiteral("t8"), QStringLiteral("p3"), QStringLiteral("Collect figures"), 0);
  }

  MockReply handle(const MockRequest& r) const {
    if (status != 0) return MockReply::error(status, errorCode, QStringLiteral("scripted"));
    if (r.path == QLatin1String("/v1/health")) {
      return MockReply::json(200, {{QStringLiteral("ok"), true},
                                   {QStringLiteral("version"), QStringLiteral("0.7.0")},
                                   {QStringLiteral("apiVersion"), 1},
                                   {QStringLiteral("schemaVersion"), 2}});
    }
    if (r.path == QLatin1String("/v1/changes")) {
      const qint64 since = r.query.queryItemValue(QStringLiteral("since")).toLongLong();
      QJsonArray items;
      qint64 last = since;
      for (const QJsonValue& v : feed) {
        const qint64 seq = v.toObject().value(QStringLiteral("seq")).toInteger();
        if (seq <= since) continue;
        items.append(v);
        last = seq;
      }
      return MockReply::json(200, {{QStringLiteral("items"), items},
                                   {QStringLiteral("nextSince"), last},
                                   {QStringLiteral("more"), false}});
    }
    if (r.path.endsWith(QLatin1String("/wbs"))) return MockReply::json(200, {{QStringLiteral("items"), QJsonArray{}}});
    return MockReply::error(404, QStringLiteral("not_found"), QStringLiteral("no route"));
  }
};

client::ChangeFeedSyncer::Timing fast() {
  client::ChangeFeedSyncer::Timing t;
  t.activeMs = 60'000;  // tests drive cycles with refresh()
  t.inactiveMs = 120'000;
  t.listPollMs = 90'000;
  t.backoffMinMs = 50;
  t.backoffMaxMs = 200;
  return t;
}

QPalette lightPalette() {
  QPalette p;
  p.setColor(QPalette::Window, QColor(0xef, 0xef, 0xef));
  p.setColor(QPalette::WindowText, Qt::black);
  return p;
}

QPalette darkPalette() {
  QPalette p;
  p.setColor(QPalette::Window, QColor(0x20, 0x20, 0x20));
  p.setColor(QPalette::WindowText, Qt::white);
  return p;
}

// Every visible string in a widget tree (labels, buttons, fields' text and
// placeholders, titles, accessible names, tooltips).
QStringList visibleStrings(QWidget* root) {
  QStringList out{root->windowTitle(), root->accessibleName(), root->toolTip()};
  for (QWidget* w : root->findChildren<QWidget*>()) {
    out << w->accessibleName() << w->accessibleDescription() << w->toolTip() << w->windowTitle();
    if (auto* l = qobject_cast<QLabel*>(w)) out << l->text();
    if (auto* b = qobject_cast<QAbstractButton*>(w)) out << b->text();
    if (auto* e = qobject_cast<QLineEdit*>(w)) {
      out << e->placeholderText() << e->displayText();
      if (e->echoMode() == QLineEdit::Normal) out << e->text();
    }
  }
  return out;
}

}  // namespace

class TestAppShell : public QObject {
  Q_OBJECT

  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<MockServer> mock_;
  ExampleServer data_;
  QString token_;
  QList<QStringList> commands_;  // what the fake secret-tool was asked

  QHash<QString, QString> env() const {
    return {{QStringLiteral("HOME"), dir_->path()},
            {QStringLiteral("ZPROJECT_CONFIG_DIR"), dir_->filePath(QStringLiteral("config"))},
            {QStringLiteral("ZPROJECT_CACHE_DIR"), dir_->filePath(QStringLiteral("cache"))}};
  }
  MainWindow::Options options(bool autoConnect = true) {
    MainWindow::Options o;
    o.env = env();
    o.paths = client::desktopPaths(o.env, false);
    o.runner = [this](const QString& program, const QStringList& args, const QByteArray&) {
      commands_.append(QStringList{program} + args);
      return client::CommandResult{-1, {}, {}};  // no Keychain in tests
    };
    o.macos = false;
    o.timing = fast();
    o.autoConnect = autoConnect;
    o.startupUpdateCheck = false;
    o.systemPalette = lightPalette();
    return o;
  }
  client::ClientConfig cli() const { return client::loadClientConfig(env(), QString()); }
  // zproject-cli already logged in on this machine.
  void loginCli() {
    client::ClientConfig c = cli();
    c.url = mock_->url().toString();
    c.store = QStringLiteral("file");
    QVERIFY(client::saveClientConfig(c).ok());
    client::TokenStore store(c.tokenFile, client::runCommand, false);
    QVERIFY(store.writeFile(token_).ok());
  }
  static bool waitHealth(MainWindow& w, Health h, int ms = 5000) {
    return QTest::qWaitFor([&] { return w.session() && w.session()->health() == h; }, ms);
  }
  static bool waitProjects(MainWindow& w, int n, int ms = 5000) {
    return QTest::qWaitFor([&] { return w.projectList()->count() == n; }, ms);
  }

 private slots:
  void initTestCase() {
    Q_INIT_RESOURCE(zproject_app);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  }
  void init() {
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());
    data_ = ExampleServer();
    mock_ = std::make_unique<MockServer>();
    mock_->setHandler([this](const MockRequest& r) { return data_.handle(r); });
    QVERIFY(mock_->listen());
    token_ = randomToken();
    commands_.clear();
  }
  void cleanup() {
    mock_.reset();
    dir_.reset();
  }

  // ---- themes ----

  void themeContrast_data() {
    QTest::addColumn<QString>("theme");
    for (const QString& t : {QStringLiteral("light"), QStringLiteral("dark"), QStringLiteral("high-contrast")})
      QTest::newRow(qPrintable(t)) << t;
  }
  void themeContrast() {
    QFETCH(QString, theme);
    const ThemeColors c = Theme::colors(theme);
    QCOMPARE(c.id, theme);
    const double minText = theme == QLatin1String("high-contrast") ? 7.0 : 4.5;
    for (const QColor& bg : {c.windowBg, c.barBg, c.panelBg, c.inputBg}) {
      QVERIFY2(Theme::contrastRatio(c.fg, bg) >= minText, qPrintable(bg.name()));
      QVERIFY2(Theme::contrastRatio(c.muted, bg) >= 4.5, qPrintable(bg.name()));
      QVERIFY2(Theme::contrastRatio(c.accentText, bg) >= 4.5, qPrintable(bg.name()));
    }
    QVERIFY(Theme::contrastRatio(c.selFg, c.selBg) >= minText);
    QVERIFY(Theme::contrastRatio(c.bannerFg, c.bannerBg) >= minText);
    QVERIFY(Theme::contrastRatio(c.muted, c.bannerBg) >= 1.0);
    if (theme != QLatin1String("high-contrast")) QCOMPARE(c.accent.name(), QStringLiteral("#2fa8f0"));
    const QString css = Theme::styleSheet(c, 12.5);
    QVERIFY(css.contains(QStringLiteral("12.5pt")));
    QVERIFY(css.contains(c.fg.name()));
    QVERIFY(!css.contains(QLatin1Char('@')));  // every token substituted
  }

  void contrastRatioMath() {
    QCOMPARE(qRound(Theme::contrastRatio(Qt::black, Qt::white) * 10), 210);
    QCOMPARE(Theme::contrastRatio(Qt::red, Qt::red), 1.0);
  }

  void systemThemeFallback() {
    QCOMPARE(Theme::resolve(QStringLiteral("light"), darkPalette()), QStringLiteral("light"));
    QCOMPARE(Theme::resolve(QStringLiteral("dark"), lightPalette()), QStringLiteral("dark"));
    QCOMPARE(Theme::resolve(QStringLiteral("high-contrast"), lightPalette()), QStringLiteral("high-contrast"));
    QVERIFY(Theme::paletteIsDark(darkPalette()));
    QVERIFY(!Theme::paletteIsDark(lightPalette()));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Unknown) {
      QSKIP("the platform reports a colour scheme; the palette fallback is not used");
    }
#endif
    // Qt 6.4, or no scheme from the platform: the palette decides.
    QCOMPARE(Theme::resolve(QStringLiteral("system"), darkPalette()), QStringLiteral("dark"));
    QCOMPARE(Theme::resolve(QStringLiteral("system"), lightPalette()), QStringLiteral("light"));
    QCOMPARE(Theme::resolve(QStringLiteral("bogus"), lightPalette()), QStringLiteral("light"));
  }

  void themeMenuAndPersistence() {
    {
      MainWindow w(options(false));
      QCOMPARE(w.themeChoice(), QStringLiteral("system"));
      QVERIFY(w.action(QStringLiteral("actionTheme-system"))->isChecked());
      w.action(QStringLiteral("actionTheme-dark"))->trigger();
      QCOMPARE(w.resolvedTheme(), QStringLiteral("dark"));
      QVERIFY(w.action(QStringLiteral("actionTheme-dark"))->isChecked());
      QCOMPARE(QApplication::palette().color(QPalette::Window), Theme::colors(QStringLiteral("dark")).windowBg);
      QVERIFY(qApp->styleSheet().contains(Theme::colors(QStringLiteral("dark")).windowBg.name()));
    }
    const auto o = options(false);
    QCOMPARE(client::loadDesktopSettings(o.paths).theme, QStringLiteral("dark"));
    QCOMPARE(QFileInfo(o.paths.settingsFile).permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
    MainWindow again(options(false));
    QCOMPARE(again.resolvedTheme(), QStringLiteral("dark"));
    again.setTheme(QStringLiteral("nonsense"));  // ignored
    QCOMPARE(again.themeChoice(), QStringLiteral("dark"));
    again.setTheme(QStringLiteral("system"));
  }

  // ---- zoom ----

  void zoomSteps() {
    QCOMPARE(ViewZoom::stepUp(100), 110);
    QCOMPARE(ViewZoom::stepDown(100), 90);
    QCOMPARE(ViewZoom::stepUp(300), 300);
    QCOMPARE(ViewZoom::stepDown(50), 50);
    QCOMPARE(ViewZoom::stepUp(105), 110);
    QCOMPARE(ViewZoom::clampPercent(1000), 300);
    QCOMPARE(ViewZoom::pointSize(150), 15.0);
  }

  void zoomActionsAndShortcuts() {
    MainWindow w(options(false));
    QCOMPARE(w.action(QStringLiteral("actionZoomIn"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_Equal));
    QVERIFY(w.action(QStringLiteral("actionZoomIn"))->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_Plus)));
    QCOMPARE(w.action(QStringLiteral("actionZoomOut"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_Minus));
    QCOMPARE(w.action(QStringLiteral("actionZoomReset"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_0));
    w.action(QStringLiteral("actionZoomIn"))->trigger();
    w.action(QStringLiteral("actionZoomIn"))->trigger();
    QCOMPARE(w.zoomPercent(), 125);
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("12.5pt")));
    w.action(QStringLiteral("actionZoomOut"))->trigger();
    QCOMPARE(w.zoomPercent(), 110);
    for (int i = 0; i < 20; ++i) w.zoomIn();
    QCOMPARE(w.zoomPercent(), 300);
    QVERIFY(!w.action(QStringLiteral("actionZoomIn"))->isEnabled());
    w.action(QStringLiteral("actionZoomReset"))->trigger();
    QCOMPARE(w.zoomPercent(), 100);
    w.setZoomPercent(175);
    QCOMPARE(client::loadDesktopSettings(options(false).paths).zoomPercent, 175);
    w.zoomReset();
  }

  // The keyboard reaches the actions (shortcut → QAction) in the shown window.
  void shortcutsDispatch() {
    MainWindow w(options(false));
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&w));
    QTest::keyClick(&w, Qt::Key_Equal, Qt::ControlModifier);
    QCOMPARE(w.zoomPercent(), 110);
    QTest::keyClick(&w, Qt::Key_0, Qt::ControlModifier);
    QCOMPARE(w.zoomPercent(), 100);
    QSignalSpy help(w.action(QStringLiteral("actionHelp")), &QAction::triggered);
    QTest::keyClick(&w, Qt::Key_F1);
    QCOMPARE(help.count(), 1);
    QTest::keyClick(&w, Qt::Key_B, Qt::ControlModifier);
    QVERIFY(!w.action(QStringLiteral("actionToggleSidebar"))->isChecked());
    QTest::keyClick(&w, Qt::Key_B, Qt::ControlModifier);
    QVERIFY(w.action(QStringLiteral("actionToggleSidebar"))->isChecked());
    w.close();
  }

  void menusAndShortcutTable() {
    MainWindow w(options(false));
    QStringList menus;
    for (QAction* a : w.menuBar()->actions()) menus << a->text();
    QCOMPARE(menus, (QStringList{QStringLiteral("&File"), QStringLiteral("&Project"), QStringLiteral("&View"),
                                 QStringLiteral("&Help")}));
    // Every listed shortcut is wired to an action, and no two actions share a key.
    QSet<QString> keys;
    for (const ShortcutInfo& s : shellShortcuts()) {
      QAction* a = w.action(s.action);
      QVERIFY2(a, qPrintable(s.action));
      QCOMPARE(a->shortcuts(), s.keys);
      for (const QKeySequence& k : s.keys) {
        QVERIFY2(!keys.contains(k.toString()), qPrintable(k.toString()));
        keys.insert(k.toString());
      }
    }
    QCOMPARE(w.action(QStringLiteral("actionPreferences"))->menuRole(), QAction::PreferencesRole);
    QCOMPARE(w.action(QStringLiteral("actionAbout"))->menuRole(), QAction::AboutRole);
    QCOMPARE(w.action(QStringLiteral("actionRefresh"))->shortcut(), QKeySequence(Qt::Key_F5));
    // The Gantt keeps Ctrl+Shift for its own zoom (SPEC.md §8.4).
    QVERIFY(!keys.contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Equal).toString()));
    QVERIFY(!keys.contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Minus).toString()));
    // Ctrl+1…6 switch views, and other §8.4 keys stay free for later views.
    for (int k = Qt::Key_1; k <= Qt::Key_6; ++k) QVERIFY(!keys.contains(QKeySequence(QKeyCombination(Qt::ControlModifier, static_cast<Qt::Key>(k))).toString()));
    for (const QKeySequence& reserved :
         {QKeySequence(Qt::CTRL | Qt::Key_L), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L),
          QKeySequence(Qt::CTRL | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z),
          QKeySequence(Qt::CTRL | Qt::Key_F), QKeySequence(Qt::CTRL | Qt::Key_K), QKeySequence(Qt::Key_F2)})
      QVERIFY2(!keys.contains(reserved.toString()), qPrintable(reserved.toString()));

    ShortcutsDialog* d = w.showShortcuts();
    QCOMPARE(d->table()->rowCount(), static_cast<int>(shellShortcuts().size()));
    QCOMPARE(w.showShortcuts(), d);
  }

  void accessibleNames() {
    loginCli();
    MainWindow w(options());
    QVERIFY(waitHealth(w, Health::Online));
    QList<QWidget*> roots{&w, w.showPreferences(), w.showHelp(), w.showAbout(), w.showShortcuts(), w.showSignIn()};
    for (QWidget* root : roots) {
      for (QWidget* child : root->findChildren<QWidget*>()) {
        if (child->focusPolicy() == Qt::NoFocus || !child->isVisibleTo(root)) continue;
        if (qobject_cast<QAbstractButton*>(child) && !qobject_cast<QAbstractButton*>(child)->text().isEmpty()) continue;
        if (child->inherits("QScrollBar") || child->inherits("QTabBar") || child->inherits("QHeaderView") ||
            child->inherits("QDockWidget") || child->inherits("QMenuBar") || child->inherits("QMenu"))
          continue;
        if (child->parentWidget() && child->parentWidget()->inherits("QAbstractSpinBox")) continue;  // its editor
        if (child->parentWidget() && child->parentWidget()->inherits("QComboBox")) continue;
        if (child->objectName() == QLatin1String("qt_scrollarea_viewport")) continue;
        if (qobject_cast<QLabel*>(child)) continue;  // selectable labels speak their text
        QVERIFY2(!child->accessibleName().isEmpty(),
                 qPrintable(root->objectName() + QLatin1Char('/') + child->metaObject()->className() +
                            QLatin1Char('#') + child->objectName()));
      }
    }
    for (QAction* a : w.findChildren<QAction*>()) {
      if (!a->isSeparator() && a->menu() == nullptr && !a->objectName().isEmpty()) QVERIFY(!a->text().isEmpty());
    }
  }

  // ---- connection, status bar, banner ----

  void onlineStatus() {
    loginCli();
    MainWindow w(options());
    QVERIFY(!w.needsSignIn());
    QVERIFY(waitHealth(w, Health::Online));
    QVERIFY(waitProjects(w, 3));
    QVERIFY(w.editingAllowed());
    QVERIFY(w.banner()->isHidden());
    QVERIFY(w.connectionStatus()->text().contains(QStringLiteral("Connected")));
    QVERIFY(w.connectionStatus()->text().contains(QStringLiteral("zprojectd 0.7.0")));
    QVERIFY(w.refreshStatus()->text().startsWith(QStringLiteral("Updated ")));
    QCOMPARE(w.projectList()->item(0)->text(), QStringLiteral("Kitchen remodel"));
    QVERIFY(w.findChild<QLabel*>(QStringLiteral("projectSummary"))->text().contains(QStringLiteral("4 tasks")));
    // The token went in the Authorization header, and nowhere visible.
    QVERIFY(!mock_->requests().isEmpty());
    QCOMPARE(mock_->requests().first().header("authorization"), QByteArray("Bearer ") + token_.toLatin1());
    QVERIFY(!visibleStrings(&w).join(QLatin1Char('\n')).contains(token_.mid(token_.indexOf(QLatin1Char('.')) + 1)));
    // A new project on the server shows up on the next poll.
    data_.project(QStringLiteral("p4"), QStringLiteral("Garden"), QStringLiteral("Sam Lee"), QStringLiteral("2027-03-01"),
                  QStringLiteral("2027-04-01"));
    w.action(QStringLiteral("actionRefresh"))->trigger();
    QVERIFY(waitProjects(w, 4));
  }

  void offlineBannerAndReadOnly() {
    loginCli();
    MainWindow w(options());
    QVERIFY(waitHealth(w, Health::Online));
    QVERIFY(waitProjects(w, 3));
    QSignalSpy editing(&w, &MainWindow::editingAllowedChanged);
    data_.status = 503;
    data_.errorCode = QStringLiteral("internal");
    w.session()->refresh();
    QVERIFY(waitHealth(w, Health::Offline));
    QVERIFY(!w.editingAllowed());
    QVERIFY(!editing.isEmpty());
    QCOMPARE(editing.last().at(0).toBool(), false);
    QVERIFY(!w.banner()->isHidden());
    QVERIFY2(w.bannerText()->text().contains(QStringLiteral("Offline — showing data from ")),
             qPrintable(w.bannerText()->text()));
    QVERIFY(w.bannerText()->text().contains(QStringLiteral("Editing is off")));
    QVERIFY(!w.findChild<QLabel*>(QStringLiteral("readOnlyStatus"))->isHidden());
    QCOMPARE(w.projectList()->count(), 3);  // the cache still shows everything
    QString message;
    QVERIFY(!w.clearCache(&message));  // not while offline
    QVERIFY(message.contains(QStringLiteral("only copy")));
    // Back online: Retry now.
    data_.status = 0;
    w.findChild<QPushButton*>(QStringLiteral("bannerRetry"))->click();
    QVERIFY(waitHealth(w, Health::Online));
    QVERIFY(w.banner()->isHidden());
    QVERIFY(w.editingAllowed());
    QCOMPARE(editing.last().at(0).toBool(), true);
  }

  void offlineFromColdStartWithCache() {
    loginCli();
    {
      MainWindow w(options());
      QVERIFY(waitHealth(w, Health::Online));
      QVERIFY(waitProjects(w, 3));
    }
    data_.status = 503;
    MainWindow w(options());
    QCOMPARE(w.projectList()->count(), 3);  // from the cache, before any answer
    QVERIFY(waitHealth(w, Health::Offline));
    QVERIFY(w.bannerText()->text().contains(QStringLiteral("showing data from")));
  }

  void refusedTokenAsksToSignIn() {
    loginCli();
    data_.status = 401;
    data_.errorCode = QStringLiteral("unauthenticated");
    MainWindow w(options());
    QVERIFY(waitHealth(w, Health::SignInNeeded));
    QVERIFY(w.needsSignIn());
    QVERIFY(!w.editingAllowed());
    QVERIFY(w.bannerText()->text().contains(client::tokenLabel(token_)));
    QVERIFY(!w.findChild<QPushButton*>(QStringLiteral("bannerSignIn"))->isHidden());
    SignInDialog* d = w.showSignIn();
    // The refused token is not offered again.
    QVERIFY(!d->useExistingToken());
    QVERIFY(d->findChild<QRadioButton*>(QStringLiteral("useExistingToken")) == nullptr);
    QVERIFY(d->findChild<QLabel*>(QStringLiteral("signInReason"))->text().contains(QStringLiteral("refused")));
    d->reject();
  }

  // ---- first run ----

  void firstRunPastesTokenWithoutShowingIt() {
    MainWindow w(options());
    QVERIFY(w.needsSignIn());
    QVERIFY(w.session() == nullptr);
    QVERIFY(!w.banner()->isHidden());
    QVERIFY(w.bannerText()->text().contains(QStringLiteral("Not connected")));
    SignInDialog* d = w.showSignIn();
    QCOMPARE(w.showSignIn(), d);  // one at a time
    QVERIFY(d->urlEdit()->text().isEmpty());
    QVERIFY(!d->connectButton()->isEnabled());
    d->urlEdit()->setText(QStringLiteral("http://server-host.example.ts.net"));
    QVERIFY(!d->connectButton()->isEnabled());  // http only to loopback
    d->urlEdit()->setText(mock_->url().toString());
    d->tokenEdit()->setText(QStringLiteral("not-a-token"));
    QVERIFY(!d->connectButton()->isEnabled());
    QVERIFY(d->tokenStatus()->text().contains(QStringLiteral("not a zproject token")));
    d->tokenEdit()->setText(token_);
    QVERIFY(d->connectButton()->isEnabled());
    QCOMPARE(d->tokenEdit()->echoMode(), QLineEdit::Password);
    QVERIFY(d->tokenStatus()->text().contains(client::tokenLabel(token_)));
    const QString secret = token_.mid(token_.indexOf(QLatin1Char('.')) + 1);
    QVERIFY(!visibleStrings(d).join(QLatin1Char('\n')).contains(secret));
    QVERIFY(!d->tokenEdit()->displayText().contains(secret));

    d->connectButton()->click();
    QVERIFY(waitHealth(w, Health::Online));
    QVERIFY(waitProjects(w, 3));
    // Stored where zproject-cli reads it: the 0600 token file and cli.ini.
    const client::ClientConfig c = cli();
    QFile f(c.tokenFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromLatin1(f.readAll()).trimmed(), token_);
    struct stat st {};
    QCOMPARE(::stat(QFile::encodeName(c.tokenFile).constData(), &st), 0);
    QCOMPARE(st.st_mode & 0777, 0600u);
    QCOMPARE(c.url, mock_->url().toString());
    QCOMPARE(c.store, QStringLiteral("file"));
    QVERIFY(client::loadDesktopSettings(options().paths).serverUrl.isEmpty());
    // Nothing else on disk holds the secret: zproject.ini, cli.ini, the cache.
    for (const QString& path : {options().paths.settingsFile, c.iniPath(), options().paths.cacheFile()}) {
      QFile other(path);
      if (!other.open(QIODevice::ReadOnly)) continue;
      QVERIFY2(!other.readAll().contains(secret.toLatin1()), qPrintable(path));
    }
    QVERIFY(w.connectionInfo().tokenLabel == client::tokenLabel(token_));
  }

  void firstRunKeychainStore() {
    MainWindow w(options(false));
    SignInDialog* d = w.showSignIn();
    d->urlEdit()->setText(mock_->url().toString());
    d->tokenEdit()->setText(token_);
    auto* box = d->findChild<QComboBox*>(QStringLiteral("tokenStore"));
    box->setCurrentIndex(box->findData(QStringLiteral("keychain")));
    QCOMPARE(d->store(), QStringLiteral("keychain"));
    d->connectButton()->click();
    // The fake secret-tool was asked to store it (on stdin, not in argv).
    QVERIFY(!commands_.isEmpty());
    QVERIFY(commands_.first().first().contains(QStringLiteral("secret-tool")));
    for (const QStringList& cmd : std::as_const(commands_)) QVERIFY(!cmd.join(QLatin1Char(' ')).contains(token_));
  }

  void firstRunOffersCliTokenByLabel() {
    loginCli();
    MainWindow w(options(false));
    SignInDialog* d = w.showSignIn();
    auto* existing = d->findChild<QRadioButton*>(QStringLiteral("useExistingToken"));
    QVERIFY(existing);
    QVERIFY(existing->isChecked());
    QVERIFY(existing->text().contains(client::tokenLabel(token_)));
    QCOMPARE(d->urlEdit()->text(), mock_->url().toString());
    QVERIFY(d->connectButton()->isEnabled());
    QVERIFY(!d->tokenEdit()->isEnabled());
    const QString secret = token_.mid(token_.indexOf(QLatin1Char('.')) + 1);
    QVERIFY(!visibleStrings(d).join(QLatin1Char('\n')).contains(secret));
    d->connectButton()->click();
    QVERIFY(waitHealth(w, Health::Online));
  }

  void differentUrlGoesToZprojectIni() {
    loginCli();
    MockServer other;
    other.setHandler([this](const MockRequest& r) { return data_.handle(r); });
    QVERIFY(other.listen());
    MainWindow w(options(false));
    SignInDialog* d = w.showSignIn();
    d->urlEdit()->setText(other.url().toString());
    d->connectButton()->click();
    QVERIFY(waitHealth(w, Health::Online));
    QCOMPARE(cli().url, mock_->url().toString());  // the CLI keeps its server
    QCOMPARE(client::loadDesktopSettings(options().paths).serverUrl, other.url().toString());
    QCOMPARE(w.connectionInfo().urlSource, QStringLiteral("zproject.ini"));
  }

  // ---- preferences ----

  void preferencesClearCache() {
    loginCli();
    MainWindow w(options());
    QVERIFY(waitHealth(w, Health::Online));
    QVERIFY(waitProjects(w, 3));
    PreferencesDialog* p = w.showPreferences();
    QCOMPARE(w.showPreferences(), p);
    QVERIFY(p->connectionLabel()->text().contains(client::tokenLabel(token_)));
    QVERIFY(!p->connectionLabel()->text().contains(token_));
    const int before = static_cast<int>(mock_->requests().size());
    p->clearCacheButton()->click();
    QVERIFY2(p->clearResult()->text().contains(QStringLiteral("Cache cleared")), qPrintable(p->clearResult()->text()));
    // Reloaded from seq 0.
    QVERIFY(QTest::qWaitFor(
        [&] {
          for (qsizetype i = before; i < mock_->requests().size(); ++i) {
            const auto& r = mock_->requests().at(i);
            if (r.path == QLatin1String("/v1/changes") && r.query.queryItemValue(QStringLiteral("since")) == QLatin1String("0"))
              return true;
          }
          return false;
        },
        5000));
    QVERIFY(waitProjects(w, 3));
    QVERIFY(waitHealth(w, Health::Online));

    // Theme and text size apply at once and are saved.
    p->themeBox()->setCurrentIndex(p->themeBox()->findData(QStringLiteral("high-contrast")));
    QCOMPARE(w.resolvedTheme(), QStringLiteral("high-contrast"));
    p->zoomBox()->setValue(150);
    QCOMPARE(w.zoomPercent(), 150);
    w.setTheme(QStringLiteral("light"));
    QCOMPARE(p->themeBox()->currentData().toString(), QStringLiteral("light"));
    w.zoomReset();
    QCOMPARE(p->zoomBox()->value(), 100);
  }

  void cacheOffKeepsNothingOnDisk() {
    loginCli();
    MainWindow w(options());
    QVERIFY(waitHealth(w, Health::Online));
    const QString file = options().paths.cacheFile();
    QVERIFY(QFile::exists(file));
    w.showPreferences()->cacheBox()->setChecked(false);
    QVERIFY(!QFile::exists(file));
    QCOMPARE(w.session()->cachePath(), QStringLiteral(":memory:"));
    QVERIFY(waitHealth(w, Health::Online));
    QVERIFY(waitProjects(w, 3));
    QVERIFY(!QFile::exists(file));
    QVERIFY(!client::loadDesktopSettings(options().paths).cacheEnabled);
    w.showPreferences()->cacheBox()->setChecked(true);
    QVERIFY(waitHealth(w, Health::Online));
    QVERIFY(QTest::qWaitFor([&] { return QFile::exists(file); }, 5000));
  }

  // ---- help and about ----

  void helpDialog() {
    MainWindow w(options(false));
    QList<QUrl> opened;
    w.setOpenUrlHandler([&](const QUrl& u) { opened << u; });
    QCOMPARE(w.action(QStringLiteral("actionHelp"))->shortcut(), QKeySequence(Qt::Key_F1));
    HelpDialog* h = w.showHelp();
    QVERIFY(!h->isModal());
    QCOMPARE(w.showHelp(), h);
    QVERIFY(h->sourceMarkdown().startsWith(QStringLiteral("# zproject Help")));
    QVERIFY(h->browser()->toPlainText().contains(QStringLiteral("Offline")));
    QVERIFY(h->handleAnchor(QUrl(QStringLiteral("#offline"))));
    QVERIFY(opened.isEmpty());
    QVERIFY(h->handleAnchor(QUrl(QStringLiteral("https://github.com/sbj-ee/zproject"))));
    QCOMPARE(opened.size(), 1);
    QVERIFY(!h->handleAnchor(QUrl(QStringLiteral("http://example.com/"))));
    QVERIFY(!h->handleAnchor(QUrl(QStringLiteral("file:///etc/passwd"))));
    QCOMPARE(opened.size(), 1);
    // Every in-page link in the guide has a heading to land on.
    for (const QString& line : h->sourceMarkdown().split(QLatin1Char('\n'))) {
      const qsizetype at = line.indexOf(QStringLiteral("](#"));
      if (at < 0) continue;
      const QString anchor = line.mid(at + 3, line.indexOf(QLatin1Char(')'), at) - at - 3);
      bool found = false;
      for (const QString& l : h->sourceMarkdown().split(QLatin1Char('\n'))) {
        if (l.startsWith(QStringLiteral("## ")) && HelpDialog::slugify(l.mid(3)) == anchor) found = true;
      }
      QVERIFY2(found, qPrintable(anchor));
    }
    // Help menu links go to https only.
    w.action(QStringLiteral("actionReportIssue"))->trigger();
    w.action(QStringLiteral("actionApiDocs"))->trigger();
    QCOMPARE(opened.size(), 3);
    for (const QUrl& u : opened) QCOMPARE(u.scheme(), QStringLiteral("https"));
  }

  void aboutDialog() {
    loginCli();
    MainWindow w(options());
    QVERIFY(waitHealth(w, Health::Online));
    AboutDialog* a = w.showAbout();
    const QString title = a->findChild<QLabel*>(QStringLiteral("aboutTitle"))->text();
    QVERIFY(title.contains(QStringLiteral("zproject ") + appVersion()));
    QVERIFY(a->details()->text().contains(client::tokenLabel(token_)));
    QVERIFY(a->details()->text().contains(QStringLiteral("0.7.0")));
    QVERIFY(!visibleStrings(a).join(QLatin1Char('\n')).contains(token_));
  }

  // ---- single instance ----

  void singleInstanceLock() {
    const QString path = dir_->filePath(QStringLiteral("cache/zproject.lock"));
    SingleInstance first(path);
    QVERIFY(first.acquire());
    QVERIFY(first.held());
    struct stat st {};
    QCOMPARE(::stat(QFile::encodeName(dir_->filePath(QStringLiteral("cache"))).constData(), &st), 0);
    QCOMPARE(st.st_mode & 0777, 0700u);
    SingleInstance second(path);
    qint64 pid = 0;
    QVERIFY(!second.acquire(&pid));
    QCOMPARE(pid, QCoreApplication::applicationPid());
  }

  // ---- updates (SPEC.md §21; the updater itself is tst_update) ----

  void updatesMenuPreferencesAndFirstRun() {
    MainWindow w(options(false));
    // https only: this loopback http URL is refused before any request.
    w.updater()->checker()->setApiUrl(QUrl(QStringLiteral("http://127.0.0.1:1/latest")));
    QAction* check = w.action(QStringLiteral("actionCheckUpdates"));
    QVERIFY(check);
    QCOMPARE(check->menuRole(), QAction::ApplicationSpecificRole);
    QVERIFY(w.menuBar()->actions().last()->menu()->actions().contains(check));  // under Help
    check->trigger();
    UpdateDialog* d = w.updater()->dialog();
    QVERIFY(d);
    QVERIFY(QTest::qWaitFor([d] { return d->state() == UpdateDialog::State::CheckFailed; }, 5000));
    d->close();
    QVERIFY(w.updateStatePath().endsWith(QStringLiteral("/update-state.ini")));
    QCOMPARE(QFileInfo(w.updateStatePath()).absolutePath(), QFileInfo(options().paths.settingsFile).absolutePath());

    // First run asks once, default yes.
    QMessageBox* box = w.startUpdates();
    QVERIFY(box);
    auto* yes = box->findChild<QPushButton*>(QStringLiteral("updateQuestionYes"));
    QVERIFY(yes);
    QCOMPARE(box->defaultButton(), yes);
    QTest::mouseClick(box->findChild<QPushButton*>(QStringLiteral("updateQuestionNo")), Qt::LeftButton);
    client::DesktopSettings saved = client::loadDesktopSettings(options().paths);
    QVERIFY(saved.updateAsked);
    QVERIFY(!saved.updateCheckDaily);
    QVERIFY(w.startUpdates() == nullptr);  // never asked again

    PreferencesDialog* p = w.showPreferences();
    QVERIFY(!p->updatesBox()->isChecked());
    p->updatesBox()->setChecked(true);
    QVERIFY(client::loadDesktopSettings(options().paths).updateCheckDaily);
    QTest::mouseClick(p->checkNowButton(), Qt::LeftButton);
    UpdateDialog* again = w.updater()->dialog();
    QVERIFY(again != nullptr);
    QVERIFY(QTest::qWaitFor([again] { return again->state() == UpdateDialog::State::CheckFailed; }, 5000));
  }

  void singleInstanceRelease() {
    const QString path = dir_->filePath(QStringLiteral("release.lock"));
    SingleInstance first(path);
    QVERIFY(first.acquire());
    first.release();
    QVERIFY(!first.held());
    SingleInstance second(path);
    QVERIFY(second.acquire());  // the restarted app gets the lock
  }

  // ---- screenshots (docs/screenshots/), only on request ----

  void screenshots() {
    const QString out = qEnvironmentVariable("ZPROJECT_SCREENSHOT_DIR");
    if (out.isEmpty()) QSKIP("set ZPROJECT_SCREENSHOT_DIR to render the screenshots");
    QVERIFY(QDir().mkpath(out));
    auto save = [&](QWidget* wdg, const QString& name) {
      QCoreApplication::processEvents();
      QTest::qWait(50);
      QVERIFY2(wdg->grab().save(out + QLatin1Char('/') + name), qPrintable(name));
    };
    loginCli();
    {
      MainWindow w(options());
      w.resize(1100, 640);
      w.show();
      QVERIFY(QTest::qWaitForWindowExposed(&w));
      QVERIFY(waitHealth(w, Health::Online));
      QVERIFY(waitProjects(w, 3));
      w.setTheme(QStringLiteral("light"));
      save(&w, QStringLiteral("main-light.png"));
      w.setTheme(QStringLiteral("dark"));
      save(&w, QStringLiteral("main-dark.png"));
      w.setTheme(QStringLiteral("high-contrast"));
      save(&w, QStringLiteral("main-high-contrast.png"));
      w.setTheme(QStringLiteral("light"));

      PreferencesDialog* p = w.showPreferences();
      p->adjustSize();
      save(p, QStringLiteral("preferences.png"));
      p->close();
      HelpDialog* h = w.showHelp();
      save(h, QStringLiteral("help.png"));
      h->close();
      AboutDialog* a = w.showAbout();
      a->adjustSize();
      save(a, QStringLiteral("about.png"));
      a->close();

      data_.status = 503;
      data_.errorCode = QStringLiteral("internal");
      w.session()->refresh();
      QVERIFY(waitHealth(w, Health::Offline));
      save(&w, QStringLiteral("offline-light.png"));
      w.setTheme(QStringLiteral("dark"));
      save(&w, QStringLiteral("offline-dark.png"));
      w.setTheme(QStringLiteral("system"));
      w.close();
    }
    // First run: a clean machine.
    QVERIFY(QFile::remove(cli().iniPath()));
    QVERIFY(QFile::remove(cli().tokenFile));
    MainWindow w(options());
    w.setTheme(QStringLiteral("light"));
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    SignInDialog* d = w.showSignIn();
    d->urlEdit()->setText(QStringLiteral("https://server-host.example.ts.net"));
    d->tokenEdit()->setText(token_);
    d->adjustSize();
    save(d, QStringLiteral("first-run.png"));
    d->reject();
    w.setTheme(QStringLiteral("system"));
  }
};

QTEST_MAIN(TestAppShell)
#include "tst_appshell.moc"
