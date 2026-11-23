// The WBS outline (SPEC.md §7.1, §8.3, §8.4; plan PR 6). Two parts:
//  - OutlineModel and OutlineFilter on their own: the tree from cached JSON,
//    WBS codes, summaries' locked roll-ups, inline edits as requests, the
//    recursive status filter (QAbstractItemModelTester throughout);
//  - the outline in the window, offscreen, against the real zprojectd API on
//    loopback with an in-memory store: insert, indent/outdent, move, delete
//    and restore, convert to milestone, inline edits with their dry runs and
//    previews, 412 and summary 400s, find, the filter and offline.
// With ZPROJECT_SCREENSHOT_DIR set, screenshots() renders docs/screenshots/
// outline-*.png from the same example data. Tokens are made per run.
#include <QAbstractItemModelTester>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFont>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

#include "app/EditDialogs.hpp"
#include "app/MainWindow.hpp"
#include "app/OutlineModel.hpp"
#include "app/OutlineView.hpp"
#include "app/TaskActions.hpp"
#include "app/TaskInspector.hpp"
#include "client/ApiClient.hpp"
#include "client/TokenStore.hpp"
#include "server/Api.hpp"
#include "server/http/HttpServer.hpp"
#include "store/Audit.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::app;
using Health = AppSession::Health;
using M = OutlineModel;

namespace {

const store::WriteContext kOther{QStringLiteral("tok_robin"), QStringLiteral("req-other")};

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

QJsonObject jt(const char* id, const char* title, const char* parent, int order, const char* status = "todo",
               int pct = 0, const char* type = "task") {
  return QJsonObject{{QStringLiteral("id"), QLatin1String(id)},
                     {QStringLiteral("title"), QLatin1String(title)},
                     {QStringLiteral("parent"), parent ? QJsonValue(QLatin1String(parent)) : QJsonValue()},
                     {QStringLiteral("order"), order},
                     {QStringLiteral("status"), QLatin1String(status)},
                     {QStringLiteral("percentComplete"), pct},
                     {QStringLiteral("type"), QLatin1String(type)},
                     {QStringLiteral("duration"), QLatin1String(type) == QLatin1String("milestone") ? 0 : 2},
                     {QStringLiteral("priority"), 2}};
}

// Shuffled on purpose: the model sorts by parent and order.
QList<QJsonObject> sample() {
  return {jt("c2", "Counters", "b", 2),           jt("a", "Design", nullptr, 1, "in_progress", 70),
          jt("h", "Handover", nullptr, 4, "todo", 0, "milestone"),
          jt("a2", "Drawings", "a", 2, "in_progress", 40), jt("b", "Build", nullptr, 3),
          jt("a1", "Survey", "a", 1, "done", 100),         jt("p", "Permits", nullptr, 2),
          jt("c1", "Cabinets", "b", 1)};
}

}  // namespace

class TestOutline : public QObject {
  Q_OBJECT

  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<store::SqliteStore> store_;
  std::unique_ptr<server::Api> api_;
  std::unique_ptr<server::http::Server> http_;
  std::function<void(const server::http::Request&)> before_;  // runs before the API sees a request
  QString owner_;
  QString url_;
  QString kitchen_;
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
  QString makeToken(const QJsonObject& body) {
    client::ApiClient c(QUrl(url_), owner_.toLatin1());
    const client::ApiResponse r = c.send("POST", QStringLiteral("/v1/tokens"), {}, &body);
    if (r.status != 201) qFatal("token: %d %s", r.status, r.body.constData());
    return r.json.value(QStringLiteral("token")).toString();
  }
  Task task(const QString& title) { return store_->task(ids_.value(title), true).value(); }
  QString add(const QString& title, const QString& parent, const std::function<void(Task&)>& set = {}) {
    Task t;
    t.project = kitchen_;
    t.title = title;
    t.parent = parent.isEmpty() ? QString() : ids_.value(parent);
    if (set) set(t);
    const QString id = store_->createTask(t, kOther).value().id;
    ids_.insert(title, id);
    return id;
  }
  // Child titles in order, as the server has them.
  QStringList children(const QString& parentTitle) {
    const QString parent = parentTitle.isEmpty() ? QString() : ids_.value(parentTitle);
    QStringList out;
    const QList<Task> all = store_->tasks(kitchen_).value();
    for (const Task& t : all) {
      if (t.parent == parent) out << t.title;
    }
    return out;
  }
  QStringList desktopWrites() {
    QStringList out;
    store::AuditLog log(store_->database());
    const QList<store::AuditEntry> rows = log.since(0, 1000).value();
    for (const auto& e : rows) {
      if (e.method == QLatin1String("GET") || e.method == QLatin1String("CLI")) continue;
      QString path = e.path.section(QLatin1Char('?'), 0, 0);
      for (auto it = ids_.constBegin(); it != ids_.constEnd(); ++it) path.replace(it.value(), it.key());
      path.replace(kitchen_, QStringLiteral("K"));
      out << QStringLiteral("%1 %2%3").arg(e.method, path, e.dryRun ? QStringLiteral(" dry") : QString());
    }
    return out;
  }
  static bool waitHealth(MainWindow& w, Health h, int ms = 5000) {
    return QTest::qWaitFor([&] { return w.session() && w.session()->health() == h; }, ms);
  }
  static bool waitFor(const std::function<bool()>& f, int ms = 5000) { return QTest::qWaitFor(f, ms); }
  // A window, online, on the Kitchen remodel outline.
  std::unique_ptr<MainWindow> open() {
    auto w = std::make_unique<MainWindow>(options());
    w->show();
    if (!QTest::qWaitForWindowExposed(w.get())) qWarning("not exposed");
    if (!waitHealth(*w, Health::Online) || !waitFor([&] { return w->projectList()->count() == 1; })) {
      qWarning("window did not come online");
    }
    w->action(QStringLiteral("actionOutline"))->trigger();
    if (!waitFor([&] { return w->outline()->model()->taskCount() == ids_.size(); })) qWarning("outline not filled");
    return w;
  }
  static QString titleAt(MainWindow& w, const QString& wbs) {
    const M* m = w.outline()->model();
    for (const QString& id : m->depthFirstIds()) {
      if (m->wbsCode(id) == wbs) return m->task(id).value(QStringLiteral("title")).toString();
    }
    return {};
  }
  // "1 Design, 1.1 Survey, …" as the outline shows it.
  static QStringList outlineRows(MainWindow& w) {
    const M* m = w.outline()->model();
    QStringList out;
    for (const QString& id : m->depthFirstIds()) {
      out << m->wbsCode(id) + QLatin1Char(' ') + m->task(id).value(QStringLiteral("title")).toString();
    }
    return out;
  }
  bool select(MainWindow& w, const QString& title) {
    w.outline()->tree()->setFocus();
    return w.outline()->selectTask(ids_.value(title));
  }
  static void key(MainWindow& w, Qt::Key k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    w.activateWindow();
    if (!QTest::qWaitForWindowActive(&w)) qWarning("window not active");
    w.outline()->tree()->setFocus();
    QTest::keyClick(w.outline()->tree(), k, mods);
  }
  // Waits for the outline to show `rows`, refreshing as the app would.
  bool waitRows(MainWindow& w, const QStringList& rows) {
    return waitFor([&] { return outlineRows(w) == rows; }, 8000);
  }
  static PreviewDialog* waitPreview(MainWindow& w) {
    if (!QTest::qWaitFor([&] { return w.taskActions()->preview() != nullptr; }, 5000)) return nullptr;
    return w.taskActions()->preview();
  }
  static bool idle(MainWindow& w) {
    return QTest::qWaitFor([&] { return !w.taskActions()->busy(); }, 5000);
  }

