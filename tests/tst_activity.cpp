// The project activity view (plan PR 10, §3.6; SPEC.md §8.3, §18) and the
// change wording it shares with the task editor's History tab.
//  - ChangeText: created, deleted, restored, field changes, project labels,
//    who made a change;
//  - ActivityView on its own: rows, names of live and deleted tasks, the
//    actor and kind filters, Enter, notices and the limit;
//  - in the window, offscreen, against the real zprojectd API on loopback
//    with an in-memory store: View → Activity (Ctrl+Shift+A), only this
//    project's changes, "This computer", Enter opening the task, refreshes.
// With ZPROJECT_SCREENSHOT_DIR set, screenshots() renders
// docs/screenshots/activity-*.png from the same example data.
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

#include "app/ActivityView.hpp"
#include "app/ChangeText.hpp"
#include "app/MainWindow.hpp"
#include "app/OutlineView.hpp"
#include "app/Shortcuts.hpp"
#include "client/TokenStore.hpp"
#include "server/Api.hpp"
#include "server/http/HttpServer.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::app;
using Health = AppSession::Health;
using C = ActivityView::Column;

namespace {

const store::WriteContext kOther{QStringLiteral("robin01"), QStringLiteral("req-other")};

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

client::ChangeRecord rec(qint64 seq, const char* actor, const char* kind, const char* id, QJsonObject before,
                         QJsonObject after) {
  client::ChangeRecord c;
  c.seq = seq;
  c.ts = QStringLiteral("2026-11-20T15:%1:00.000Z").arg(seq, 2, 10, QLatin1Char('0'));
  c.actor = QLatin1String(actor);
  c.entityKind = QLatin1String(kind);
  c.entityId = QLatin1String(id);
  c.before = std::move(before);
  c.after = std::move(after);
  return c;
}

QJsonObject task(const char* id, const char* title, int order) {
  return {{QStringLiteral("id"), QLatin1String(id)},
          {QStringLiteral("title"), QLatin1String(title)},
          {QStringLiteral("parent"), QJsonValue()},
          {QStringLiteral("order"), order},
          {QStringLiteral("status"), QStringLiteral("todo")},
          {QStringLiteral("type"), QStringLiteral("task")}};
}

// Newest first, as the cache returns them.
QList<client::ChangeRecord> sample() {
  return {
      rec(6, "me01", "task", "t2", {{QStringLiteral("status"), QStringLiteral("todo")}},
          {{QStringLiteral("status"), QStringLiteral("in_progress")}, {QStringLiteral("rev"), 2}}),
      rec(5, "robin01", "task", "gone", {{QStringLiteral("deleted"), false}},
          {{QStringLiteral("deleted"), true}, {QStringLiteral("title"), QStringLiteral("Old idea")}}),
      rec(4, "system", "task", "t1", {{QStringLiteral("percentComplete"), 0}},
          {{QStringLiteral("percentComplete"), 50}}),
      rec(3, "robin01", "project", "k", {{QStringLiteral("finish"), QStringLiteral("2026-12-11")}},
          {{QStringLiteral("finish"), QStringLiteral("2026-12-18")}}),
      rec(2, "robin01", "task", "t2", {}, task("t2", "Drawings", 2)),
      rec(1, "robin01", "task", "t1", {}, task("t1", "Survey", 1)),
  };
}

}  // namespace

class TestActivity : public QObject {
  Q_OBJECT

  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<store::SqliteStore> store_;
  std::unique_ptr<server::Api> api_;
  std::unique_ptr<server::http::Server> http_;
  QString owner_;
  QString ownerId_;  // the owner token's id: its writes are "This computer"
  QString url_;
  QString kitchen_;
  QString garden_;
  QHash<QString, QString> ids_;  // title → id