 private slots:
  void initTestCase() {
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  }

  // ---- the model on its own ----

  void modelTree() {
    M m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QList<QJsonObject> tasks = sample();
    for (QJsonObject& t : tasks) {
      if (t[QStringLiteral("id")] == QStringLiteral("a1")) {
        t[QStringLiteral("start")] = QStringLiteral("2026-09-07");
        t[QStringLiteral("finish")] = QStringLiteral("2026-09-08");
      } else if (t[QStringLiteral("id")] == QStringLiteral("a2")) {
        t[QStringLiteral("start")] = QStringLiteral("2026-09-09");
        t[QStringLiteral("finish")] = QStringLiteral("2026-09-11");
      }
    }
    m.setTasks(tasks, {});
    QCOMPARE(m.taskCount(), 8);
    QCOMPARE(m.rowCount(), 4);
    QStringList rows;
    for (const QString& id : m.depthFirstIds()) rows << m.wbsCode(id) + QLatin1Char(' ') + m.task(id)[QStringLiteral("title")].toString();
    QCOMPARE(rows, (QStringList{QStringLiteral("1 Design"), QStringLiteral("1.1 Survey"), QStringLiteral("1.2 Drawings"),
                                QStringLiteral("2 Permits"), QStringLiteral("3 Build"), QStringLiteral("3.1 Cabinets"),
                                QStringLiteral("3.2 Counters"), QStringLiteral("4 Handover")}));
    QCOMPARE(m.headerData(M::Percent, Qt::Horizontal).toString(), QStringLiteral("% Complete"));
    QCOMPARE(m.columnCount(), 10);
    QVERIFY(m.isSummary(QStringLiteral("a")));
    QVERIFY(!m.isSummary(QStringLiteral("a1")));
    QVERIFY(m.isMilestone(QStringLiteral("h")));
    QCOMPARE(m.parentId(QStringLiteral("a2")), QStringLiteral("a"));
    QCOMPARE(m.siblingIndex(QStringLiteral("a2")), 1);
    QCOMPARE(m.childIds(QString()).size(), 4);
    QVERIFY(!m.serverWbs());

    // Summaries: bold, the rolled-up cells locked and not editable.
    const QModelIndex design = m.indexOf(QStringLiteral("a"), M::Title);
    QVERIFY(m.data(design, Qt::FontRole).value<QFont>().bold());
    QCOMPARE(m.data(m.indexOf(QStringLiteral("a"), M::Type)).toString(), QStringLiteral("Summary"));
    const QModelIndex pct = m.indexOf(QStringLiteral("a"), M::Percent);
    QCOMPARE(m.data(pct).toString(), QStringLiteral("70%"));
    QVERIFY(!(m.flags(pct) & Qt::ItemIsEditable));
    QVERIFY(!(m.flags(m.indexOf(QStringLiteral("a"), M::Status)) & Qt::ItemIsEditable));
    QVERIFY(!m.data(pct, Qt::DecorationRole).isNull());
    QCOMPARE(m.data(pct, Qt::ToolTipRole).toString(), QStringLiteral("Rolled up from 2 child tasks"));
    QCOMPARE(m.data(pct, Qt::AccessibleTextRole).toString(), QStringLiteral("70%, rolled up"));
    QVERIFY(m.flags(design) & Qt::ItemIsEditable);  // its title is its own
    // Its dates span its subtasks (display only); no duration until scheduling.
    QCOMPARE(m.data(m.indexOf(QStringLiteral("a"), M::Start)).toString(), QStringLiteral("2026-09-07"));
    QCOMPARE(m.data(m.indexOf(QStringLiteral("a"), M::Finish)).toString(), QStringLiteral("2026-09-11"));
    QCOMPARE(m.data(m.indexOf(QStringLiteral("a"), M::Finish), Qt::ToolTipRole).toString(),
             QStringLiteral("2026-09-11, from its subtasks"));
    QCOMPARE(m.data(m.indexOf(QStringLiteral("a"), M::Duration)).toString(), QString());
    QCOMPARE(m.data(m.indexOf(QStringLiteral("b"), M::Start)).toString(), QString());  // no dates below
    QCOMPARE(m.accessibleText(QStringLiteral("a")), QStringLiteral("1 Design, summary of 2 tasks, 70% complete (rolled up)"));
    // Leaves: editable % and status; dates and duration are the task editor's.
    QVERIFY(m.flags(m.indexOf(QStringLiteral("a2"), M::Percent)) & Qt::ItemIsEditable);
    QVERIFY(m.flags(m.indexOf(QStringLiteral("a2"), M::Status)) & Qt::ItemIsEditable);
    QVERIFY(!(m.flags(m.indexOf(QStringLiteral("a2"), M::Start)) & Qt::ItemIsEditable));
    QVERIFY(m.data(m.indexOf(QStringLiteral("a2"), M::Percent), Qt::DecorationRole).isNull());
    QCOMPARE(m.accessibleText(QStringLiteral("a2")), QStringLiteral("1.2 Drawings, 40% complete, In progress"));
    QCOMPARE(m.data(m.indexOf(QStringLiteral("h"), M::Type)).toString(), QStringLiteral("Milestone"));
    QVERIFY(!m.data(m.indexOf(QStringLiteral("h"), M::Title), Qt::DecorationRole).isNull());
    QCOMPARE(m.data(m.indexOf(QStringLiteral("h"), M::Duration)).toString(), QStringLiteral("0 days"));
    // Offline: nothing is editable.
    m.setEditable(false);
    QVERIFY(!(m.flags(design) & Qt::ItemIsEditable));
    m.setEditable(true);
  }