  QHash<QString, QString> env() const {
    return {{QStringLiteral("HOME"), dir_->path()},
            {QStringLiteral("ZPROJECT_CONFIG_DIR"), dir_->filePath(QStringLiteral("config"))},
            {QStringLiteral("ZPROJECT_CACHE_DIR"), dir_->filePath(QStringLiteral("cache"))}};
  }
  MainWindow::Options options() {
    MainWindow::Options o;
    o.env = env();
    o.paths = client::desktopPaths(o.env, false);
    o.runner = [](const QString&, const QStringList&, const QByteArray&) { return client::CommandResult{-1, {}, {}}; };
    o.timing = fast();
    o.startupUpdateCheck = false;
    o.systemPalette = lightPalette();
    return o;
  }
  void login(const QString& token) {
    client::ClientConfig c = client::loadClientConfig(env(), QString());
    c.url = url_;
    c.store = QStringLiteral("file");
    QVERIFY(client::saveClientConfig(c).ok());
    client::TokenStore store(c.tokenFile, client::runCommand, false);
    QVERIFY(store.writeFile(token).ok());
  }
  QString add(const QString& project, const QString& title, const store::WriteContext& by = kOther) {
    Task t;
    t.project = project;
    t.title = title;
    const QString id = store_->createTask(t, by).value().id;
    ids_.insert(title, id);
    return id;
  }
  static bool waitHealth(MainWindow& w, Health h, int ms = 5000) {
    return QTest::qWaitFor([&] { return w.session() && w.session()->health() == h; }, ms);
  }
  static bool waitFor(const std::function<bool()>& f, int ms = 5000) { return QTest::qWaitFor(f, ms); }
  // A window, online, on Kitchen remodel's activity.
  std::unique_ptr<MainWindow> open() {
    auto w = std::make_unique<MainWindow>(options());
    w->resize(1280, 600);
    w->show();
    if (!QTest::qWaitForWindowExposed(w.get())) qWarning("not exposed");
    if (!waitHealth(*w, Health::Online) || !waitFor([&] { return w->projectList()->count() == 2; })) {
      qWarning("window did not come online");
    }
    w->selectProject(kitchen_);
    w->action(QStringLiteral("actionActivity"))->trigger();
    if (!waitFor([&] { return !w->activity()->shown().isEmpty(); })) qWarning("activity not filled");
    return w;
  }

 private slots:
  void initTestCase() {
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  }