  void modelServerWbsAndOrphans() {
    M m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QJsonArray wbs;
    auto item = [](const char* id, const char* code, const char* parent) {
      return QJsonObject{{QStringLiteral("id"), QLatin1String(id)},
                         {QStringLiteral("wbs"), QLatin1String(code)},
                         {QStringLiteral("parent"), parent ? QJsonValue(QLatin1String(parent)) : QJsonValue()}};
    };
    for (const auto& i : {item("a", "1", nullptr), item("a1", "1.1", "a"), item("a2", "1.2", "a"), item("p", "2", nullptr),
                          item("b", "3", nullptr), item("c1", "3.1", "b"), item("c2", "3.2", "b"), item("h", "4", nullptr)}) {
      wbs.append(i);
    }
    m.setTasks(sample(), wbs);
    QVERIFY(m.serverWbs());
    // A stale list (one refresh behind a move) is not used.
    QJsonArray stale = wbs;
    stale[3] = item("p", "3", nullptr);
    m.setTasks(sample(), stale);
    QVERIFY(!m.serverWbs());
    QCOMPARE(m.wbsCode(QStringLiteral("p")), QStringLiteral("2"));
    // A task whose parent isn't cached shows at the top level.
    QList<QJsonObject> orphans = sample();
    orphans.append(jt("x", "Orphan", "gone", 1));
    m.setTasks(orphans, {});
    QCOMPARE(m.rowCount(), 5);
    QCOMPARE(m.parentId(QStringLiteral("x")), QString());
  }

  void modelEditsAreRequests() {
    M m;
    m.setTasks(sample(), {});
    QSignalSpy spy(&m, &M::editRequested);
    QVERIFY(!m.setData(m.indexOf(QStringLiteral("a2"), M::Percent), 60));
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("a2"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("percentComplete"));
    QCOMPARE(spy.at(0).at(2).value<QJsonValue>().toInt(), 60);
    QCOMPARE(m.data(m.indexOf(QStringLiteral("a2"), M::Percent)).toString(), QStringLiteral("40%"));  // unchanged
    m.setData(m.indexOf(QStringLiteral("a2"), M::Title), QStringLiteral("  Drawings v2 "));
    QCOMPARE(spy.at(1).at(2).value<QJsonValue>().toString(), QStringLiteral("Drawings v2"));
    // Refused locally: an empty title, % out of range, an unchanged value, a summary's %.
    m.setData(m.indexOf(QStringLiteral("a2"), M::Title), QStringLiteral("   "));
    m.setData(m.indexOf(QStringLiteral("a2"), M::Percent), 101);
    m.setData(m.indexOf(QStringLiteral("a2"), M::Percent), 40);
    m.setData(m.indexOf(QStringLiteral("a"), M::Percent), 10);
    m.setData(m.indexOf(QStringLiteral("a2"), M::Status), QStringLiteral("blocked"));
    QCOMPARE(spy.size(), 2);
    m.setData(m.indexOf(QStringLiteral("p"), M::Status), QStringLiteral("in_progress"));
    m.setData(m.indexOf(QStringLiteral("p"), M::Priority), 1);
    QCOMPARE(spy.size(), 4);
  }

  void filterKeepsAncestors() {
    M m;
    OutlineFilter f;
    f.setSourceModel(&m);
    QAbstractItemModelTester tester(&f, QAbstractItemModelTester::FailureReportingMode::QtTest);
    m.setTasks(sample(), {});
    f.setStatusFilter(QStringLiteral("done"));
    // Survey is done; Design stays as its context.
    QCOMPARE(f.rowCount(), 1);
    const QModelIndex design = f.index(0, M::Title);
    QCOMPARE(design.data().toString(), QStringLiteral("Design"));
    QCOMPARE(f.rowCount(design.siblingAtColumn(0)), 1);
    f.setStatusFilter(QStringLiteral("open"));
    QCOMPARE(f.rowCount(), 4);  // Design (in progress), Permits, Build, Handover
    f.setStatusFilter(QString());
    QCOMPARE(f.rowCount(), 4);
    QCOMPARE(f.rowCount(f.index(0, 0)), 2);
  }

  // ---- the outline in the window ----

  void init() {
    if (QByteArray(QTest::currentTestFunction()).startsWith("model") ||
        QByteArray(QTest::currentTestFunction()) == "filterKeepsAncestors") {
      return;
    }
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());
    store_ = std::move(store::SqliteStore::open(QStringLiteral(":memory:"))).value();
    before_ = nullptr;
    http_ = std::make_unique<server::http::Server>([this](const server::http::Request& r) {
      if (before_) before_(r);
      return api_->handle(r);
    });
    QVERIFY(http_->listen(QHostAddress::LocalHost, 0));
    server::ApiOptions o;
    o.allowedHosts = {QStringLiteral("127.0.0.1:%1").arg(http_->port())};
    api_ = std::make_unique<server::Api>(*store_, o);
    owner_ = api_->createOwnerToken(QStringLiteral("owner")).value();
    url_ = QStringLiteral("http://127.0.0.1:%1").arg(http_->port());
    Project p;
    p.name = QStringLiteral("Kitchen remodel");
    p.owner = QStringLiteral("Alex Rivera");
    p.start = QStringLiteral("2026-09-07");
    p.finish = QStringLiteral("2026-12-18");
    kitchen_ = store_->createProject(p, kOther).value().id;
    ids_.clear();
    auto manual = [](const char* start, const char* finish, double days) {
      return [=](Task& t) {
        t.mode = QStringLiteral("manual");
        t.start = QLatin1String(start);
        t.finish = QLatin1String(finish);
        t.duration = days;
      };
    };
    add(QStringLiteral("Design"), {});
    add(QStringLiteral("Survey"), QStringLiteral("Design"), [&](Task& t) {
      manual("2026-09-07", "2026-09-08", 2)(t);
      t.percentComplete = 100;
      t.status = QStringLiteral("done");
      t.actualStart = QStringLiteral("2026-09-07");
      t.actualFinish = QStringLiteral("2026-09-08");
    });
    add(QStringLiteral("Drawings"), QStringLiteral("Design"), [&](Task& t) {
      manual("2026-09-09", "2026-09-11", 3)(t);
      t.percentComplete = 40;
      t.status = QStringLiteral("in_progress");
      t.actualStart = QStringLiteral("2026-09-09");
    });
    add(QStringLiteral("Permits"), {}, [&](Task& t) {
      manual("2026-09-14", "2026-09-18", 5)(t);
      t.deadline = QStringLiteral("2026-10-15");
      t.priority = 1;
    });
    add(QStringLiteral("Build"), {});
    add(QStringLiteral("Cabinets"), QStringLiteral("Build"), manual("2026-10-19", "2026-10-22", 4));
    add(QStringLiteral("Counters"), QStringLiteral("Build"), manual("2026-10-23", "2026-10-26", 2));
    add(QStringLiteral("Handover"), {}, [&](Task& t) {
      t.type = QStringLiteral("milestone");
      t.duration = 0;
      t.mode = QStringLiteral("manual");
      t.start = QStringLiteral("2026-12-18");
      t.finish = QStringLiteral("2026-12-18");
    });
  }
  void cleanup() {
    before_ = nullptr;
    http_.reset();
    api_.reset();
    store_.reset();
    dir_.reset();
  }

  void outlineShowsTheWbs() {
    login(owner_);
    auto w = open();
    QCOMPARE(w->mainStack()->currentWidget(), w->outline());
    QCOMPARE(outlineRows(*w),
             (QStringList{QStringLiteral("1 Design"), QStringLiteral("1.1 Survey"), QStringLiteral("1.2 Drawings"),
                          QStringLiteral("2 Permits"), QStringLiteral("3 Build"), QStringLiteral("3.1 Cabinets"),
                          QStringLiteral("3.2 Counters"), QStringLiteral("4 Handover")}));
    // The codes agree with GET /v1/projects/{id}/wbs once the syncer has it.
    QVERIFY(waitFor([&] { return w->outline()->model()->serverWbs(); }));
    // Design rolled up from its children on the server (duration-weighted).
    const M* m = w->outline()->model();
    const QString design = ids_.value(QStringLiteral("Design"));
    QCOMPARE(m->data(m->indexOf(design, M::Percent)).toString(), QStringLiteral("64%"));
    QCOMPARE(m->data(m->indexOf(design, M::Status)).toString(), QStringLiteral("In progress"));
    QVERIFY(!(m->flags(m->indexOf(design, M::Percent)) & Qt::ItemIsEditable));
    QCOMPARE(m->data(m->indexOf(ids_.value(QStringLiteral("Permits")), M::Deadline)).toString(),
             QStringLiteral("2026-10-15"));
    QCOMPARE(m->data(m->indexOf(ids_.value(QStringLiteral("Permits")), M::Priority)).toString(), QStringLiteral("P1"));
    // Selecting a task fills the inspector.
    QVERIFY(select(*w, QStringLiteral("Design")));
    QVERIFY(w->taskInspector()->isVisible());
    QCOMPARE(w->taskInspector()->taskId(), design);
    QVERIFY(w->taskInspector()->rollupNote()->text().contains(QStringLiteral("rolled up from 2 child tasks")));
    // Collapsing survives a refresh.
    QTreeView* tree = w->outline()->tree();
    const QModelIndex di = tree->currentIndex().siblingAtColumn(0);
    tree->collapse(di);
    w->session()->refresh();
    QTest::qWait(300);
    QVERIFY(!tree->isExpanded(tree->currentIndex().siblingAtColumn(0)));
    // View → Project Details and back.
    w->action(QStringLiteral("actionProjectDetails"))->trigger();
    QCOMPARE(w->mainStack()->currentIndex(), 1);
    w->findChild<QPushButton*>(QStringLiteral("openOutlineButton"))->click();
    QCOMPARE(w->mainStack()->currentWidget(), w->outline());
  }

  void filterAndFind() {
    login(owner_);
    auto w = open();
    OutlineView* o = w->outline();
    o->setStatusFilter(QStringLiteral("done"));
    QCOMPARE(o->filter()->rowCount(), 1);  // Design, for Survey
    QVERIFY(o->findChild<QLabel*>(QStringLiteral("outlineCounts"))->text().contains(QStringLiteral("filtered: Done")));
    o->setStatusFilter(QString());

    w->outline()->tree()->setFocus();
    QTest::keyClick(w.get(), Qt::Key_F, Qt::ControlModifier);
    QVERIFY(o->findBar()->isVisible());
    QVERIFY(o->findField()->hasFocus());
    QTest::keyClicks(o->findField(), QStringLiteral("co"));  // Counters, and "Design" doesn't match
    QCOMPARE(o->findMatches(), (QStringList{ids_.value(QStringLiteral("Counters"))}));
    QCOMPARE(o->currentTaskId(), ids_.value(QStringLiteral("Counters")));
    QCOMPARE(o->findStatus()->text(), QStringLiteral("1 of 1"));
    o->findField()->clear();
    QTest::keyClicks(o->findField(), QStringLiteral("s"));  // Design, Survey, Drawings, Permits, Cabinets, Counters
    QCOMPARE(o->findMatches().size(), 6);
    QCOMPARE(o->currentTaskId(), ids_.value(QStringLiteral("Design")));
    QTest::keyClick(o->findField(), Qt::Key_Return);
    QCOMPARE(o->currentTaskId(), ids_.value(QStringLiteral("Survey")));
    QTest::keyClick(o->findField(), Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(o->currentTaskId(), ids_.value(QStringLiteral("Design")));
    QTest::keyClick(w.get(), Qt::Key_F3, Qt::ShiftModifier);
    QCOMPARE(o->currentTaskId(), ids_.value(QStringLiteral("Counters")));  // wraps
    o->findField()->clear();
    QTest::keyClicks(o->findField(), QStringLiteral("3.1"));  // a WBS code
    QCOMPARE(o->currentTaskId(), ids_.value(QStringLiteral("Cabinets")));
    QTest::keyClicks(o->findField(), QStringLiteral("9"));
    QCOMPARE(o->findStatus()->text(), QStringLiteral("No matches"));
    QTest::keyClick(o->findField(), Qt::Key_Escape);
    QVERIFY(!o->findBar()->isVisible());
    QVERIFY(o->tree()->hasFocus());
  }

  void insertBelowAndName() {
    login(owner_);
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Permits")));
    const int before = static_cast<int>(desktopWrites().size());
    key(*w, Qt::Key_Return);
    // Created at the end, then moved into place, then its title editor opens.
    QVERIFY(waitFor([&] { return titleAt(*w, QStringLiteral("3")) == QStringLiteral("New task"); }, 8000));
    QVERIFY(waitFor([&] { return w->outline()->tree()->findChild<QLineEdit*>(QStringLiteral("titleEditor")) != nullptr; }));
    QCOMPARE(desktopWrites().mid(before),
             (QStringList{QStringLiteral("POST /v1/projects/K/tasks dry"), QStringLiteral("POST /v1/projects/K/tasks"),
                          QStringLiteral("POST /v1/tasks/%1/move dry"), QStringLiteral("POST /v1/tasks/%1/move")}
                  .replaceInStrings(QStringLiteral("%1"),
                                    w->outline()->model()->task(w->outline()->currentTaskId())[QStringLiteral("id")].toString())));
    QCOMPARE(children(QString()), (QStringList{QStringLiteral("Design"), QStringLiteral("Permits"), QStringLiteral("New task"),
                                              QStringLiteral("Build"), QStringLiteral("Handover")}));
    QLineEdit* ed = w->outline()->tree()->findChild<QLineEdit*>(QStringLiteral("titleEditor"));
    QVERIFY(ed);
    ed->selectAll();
    QTest::keyClicks(ed, QStringLiteral("Order appliances"));
    QTest::keyClick(ed, Qt::Key_Return);
    QVERIFY(waitFor([&] { return titleAt(*w, QStringLiteral("3")) == QStringLiteral("Order appliances"); }, 8000));
    QCOMPARE(desktopWrites().mid(before + 4).size(), 2);  // PATCH dry, PATCH (no preview: nothing ripples)
    QVERIFY(desktopWrites().last().startsWith(QStringLiteral("PATCH /v1/tasks/")));
    QCOMPARE(w->outline()->tree()->findChild<QLineEdit*>(QStringLiteral("titleEditor")), nullptr);
    // Edit → Insert Task with nothing selected: last at the top level.
    w->outline()->tree()->setCurrentIndex({});
    w->outline()->tree()->clearSelection();
    QVERIFY(w->outline()->currentTaskId().isEmpty());
    w->action(QStringLiteral("actionInsertTask"))->trigger();
    QVERIFY(waitFor([&] { return titleAt(*w, QStringLiteral("6")) == QStringLiteral("New task"); }, 8000));
  }

  void indentAndOutdentWithPreview() {
    login(owner_);
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Permits")));
    QVERIFY(w->action(QStringLiteral("actionIndentTask"))->isEnabled());
    QVERIFY(!w->action(QStringLiteral("actionOutdentTask"))->isEnabled());
    key(*w, Qt::Key_Tab);
    // Under Design, which rolls it up: a preview first.
    PreviewDialog* d = waitPreview(*w);
    QVERIFY(d);
    QVERIFY(d->intro()->text().contains(QStringLiteral("under 1 Design")));
    bool designRow = false;
    for (int r = 0; r < d->table()->rowCount(); ++r) {
      if (d->table()->item(r, 1)->text().contains(QStringLiteral("Design"))) designRow = true;
    }
    QVERIFY(designRow);
    QVERIFY(d->cancelButton()->isDefault());
    d->cancelButton()->click();
    QVERIFY(idle(*w));
    QCOMPARE(w->taskActions()->lastMessage(), QStringLiteral("Nothing changed."));
    QCOMPARE(children(QStringLiteral("Design")).size(), 2);
    QVERIFY(waitFor([&] { return w->taskActions()->preview() == nullptr; }));

    key(*w, Qt::Key_Tab);
    d = waitPreview(*w);
    QVERIFY(d);
    d->confirmButton()->click();
    QVERIFY(waitRows(*w, {QStringLiteral("1 Design"), QStringLiteral("1.1 Survey"), QStringLiteral("1.2 Drawings"),
                          QStringLiteral("1.3 Permits"), QStringLiteral("2 Build"), QStringLiteral("2.1 Cabinets"),
                          QStringLiteral("2.2 Counters"), QStringLiteral("3 Handover")}));
    QCOMPARE(w->outline()->currentTaskId(), ids_.value(QStringLiteral("Permits")));
    QCOMPARE(task(QStringLiteral("Permits")).parent, ids_.value(QStringLiteral("Design")));
    QVERIFY(waitFor([&] { return w->action(QStringLiteral("actionOutdentTask"))->isEnabled(); }));
    // Shift+Tab: back after Design.
    w->outline()->tree()->setFocus();
    key(*w, Qt::Key_Backtab, Qt::ShiftModifier);
    d = waitPreview(*w);
    QVERIFY(d);
    d->confirmButton()->click();
    QVERIFY(waitRows(*w, {QStringLiteral("1 Design"), QStringLiteral("1.1 Survey"), QStringLiteral("1.2 Drawings"),
                          QStringLiteral("2 Permits"), QStringLiteral("3 Build"), QStringLiteral("3.1 Cabinets"),
                          QStringLiteral("3.2 Counters"), QStringLiteral("4 Handover")}));
    // Not under a milestone (milestones have no children).
    QVERIFY(select(*w, QStringLiteral("Handover")));
    QVERIFY(waitFor([&] { return w->action(QStringLiteral("actionIndentTask"))->isEnabled(); }));  // under Build
    QVERIFY(select(*w, QStringLiteral("Design")));
    QVERIFY(waitFor([&] { return !w->action(QStringLiteral("actionIndentTask"))->isEnabled(); }));  // first
  }

  void moveUpAndDownWithoutPreview() {
    login(owner_);
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Cabinets")));
    const int before = static_cast<int>(desktopWrites().size());
    key(*w, Qt::Key_Down, Qt::AltModifier);
    // Renumbered siblings aren't a ripple: no preview.
    QVERIFY(waitFor([&] { return children(QStringLiteral("Build")).first() == QStringLiteral("Counters"); }));
    QCOMPARE(w->taskActions()->preview(), nullptr);
    QVERIFY(waitFor([&] { return titleAt(*w, QStringLiteral("3.2")) == QStringLiteral("Cabinets"); }));
    QCOMPARE(desktopWrites().mid(before).size(), 2);
    QCOMPARE(w->outline()->currentTaskId(), ids_.value(QStringLiteral("Cabinets")));
    QVERIFY(waitFor([&] { return !w->action(QStringLiteral("actionMoveTaskDown"))->isEnabled(); }));
    key(*w, Qt::Key_Up, Qt::AltModifier);
    QVERIFY(waitFor([&] { return titleAt(*w, QStringLiteral("3.1")) == QStringLiteral("Cabinets"); }));
    QCOMPARE(children(QStringLiteral("Build")), (QStringList{QStringLiteral("Cabinets"), QStringLiteral("Counters")}));
  }

  void deleteAndRestore() {
    login(owner_);
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Build")));
    key(*w, Qt::Key_Delete);
    PreviewDialog* d = waitPreview(*w);
    QVERIFY(d);
    QVERIFY(d->intro()->text().startsWith(QStringLiteral("Delete 3 Build and everything under it?")));
    QCOMPARE(d->table()->rowCount(), 3);  // Build, Cabinets, Counters
    QCOMPARE(d->confirmButton()->text(), QStringLiteral("Delete"));
    QVERIFY(d->cancelButton()->isDefault());
    d->confirmButton()->click();
    QVERIFY(waitRows(*w, {QStringLiteral("1 Design"), QStringLiteral("1.1 Survey"), QStringLiteral("1.2 Drawings"),
                          QStringLiteral("2 Permits"), QStringLiteral("3 Handover")}));
    QVERIFY(task(QStringLiteral("Cabinets")).deleted);
    QCOMPARE(w->outline()->currentTaskId(), ids_.value(QStringLiteral("Handover")));
    QAction* restore = w->action(QStringLiteral("actionRestoreTask"));
    QVERIFY(waitFor([&] { return restore->isEnabled(); }));
    QCOMPARE(restore->text(), QStringLiteral("Re&store “3 Build”…"));
    restore->trigger();
    d = waitPreview(*w);
    QVERIFY2(d, qPrintable(w->taskActions()->lastMessage()));
    QCOMPARE(d->table()->rowCount(), 3);
    d->confirmButton()->click();
    QVERIFY(waitRows(*w, {QStringLiteral("1 Design"), QStringLiteral("1.1 Survey"), QStringLiteral("1.2 Drawings"),
                          QStringLiteral("2 Permits"), QStringLiteral("3 Build"), QStringLiteral("3.1 Cabinets"),
                          QStringLiteral("3.2 Counters"), QStringLiteral("4 Handover")}));
    QCOMPARE(w->outline()->currentTaskId(), ids_.value(QStringLiteral("Build")));
    QVERIFY(waitFor([&] { return !restore->isEnabled(); }));
  }

  void deleteWithConfirmingToken() {
    // requireConfirm: the dry run's confirmId goes with the delete.
    login(makeToken({{QStringLiteral("name"), QStringLiteral("laptop")},
                     {QStringLiteral("kind"), QStringLiteral("device")},
                     {QStringLiteral("requireConfirm"), true}}));
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Permits")));
    key(*w, Qt::Key_Delete);
    PreviewDialog* d = waitPreview(*w);
    QVERIFY(d);
    d->confirmButton()->click();
    QVERIFY(waitFor([&] { return task(QStringLiteral("Permits")).deleted; }));
  }

  void convertToMilestone() {
    login(owner_);
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Permits")));
    QAction* conv = w->action(QStringLiteral("actionToggleMilestone"));
    QVERIFY(waitFor([&] { return conv->isEnabled(); }));
    QCOMPARE(conv->text(), QStringLiteral("Convert to &Milestone"));
    conv->trigger();
    QVERIFY(waitFor([&] { return task(QStringLiteral("Permits")).type == QLatin1String("milestone"); }));
    QCOMPARE(task(QStringLiteral("Permits")).duration, 0.0);
    QVERIFY(waitFor([&] { return w->outline()->model()->isMilestone(ids_.value(QStringLiteral("Permits"))); }));
    QVERIFY(waitFor([&] { return conv->text() == QStringLiteral("Convert to &Task"); }));
    conv->trigger();
    QVERIFY(waitFor([&] { return task(QStringLiteral("Permits")).type == QLatin1String("task"); }));
    QCOMPARE(task(QStringLiteral("Permits")).duration, 1.0);
    // Not for a summary.
    QVERIFY(select(*w, QStringLiteral("Design")));
    QVERIFY(waitFor([&] { return !conv->isEnabled(); }));
  }

  void leafEditRipplesToItsSummary() {
    login(owner_);
    auto w = open();
    const M* m = w->outline()->model();
    // A summary's % can't be opened for editing.
    QVERIFY(select(*w, QStringLiteral("Design")));
    QTreeView* tree = w->outline()->tree();
    tree->edit(tree->currentIndex().siblingAtColumn(M::Percent));
    QCOMPARE(tree->findChild<QSpinBox*>(QStringLiteral("percentEditor")), nullptr);
    // A leaf's can; Design rolls up, so a preview shows that too.
    QVERIFY(select(*w, QStringLiteral("Drawings")));
    tree->edit(tree->currentIndex().siblingAtColumn(M::Percent));
    QSpinBox* spin = tree->findChild<QSpinBox*>(QStringLiteral("percentEditor"));
    QVERIFY(spin);
    spin->setValue(100);
    QTest::keyClick(spin, Qt::Key_Return);
    PreviewDialog* d = waitPreview(*w);
    QVERIFY(d);
    QStringList details;
    for (int r = 0; r < d->table()->rowCount(); ++r) details << d->table()->item(r, 1)->text() + QLatin1Char(':') + d->table()->item(r, 2)->text();
    QVERIFY2(details.join(QLatin1Char('|')).contains(QStringLiteral("1 Design:")), qPrintable(details.join(QLatin1Char('|'))));
    QVERIFY(details.join(QLatin1Char('|')).contains(QStringLiteral("% complete 64 → 100")));
    d->confirmButton()->click();
    QVERIFY(waitFor([&] { return m->data(m->indexOf(ids_.value(QStringLiteral("Design")), M::Percent)).toString() == QStringLiteral("100%"); }, 8000));
    QCOMPARE(task(QStringLiteral("Drawings")).percentComplete, 100);
    // A leaf with no actual start gets one (SPEC.md §5.2): its scheduled start.
    QVERIFY(select(*w, QStringLiteral("Permits")));
    tree->edit(tree->currentIndex().siblingAtColumn(M::Percent));
    spin = tree->findChild<QSpinBox*>(QStringLiteral("percentEditor"));
    QVERIFY(spin);
    spin->setValue(20);
    QTest::keyClick(spin, Qt::Key_Return);
    QVERIFY(waitFor([&] { return task(QStringLiteral("Permits")).percentComplete == 20; }));
    QCOMPARE(task(QStringLiteral("Permits")).actualStart, QStringLiteral("2026-09-14"));
    // Status through its list.
    QVERIFY(idle(*w));
    tree->edit(tree->currentIndex().siblingAtColumn(M::Status));
    QComboBox* box = tree->findChild<QComboBox*>(QStringLiteral("statusEditor"));
    QVERIFY(box);
    box->setCurrentIndex(box->findData(QStringLiteral("done")));
    QTest::keyClick(box, Qt::Key_Return);
    QVERIFY(waitFor([&] { return task(QStringLiteral("Permits")).status == QLatin1String("done"); }));
  }

  void editConflictOpensTheDialog() {
    login(owner_);
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Permits")));
    // Someone else renames it between our read and our dry run.
    bool once = true;
    before_ = [&](const server::http::Request& r) {
      if (once && r.method == "PATCH" && r.query.hasQueryItem(QStringLiteral("dryRun"))) {
        once = false;
        Task t = task(QStringLiteral("Permits"));
        t.title = QStringLiteral("Permits (city)");
        t.priority = 3;
        QVERIFY(store_->updateTask(t, t.rev, kOther).ok());
      }
    };
    QTreeView* tree = w->outline()->tree();
    tree->edit(tree->currentIndex().siblingAtColumn(M::Title));
    QLineEdit* ed = tree->findChild<QLineEdit*>(QStringLiteral("titleEditor"));
    QVERIFY(ed);
    ed->setText(QStringLiteral("Permits and inspections"));
    QTest::keyClick(ed, Qt::Key_Return);
    QVERIFY(waitFor([&] { return w->taskActions()->conflict() != nullptr; }));
    ConflictDialog* c = w->taskActions()->conflict();
    QCOMPARE(c->table()->rowCount(), 2);  // title (both), priority (theirs)
    QVERIFY(c->cancelButton()->isDefault());
    c->keepMineButton()->click();
    QVERIFY(waitFor([&] { return task(QStringLiteral("Permits")).title == QLatin1String("Permits and inspections"); }));
    QCOMPARE(task(QStringLiteral("Permits")).priority, 3);  // theirs, kept
  }

  void structuralConflictRefreshes() {
    login(owner_);
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Cabinets")));
    bool once = true;
    before_ = [&](const server::http::Request& r) {
      if (once && r.path.endsWith(QStringLiteral("/move")) && r.query.hasQueryItem(QStringLiteral("dryRun"))) {
        once = false;
        Task t = task(QStringLiteral("Cabinets"));
        t.notes = QStringLiteral("Changed elsewhere");
        QVERIFY(store_->updateTask(t, t.rev, kOther).ok());
      }
    };
    key(*w, Qt::Key_Down, Qt::AltModifier);
    QVERIFY(idle(*w));
    QVERIFY(w->taskActions()->lastMessage().contains(QStringLiteral("changed on the server a moment ago")));
    QCOMPARE(children(QStringLiteral("Build")).first(), QStringLiteral("Cabinets"));  // nothing moved
    QVERIFY(w->outline()->message()->text().startsWith(QStringLiteral("⚠ ")));
  }

  void summaryRollupRefusal() {
    login(owner_);
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Permits")));
    // Permits gains a subtask elsewhere just before our % edit is checked.
    bool once = true;
    before_ = [&](const server::http::Request& r) {
      if (once && r.method == "PATCH") {
        once = false;
        add(QStringLiteral("Zoning check"), QStringLiteral("Permits"));
      }
    };
    QTreeView* tree = w->outline()->tree();
    tree->edit(tree->currentIndex().siblingAtColumn(M::Status));
    QComboBox* box = tree->findChild<QComboBox*>(QStringLiteral("statusEditor"));
    QVERIFY(box);
    box->setCurrentIndex(box->findData(QStringLiteral("done")));
    QTest::keyClick(box, Qt::Key_Return);
    QVERIFY(idle(*w));
    QVERIFY(waitFor([&] { return !w->taskActions()->lastMessage().isEmpty(); }));
    QCOMPARE(w->taskActions()->lastMessage(),
             QStringLiteral("This task now has subtasks; its progress is rolled up. The outline is refreshed."));
    ids_.insert(QStringLiteral("Zoning check"), ids_.value(QStringLiteral("Zoning check")));
    QVERIFY(waitFor([&] { return w->outline()->model()->isSummary(ids_.value(QStringLiteral("Permits"))); }));
  }

  void keysBelongToTheTree() {
    login(owner_);
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Permits")));
    // Tab and Del in the sidebar keep their usual meaning.
    w->projectList()->setFocus();
    QTest::keyClick(w->projectList(), Qt::Key_Delete);
    QTest::keyClick(w->projectList(), Qt::Key_Tab);
    QTest::qWait(100);
    QCOMPARE(w->taskActions()->preview(), nullptr);
    QVERIFY(!w->taskActions()->busy());
    // Ctrl+Tab leaves the tree (Tab indents there); nothing is written.
    const int before = static_cast<int>(desktopWrites().size());
    w->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(w.get()));
    w->outline()->tree()->setFocus();
    QTest::keyClick(w->outline()->tree(), Qt::Key_Tab, Qt::ControlModifier);
    QVERIFY(!w->outline()->tree()->hasFocus());
    w->outline()->tree()->setFocus();
    QTest::keyClick(w->outline()->tree(), Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(!w->outline()->tree()->hasFocus());
    QTest::qWait(100);
    QVERIFY(!w->taskActions()->busy());
    QCOMPARE(desktopWrites().size(), before);
  }

  void readOnlyTokenIsTold() {
    login(makeToken({{QStringLiteral("name"), QStringLiteral("viewer")},
                     {QStringLiteral("kind"), QStringLiteral("app")},
                     {QStringLiteral("scopes"), QJsonArray{QStringLiteral("read")}}}));
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Cabinets")));
    key(*w, Qt::Key_Down, Qt::AltModifier);
    QVERIFY(waitFor([&] { return w->taskActions()->lastMessage().contains(QStringLiteral("can't change tasks")); }));
    QCOMPARE(children(QStringLiteral("Build")).first(), QStringLiteral("Cabinets"));
  }

  void offlineIsReadOnly() {
    login(owner_);
    auto w = open();
    QVERIFY(select(*w, QStringLiteral("Permits")));
    QVERIFY(waitFor([&] { return w->action(QStringLiteral("actionIndentTask"))->isEnabled(); }));
    http_.reset();
    w->session()->refresh();
    QVERIFY(waitHealth(*w, Health::Offline));
    for (const char* a : {"actionInsertTask", "actionIndentTask", "actionDeleteTask", "actionToggleMilestone",
                          "actionRenameTask"}) {
      QVERIFY2(!w->action(QString::fromLatin1(a))->isEnabled(), a);
    }
    const M* m = w->outline()->model();
    QVERIFY(!(m->flags(m->indexOf(ids_.value(QStringLiteral("Permits")), M::Title)) & Qt::ItemIsEditable));
    QCOMPARE(m->taskCount(), 8);  // the cache still shows them
    w->taskActions()->moveDown(ids_.value(QStringLiteral("Permits")));
    QCOMPARE(w->taskActions()->lastMessage(), QStringLiteral("Editing is off until the server answers."));
    QVERIFY(w->action(QStringLiteral("actionFind"))->isEnabled());  // reading still works
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
    login(owner_);
    auto w = open();
    w->resize(1180, 640);
    QVERIFY(waitFor([&] { return w->outline()->model()->serverWbs(); }));
    QVERIFY(select(*w, QStringLiteral("Design")));
    w->setTheme(QStringLiteral("light"));
    save(w.get(), QStringLiteral("outline-light.png"));
    w->setTheme(QStringLiteral("dark"));
    save(w.get(), QStringLiteral("outline-dark.png"));
    w->setTheme(QStringLiteral("high-contrast"));
    save(w.get(), QStringLiteral("outline-high-contrast.png"));
    w->setTheme(QStringLiteral("light"));
    w->outline()->setStatusFilter(QStringLiteral("open"));
    w->outline()->showFind();
    QTest::keyClicks(w->outline()->findField(), QStringLiteral("co"));
    save(w.get(), QStringLiteral("outline-filter-find.png"));
    w->outline()->hideFind();
    w->outline()->setStatusFilter(QString());
    QVERIFY(select(*w, QStringLiteral("Permits")));
    key(*w, Qt::Key_Tab);
    PreviewDialog* d = waitPreview(*w);
    QVERIFY(d);
    save(d, QStringLiteral("outline-indent-preview.png"));
    d->cancelButton()->click();
    QVERIFY(idle(*w));
    QVERIFY(waitFor([&] { return w->taskActions()->preview() == nullptr; }));
    QVERIFY(select(*w, QStringLiteral("Build")));
    key(*w, Qt::Key_Delete);
    d = waitPreview(*w);
    QVERIFY(d);
    save(d, QStringLiteral("outline-delete-preview.png"));
    d->cancelButton()->click();
    w->setTheme(QStringLiteral("system"));
  }
};

QTEST_MAIN(TestOutline)
#include "tst_outline.moc"