  void init() {
    const QByteArray fn(QTest::currentTestFunction());
    if (!fn.startsWith("window") && fn != "screenshots") return;
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());
    store_ = std::move(store::SqliteStore::open(QStringLiteral(":memory:"))).value();
    http_ = std::make_unique<server::http::Server>([this](const server::http::Request& r) { return api_->handle(r); });
    QVERIFY(http_->listen(QHostAddress::LocalHost, 0));
    server::ApiOptions o;
    o.allowedHosts = {QStringLiteral("127.0.0.1:%1").arg(http_->port())};
    api_ = std::make_unique<server::Api>(*store_, o);
    owner_ = api_->createOwnerToken(QStringLiteral("owner")).value();
    // zpt_<id>.<secret>
    ownerId_ = owner_.mid(4, owner_.indexOf(QLatin1Char('.')) - 4);
    url_ = QStringLiteral("http://127.0.0.1:%1").arg(http_->port());
    Project p;
    p.name = QStringLiteral("Kitchen remodel");
    kitchen_ = store_->createProject(p, kOther).value().id;
    p.name = QStringLiteral("Garden");
    garden_ = store_->createProject(p, kOther).value().id;
    ids_.clear();
    add(kitchen_, QStringLiteral("Survey"));
    add(kitchen_, QStringLiteral("Drawings"), {ownerId_, QStringLiteral("req-me")});
    add(garden_, QStringLiteral("Plants"));
    Task t = store_->task(ids_.value(QStringLiteral("Survey")), true).value();
    t.status = QStringLiteral("in_progress");
    t.percentComplete = 30;
    t.actualStart = QStringLiteral("2026-11-02");
    QVERIFY(store_->updateTask(t, t.rev, kOther).ok());
  }
  void cleanup() {
    http_.reset();
    api_.reset();
    store_.reset();
    dir_.reset();
  }

  // ---- ChangeText ----

  void describesChanges() {
    QCOMPARE(describeChange(QStringLiteral("task"), {}, task("t", "Survey", 1)), QStringLiteral("Created “Survey”"));
    QCOMPARE(describeChange(QStringLiteral("project"), {},
                            {{QStringLiteral("name"), QStringLiteral("Garden")}, {QStringLiteral("status"), QStringLiteral("active")}}),
             QStringLiteral("Created “Garden”"));
    QCOMPARE(describeChange(QStringLiteral("task"), {{QStringLiteral("deleted"), false}}, {{QStringLiteral("deleted"), true}}),
             QStringLiteral("Deleted"));
    QCOMPARE(describeChange(QStringLiteral("task"), {{QStringLiteral("deleted"), true}}, {{QStringLiteral("deleted"), false}}),
             QStringLiteral("Restored"));
    const QString notes = describeChange(QStringLiteral("task"), {{QStringLiteral("notes"), QStringLiteral("a")}},
                                         {{QStringLiteral("notes"), QStringLiteral("b")}, {QStringLiteral("rev"), 3}});
    QCOMPARE(notes, QStringLiteral("Notes edited"));
    // A project's finish is its target finish; a task's is plain Finish.
    const QJsonObject b{{QStringLiteral("finish"), QStringLiteral("2026-12-11")}};
    const QJsonObject a{{QStringLiteral("finish"), QStringLiteral("2026-12-18")}};
    QVERIFY(describeChange(QStringLiteral("project"), b, a).startsWith(QStringLiteral("Target finish: 2026-12-11 → 2026-12-18")));
    QVERIFY(describeChange(QStringLiteral("task"), b, a).startsWith(QStringLiteral("Finish: 2026-12-11 → 2026-12-18")));
  }

  void namesWhoChangedIt() {
    QCOMPARE(changeActorName(QStringLiteral("system"), {}), QStringLiteral("system"));
    QCOMPARE(changeActorName(QString(), {}), QStringLiteral("system"));
    QCOMPARE(changeActorName(QStringLiteral("me01"), QStringLiteral("zpt_me01")), QStringLiteral("This computer"));
    QCOMPARE(changeActorName(QStringLiteral("robin01"), QStringLiteral("zpt_me01")), QStringLiteral("zpt_robin01"));
    QCOMPARE(changeActorName(QStringLiteral("0123456789abcdef"), {}), QStringLiteral("zpt_01234567…"));
    QCOMPARE(changeActorLabel(QStringLiteral("0123456789abcdef")), QStringLiteral("zpt_0123456789abcdef"));
  }

  // ---- ActivityView on its own ----

  void viewRows() {
    ActivityView v;
    v.setProject(QStringLiteral("k"), QStringLiteral("Kitchen remodel"));
    v.reload(sample(), {task("t1", "Survey", 1), task("t2", "Drawings", 2)}, {}, QStringLiteral("zpt_me01"));
    QTableWidget* t = v.table();
    QCOMPARE(t->rowCount(), 6);
    QCOMPARE(t->editTriggers(), QAbstractItemView::NoEditTriggers);
    QVERIFY(v.heading()->text().contains(QStringLiteral("Kitchen remodel")));
    // Newest first; live tasks by WBS code and title.
    QCOMPARE(t->item(0, C::Who)->text(), QStringLiteral("This computer"));
    QCOMPARE(t->item(0, C::Item)->text(), QStringLiteral("2 Drawings"));
    QCOMPARE(t->item(0, C::Change)->text(), QStringLiteral("Status: todo → in_progress"));
    QCOMPARE(t->item(1, C::Item)->text(), QStringLiteral("Old idea (deleted)"));
    QCOMPARE(t->item(1, C::Change)->text(), QStringLiteral("Deleted"));
    QCOMPARE(t->item(2, C::Who)->text(), QStringLiteral("system"));
    QCOMPARE(t->item(3, C::Item)->text(), QStringLiteral("Project details"));
    QVERIFY(t->item(3, C::Change)->text().startsWith(QStringLiteral("Target finish")));
    QCOMPARE(t->item(5, C::Change)->text(), QStringLiteral("Created “Survey”"));
    QCOMPARE(t->item(3, C::Who)->toolTip(), QStringLiteral("zpt_robin01"));
    // The row in words for screen readers.
    const QString words = t->item(0, C::When)->data(Qt::AccessibleTextRole).toString();
    QVERIFY2(words.contains(QStringLiteral("This computer, 2 Drawings: Status: todo → in_progress")), qPrintable(words));
    QVERIFY(v.note()->text().startsWith(QStringLiteral("6 changes, newest first")));
  }

  void viewFilters() {
    ActivityView v;
    v.reload(sample(), {task("t1", "Survey", 1), task("t2", "Drawings", 2)}, {}, QStringLiteral("zpt_me01"));
    // Everyone, This computer, system, then the others.
    QCOMPARE(v.actorBox()->count(), 4);
    QCOMPARE(v.actorBox()->itemText(1), QStringLiteral("This computer"));
    QCOMPARE(v.actorBox()->itemText(2), QStringLiteral("system"));
    v.setActorFilter(QStringLiteral("robin01"));
    QCOMPARE(v.shown().size(), 4);
    QVERIFY(v.note()->text().contains(QStringLiteral("(of 6)")));
    v.setKindFilter(QStringLiteral("project"));
    QCOMPARE(v.shown().size(), 1);
    QCOMPARE(v.shown().first().seq, 3);
    v.setKindFilter(QStringLiteral("task"));
    QCOMPARE(v.shown().size(), 3);
    // The filters and the selection survive a reload with a new record.
    v.table()->setCurrentCell(1, C::When);
    const qint64 seq = v.currentSeq();
    QList<client::ChangeRecord> more = sample();
    more.prepend(rec(7, "robin01", "task", "t1", {{QStringLiteral("title"), QStringLiteral("Survey")}},
                     {{QStringLiteral("title"), QStringLiteral("Site survey")}}));
    v.reload(more, {task("t1", "Site survey", 1), task("t2", "Drawings", 2)}, {}, QStringLiteral("zpt_me01"));
    QCOMPARE(v.actorBox()->currentData().toString(), QStringLiteral("robin01"));
    QCOMPARE(v.kindBox()->currentData().toString(), QStringLiteral("task"));
    QCOMPARE(v.shown().size(), 4);
    QCOMPARE(v.currentSeq(), seq);
    // Nothing matches.
    v.setActorFilter(QStringLiteral("system"));
    v.setKindFilter(QStringLiteral("project"));
    QVERIFY(v.shown().isEmpty());
    QCOMPARE(v.note()->text(), QStringLiteral("No changes match the filters."));
  }

  void viewEnterOpensLiveTasksOnly() {
    ActivityView v;
    v.reload(sample(), {task("t1", "Survey", 1), task("t2", "Drawings", 2)}, {}, {});
    v.show();
    QVERIFY(QTest::qWaitForWindowExposed(&v));
    QSignalSpy spy(&v, &ActivityView::openTask);
    v.table()->setCurrentCell(0, C::When);
    QTest::keyClick(v.table(), Qt::Key_Return);
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("t2"));
    v.table()->setCurrentCell(1, C::When);  // a deleted task
    QTest::keyClick(v.table(), Qt::Key_Return);
    v.table()->setCurrentCell(3, C::When);  // the project
    QTest::keyClick(v.table(), Qt::Key_Return);
    QCOMPARE(spy.size(), 1);
  }

  void viewNoticesAndEmpty() {
    ActivityView v;
    v.reload({}, {}, {}, {});
    QCOMPARE(v.table()->rowCount(), 0);
    QCOMPARE(v.note()->text(), QStringLiteral("No changes recorded for this project yet."));
    v.setNotice(QStringLiteral("No feed here."));
    QCOMPARE(v.note()->text(), QStringLiteral("No feed here."));
    v.setNotice({});
    QList<client::ChangeRecord> many;
    for (int i = ActivityView::kLimit; i >= 1; --i) many.append(rec(i, "system", "task", "x", {}, {}));
    v.reload(many, {}, {}, {});
    QVERIFY(v.note()->text().contains(QStringLiteral("only the newest")));
  }

  // ---- in the window ----

  void windowShowsThisProjectsChanges() {
    login(owner_);
    auto w = open();
    ActivityView* a = w->activity();
    QCOMPARE(w->mainStack()->currentWidget(), a);
    QCOMPARE(shortcutFor(QStringLiteral("actionActivity")), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    QVERIFY(w->toolBar()->actions().contains(w->action(QStringLiteral("actionActivity"))));
    QVERIFY(!w->action(QStringLiteral("actionActivity"))->icon().isNull());
    // Kitchen remodel: created, Survey, Drawings, Survey's update. Nothing
    // from the garden.
    QCOMPARE(a->shown().size(), 4);
    for (const client::ChangeRecord& c : a->shown()) {
      QVERIFY(c.entityId != ids_.value(QStringLiteral("Plants")));
      QVERIFY(c.entityId != garden_);
    }
    QTableWidget* t = a->table();
    QVERIFY(t->item(0, C::Change)->text().contains(QStringLiteral("in_progress")));
    QCOMPARE(t->item(3, C::Item)->text(), QStringLiteral("Project details"));
    // Drawings was created with this device's token.
    bool mine = false;
    for (int r = 0; r < t->rowCount(); ++r) {
      if (t->item(r, C::Item)->text().endsWith(QStringLiteral("Drawings"))) {
        mine = t->item(r, C::Who)->text() == QStringLiteral("This computer");
      }
    }
    QVERIFY(mine);
    // Back to details and in again with the key.
    w->action(QStringLiteral("actionProjectDetails"))->trigger();
    QTest::keyClick(w.get(), Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(w->mainStack()->currentWidget(), a);
  }

  void windowEnterOpensTheTask() {
    login(owner_);
    auto w = open();
    ActivityView* a = w->activity();
    a->table()->setCurrentCell(0, C::When);  // Survey's update
    QTest::keyClick(a->table(), Qt::Key_Return);
    QCOMPARE(w->mainStack()->currentWidget(), w->outline());
    QCOMPARE(w->outline()->currentTaskId(), ids_.value(QStringLiteral("Survey")));
    QCOMPARE(w->viewTaskId(), ids_.value(QStringLiteral("Survey")));
  }

  void windowFollowsRefreshes() {
    login(owner_);
    auto w = open();
    ActivityView* a = w->activity();
    a->setKindFilter(QStringLiteral("task"));
    QCOMPARE(a->shown().size(), 3);
    const Task survey = store_->task(ids_.value(QStringLiteral("Survey")), true).value();
    QVERIFY(store_->deleteTask(survey.id, survey.rev, kOther).ok());
    w->session()->refresh();
    QVERIFY(waitFor([&] { return a->shown().size() == 4; }));
    QCOMPARE(a->table()->item(0, C::Change)->text(), QStringLiteral("Deleted"));
    QCOMPARE(a->table()->item(0, C::Item)->text(), QStringLiteral("Survey (deleted)"));
    QCOMPARE(a->kindBox()->currentData().toString(), QStringLiteral("task"));
  }

  void screenshots() {
    const QString out = qEnvironmentVariable("ZPROJECT_SCREENSHOT_DIR");
    if (out.isEmpty()) QSKIP("set ZPROJECT_SCREENSHOT_DIR to render the screenshots");
    QVERIFY(QDir().mkpath(out));
    auto save = [&](QWidget* wdg, const QString& name) {
      QCoreApplication::processEvents();
      QTest::qWait(50);
      QVERIFY2(wdg->grab().save(out + QLatin1Char('/') + name), qPrintable(name));
    };
    login(owner_);
    auto w = open();
    w->resize(1280, 480);
    w->action(QStringLiteral("actionToggleInspector"))->trigger();
    w->setTheme(QStringLiteral("light"));
    save(w.get(), QStringLiteral("activity-light.png"));
    w->setTheme(QStringLiteral("dark"));
    save(w.get(), QStringLiteral("activity-dark.png"));
    w->setTheme(QStringLiteral("system"));
  }
};

QTEST_MAIN(TestActivity)
#include "tst_activity.moc"
