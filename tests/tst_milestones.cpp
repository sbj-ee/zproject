// The milestones view (plan PR 9, §3.5; SPEC.md §7.1, §8.3, §18).
//  - milestoneRows() on its own: which tasks count, order, WBS codes, the
//    parent, days to go or late, missed deadlines, the words for screen
//    readers;
//  - the view in the window, offscreen, against the real zprojectd API on
//    loopback with an in-memory store: View → Milestones (Ctrl+Shift+M),
//    the "All projects" box, Enter and Show in Gantt jumping to the task,
//    refreshes, and an empty project.
// With ZPROJECT_SCREENSHOT_DIR set, screenshots() renders
// docs/screenshots/milestones-*.png from the same example data.
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QImage>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QTreeView>

#include "app/GanttView.hpp"
#include "app/MainWindow.hpp"
#include "app/MilestonesView.hpp"
#include "app/OutlineView.hpp"
#include "app/Shortcuts.hpp"
#include "client/TokenStore.hpp"
#include "server/Api.hpp"
#include "server/http/HttpServer.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::app;
using Health = AppSession::Health;
using C = MilestonesView::Column;

namespace {

const store::WriteContext kOther{QStringLiteral("tok_robin"), QStringLiteral("req-other")};
const QDate kToday(2026, 10, 1);

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

QJsonObject jt(const char* id, const char* title, const char* parent, int order, const char* type = "task",
               const char* finish = nullptr, const char* status = "todo") {
  QJsonObject o{{QStringLiteral("id"), QLatin1String(id)},
                {QStringLiteral("title"), QLatin1String(title)},
                {QStringLiteral("parent"), parent ? QJsonValue(QLatin1String(parent)) : QJsonValue()},
                {QStringLiteral("order"), order},
                {QStringLiteral("status"), QLatin1String(status)},
                {QStringLiteral("type"), QLatin1String(type)},
                {QStringLiteral("mode"), QStringLiteral("manual")},
                {QStringLiteral("duration"), QLatin1String(type) == QLatin1String("milestone") ? 0 : 3}};
  if (finish) {
    o[QStringLiteral("start")] = QLatin1String(finish);
    o[QStringLiteral("finish")] = QLatin1String(finish);
  }
  return o;
}

QList<MilestoneSource> sample() {
  QJsonObject handover = jt("h", "Handover", nullptr, 3, "milestone", "2026-11-20");
  handover[QStringLiteral("deadline")] = QStringLiteral("2026-11-18");  // missed
  QJsonObject signoff = jt("d2", "Design sign-off", "d", 2, "milestone", "2026-09-25");  // late
  return {{QStringLiteral("k"), QStringLiteral("Kitchen remodel"),
           {jt("d", "Design", nullptr, 1),
            jt("d1", "Drawings", "d", 1, "task", "2026-09-20"),
            signoff,
            jt("b", "Build", nullptr, 2),
            jt("b1", "Permit granted", "b", 1, "milestone", "2026-09-10", "done"),
            jt("b2", "Inspection", "b", 2, "milestone"),  // no date yet
            handover},
           {},
           QStringLiteral("#d9822b")},
          {QStringLiteral("g"), QStringLiteral("Garden"),
           {jt("g1", "Plants ordered", nullptr, 1, "milestone", "2026-10-01"),
            jt("g2", "Planting done", nullptr, 2, "milestone", "2026-10-02")},
           {},
           QString()}};  // the garden has no color
}

const MilestoneRow* rowOf(const QList<MilestoneRow>& rows, const QString& id) {
  for (const MilestoneRow& r : rows) {
    if (r.taskId == id) return &r;
  }
  return nullptr;
}

}  // namespace

class TestMilestones : public QObject {
  Q_OBJECT

  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<store::SqliteStore> store_;
  std::unique_ptr<server::Api> api_;
  std::unique_ptr<server::http::Server> http_;
  QString owner_;
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
    o.today = kToday;
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
  QString add(const QString& project, const QString& title, const QString& parent,
              const std::function<void(Task&)>& set = {}) {
    Task t;
    t.project = project;
    t.title = title;
    t.parent = parent.isEmpty() ? QString() : ids_.value(parent);
    if (set) set(t);
    const QString id = store_->createTask(t, kOther).value().id;
    ids_.insert(title, id);
    return id;
  }
  static std::function<void(Task&)> milestone(const char* date, const char* deadline = nullptr) {
    return [=](Task& t) {
      t.type = QStringLiteral("milestone");
      t.duration = 0;
      t.mode = QStringLiteral("manual");
      t.start = QLatin1String(date);
      t.finish = QLatin1String(date);
      if (deadline) t.deadline = QLatin1String(deadline);
    };
  }
  static bool waitHealth(MainWindow& w, Health h, int ms = 5000) {
    return QTest::qWaitFor([&] { return w.session() && w.session()->health() == h; }, ms);
  }
  static bool waitFor(const std::function<bool()>& f, int ms = 5000) { return QTest::qWaitFor(f, ms); }
  // A window, online, showing Kitchen remodel's milestones.
  std::unique_ptr<MainWindow> open() {
    auto w = std::make_unique<MainWindow>(options());
    w->resize(1280, 640);
    w->show();
    if (!QTest::qWaitForWindowExposed(w.get())) qWarning("not exposed");
    if (!waitHealth(*w, Health::Online) || !waitFor([&] { return w->projectList()->count() == 2; })) {
      qWarning("window did not come online");
    }
    w->selectProject(kitchen_);
    w->action(QStringLiteral("actionMilestones"))->trigger();
    if (!waitFor([&] { return w->milestones()->rows().size() == 3; })) qWarning("milestones not filled");
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
    url_ = QStringLiteral("http://127.0.0.1:%1").arg(http_->port());
    Project p;
    p.name = QStringLiteral("Kitchen remodel");
    p.owner = QStringLiteral("Alex Rivera");
    p.color = QStringLiteral("#d9822b");
    kitchen_ = store_->createProject(p, kOther).value().id;
    p.name = QStringLiteral("Garden");
    p.color = QStringLiteral("#3a9d5d");
    garden_ = store_->createProject(p, kOther).value().id;
    ids_.clear();
    add(kitchen_, QStringLiteral("Design"), {});
    add(kitchen_, QStringLiteral("Drawings"), QStringLiteral("Design"), [](Task& t) {
      t.mode = QStringLiteral("manual");
      t.start = QStringLiteral("2026-09-09");
      t.finish = QStringLiteral("2026-09-15");
      t.duration = 5;
    });
    add(kitchen_, QStringLiteral("Design sign-off"), QStringLiteral("Design"), milestone("2026-09-25"));
    add(kitchen_, QStringLiteral("Build"), {});
    add(kitchen_, QStringLiteral("Cabinets fitted"), QStringLiteral("Build"), milestone("2026-10-28"));
    add(kitchen_, QStringLiteral("Handover"), {}, milestone("2026-11-20", "2026-11-18"));
    add(garden_, QStringLiteral("Plants ordered"), {}, milestone("2026-10-05"));
  }
  void cleanup() {
    http_.reset();
    api_.reset();
    store_.reset();
    dir_.reset();
  }

  // ---- milestoneRows() ----

  void rowsAreMilestonesInDateOrder() {
    const QList<MilestoneRow> rows = milestoneRows(sample(), kToday);
    QStringList ids;
    for (const MilestoneRow& r : rows) ids << r.taskId;
    // Dated by date (ties: project name, then WBS), then the undated one.
    QCOMPARE(ids, (QStringList{QStringLiteral("b1"), QStringLiteral("d2"), QStringLiteral("g1"), QStringLiteral("g2"),
                               QStringLiteral("h"), QStringLiteral("b2")}));
    // Plain tasks and summaries are not milestones.
    QVERIFY(rowOf(rows, QStringLiteral("d1")) == nullptr);
    QVERIFY(rowOf(rows, QStringLiteral("d")) == nullptr);
  }

  void rowsCarryWbsParentAndProject() {
    const QList<MilestoneRow> rows = milestoneRows(sample(), kToday);
    const MilestoneRow* s = rowOf(rows, QStringLiteral("d2"));
    QVERIFY(s);
    QCOMPARE(s->wbs, QStringLiteral("1.2"));
    QCOMPARE(s->parentTitle, QStringLiteral("Design"));
    QCOMPARE(s->projectName, QStringLiteral("Kitchen remodel"));
    const MilestoneRow* h = rowOf(rows, QStringLiteral("h"));
    QCOMPARE(h->wbs, QStringLiteral("3"));
    QVERIFY(h->parentTitle.isEmpty());
    QCOMPARE(rowOf(rows, QStringLiteral("g1"))->projectId, QStringLiteral("g"));
  }

  void dueDaysLateAndDone() {
    const QList<MilestoneRow> rows = milestoneRows(sample(), kToday);
    const MilestoneRow* late = rowOf(rows, QStringLiteral("d2"));
    QCOMPARE(late->days, -6);
    QVERIFY(late->late);
    QCOMPARE(milestoneDueText(*late), QStringLiteral("6 days late"));
    const MilestoneRow* today = rowOf(rows, QStringLiteral("g1"));
    QVERIFY(!today->late);
    QCOMPARE(milestoneDueText(*today), QStringLiteral("Today"));
    QCOMPARE(milestoneDueText(*rowOf(rows, QStringLiteral("g2"))), QStringLiteral("Tomorrow"));
    QCOMPARE(milestoneDueText(*rowOf(rows, QStringLiteral("h"))), QStringLiteral("In 50 days"));
    // Done is never late, whatever its date.
    const MilestoneRow* done = rowOf(rows, QStringLiteral("b1"));
    QVERIFY(done->done);
    QVERIFY(!done->late);
    QCOMPARE(milestoneDueText(*done), QStringLiteral("Done"));
    const MilestoneRow* undated = rowOf(rows, QStringLiteral("b2"));
    QVERIFY(!undated->date.isValid());
    QVERIFY(!undated->late);
    QCOMPARE(milestoneDueText(*undated), QStringLiteral("No date"));
    MilestoneRow one = *late;
    one.days = -1;
    QCOMPARE(milestoneDueText(one), QStringLiteral("1 day late"));
  }

  void missedDeadlines() {
    const QList<MilestoneRow> rows = milestoneRows(sample(), kToday);
    QVERIFY(rowOf(rows, QStringLiteral("h"))->afterDeadline);
    QVERIFY(!rowOf(rows, QStringLiteral("g1"))->afterDeadline);
    const QString words = milestoneAccessibleText(*rowOf(rows, QStringLiteral("h")), true);
    QVERIFY2(words.startsWith(QStringLiteral("Milestone 3 Handover, Kitchen remodel, ")), qPrintable(words));
    QVERIFY2(words.contains(QStringLiteral("in 50 days")), qPrintable(words));
    QVERIFY2(words.contains(QStringLiteral("after its deadline of")), qPrintable(words));
    QVERIFY(!milestoneAccessibleText(*rowOf(rows, QStringLiteral("h")), false).contains(QStringLiteral("Kitchen")));
  }

  void viewOnItsOwn() {
    MilestonesView v;
    v.setToday(kToday);
    v.setProject(QStringLiteral("k"), QStringLiteral("Kitchen remodel"));
    v.reload({sample().first()});
    QCOMPARE(v.table()->rowCount(), 4);
    QVERIFY(v.table()->isColumnHidden(C::Project));
    QCOMPARE(v.table()->editTriggers(), QAbstractItemView::NoEditTriggers);
    QVERIFY(v.heading()->text().contains(QStringLiteral("Kitchen remodel")));
    // The late one says so in words, not color alone.
    const int late = 1;
    QCOMPARE(v.rows().at(late).taskId, QStringLiteral("d2"));
    QVERIFY(v.table()->item(late, C::Due)->text().contains(QStringLiteral("6 days late")));
    QVERIFY(v.table()->item(late, C::Due)->text().startsWith(QStringLiteral("⚠")));
    QVERIFY(v.table()->item(2, C::Deadline)->text().startsWith(QStringLiteral("⚠")));  // Handover
    QCOMPARE(v.table()->item(3, C::Date)->text(), QStringLiteral("no date"));
    QVERIFY(v.note()->text().contains(QStringLiteral("4 milestones")));
    QVERIFY(v.note()->text().contains(QStringLiteral("1 late")));
    QVERIFY(v.note()->text().contains(QStringLiteral("1 with no date")));
    QVERIFY(!v.table()->item(0, C::Title)->data(Qt::AccessibleTextRole).toString().isEmpty());
    // The selection survives a reload.
    QVERIFY(v.selectTask(QStringLiteral("h")));
    v.reload({sample().first()});
    QCOMPARE(v.currentTaskId(), QStringLiteral("h"));
    // All projects: the project column.
    v.setAllProjects(true);
    v.reload(sample());
    QVERIFY(!v.table()->isColumnHidden(C::Project));
    QVERIFY(v.heading()->text().contains(QStringLiteral("all projects")));
    QCOMPARE(v.table()->rowCount(), 6);
    // Nothing to show.
    v.setAllProjects(false);
    v.reload({{QStringLiteral("e"), QStringLiteral("Empty"), {jt("t", "Task", nullptr, 1)}, {}, QString()}});
    QCOMPARE(v.table()->rowCount(), 0);
    QVERIFY(v.note()->text().contains(QStringLiteral("no milestones")));
    QVERIFY(!v.outlineButton()->isEnabled());
    QVERIFY(!v.ganttButton()->isEnabled());
  }

  void projectColours() {
    // The rows carry the project's color; a project without one has none.
    const QList<MilestoneRow> rows = milestoneRows(sample(), kToday);
    QCOMPARE(rowOf(rows, QStringLiteral("h"))->projectColor, QColor(QStringLiteral("#d9822b")));
    QVERIFY(!rowOf(rows, QStringLiteral("g1"))->projectColor.isValid());
    MilestonesView v;
    v.setToday(kToday);
    v.setOutlineColor(Qt::black);
    // One project: plain text diamonds, no color.
    v.reload({sample().first()});
    QVERIFY(v.table()->item(0, C::Title)->icon().isNull());
    QVERIFY(v.table()->item(0, C::Title)->text().startsWith(QStringLiteral("◇")));  // b1 is done
    // All projects: the kitchen's diamonds and Project cells in its color
    // (hollow when done); the garden's stay text.
    v.setAllProjects(true);
    v.reload(sample());
    for (int i = 0; i < v.rows().size(); ++i) {
      const MilestoneRow& r = v.rows().at(i);
      QTableWidgetItem* title = v.table()->item(i, C::Title);
      QTableWidgetItem* project = v.table()->item(i, C::Project);
      if (r.projectId == QLatin1String("k")) {
        QVERIFY(!title->icon().isNull());
        QVERIFY(!project->icon().isNull());
        QCOMPARE(title->text(), r.title);  // the drawn diamond replaces the text one
        QVERIFY(project->toolTip().contains(QStringLiteral("#d9822b")));
        // The swatch is the project's color.
        const QImage img = project->icon().pixmap(16, 16).toImage();
        QCOMPARE(img.pixelColor(8, 8), QColor(QStringLiteral("#d9822b")));
      } else {
        QVERIFY(title->icon().isNull());
        QVERIFY(title->text().startsWith(QStringLiteral("◆")));
      }
      // Still said in words.
      QVERIFY(title->data(Qt::AccessibleTextRole).toString().contains(r.projectName));
    }
    // Back to one project: colors go.
    v.setAllProjects(false);
    v.reload({sample().first()});
    QVERIFY(v.table()->item(1, C::Title)->icon().isNull());
  }

  void enterAsksToOpen() {
    MilestonesView v;
    v.setToday(kToday);
    v.reload({sample().first()});
    v.show();
    QVERIFY(QTest::qWaitForWindowExposed(&v));
    QSignalSpy spy(&v, &MilestonesView::openTask);
    QVERIFY(v.selectTask(QStringLiteral("h")));
    QTest::keyClick(v.table(), Qt::Key_Return);
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("k"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("h"));
    QCOMPARE(spy.at(0).at(2).toBool(), false);
    v.ganttButton()->click();
    QCOMPARE(spy.size(), 2);
    QCOMPARE(spy.at(1).at(2).toBool(), true);
  }

  // ---- in the window ----

  void windowMenuShortcutAndToolbar() {
    login(owner_);
    auto w = open();
    MilestonesView* m = w->milestones();
    QCOMPARE(w->mainStack()->currentWidget(), m);
    QCOMPARE(shortcutFor(QStringLiteral("actionMilestones")), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    QVERIFY(w->toolBar()->actions().contains(w->action(QStringLiteral("actionMilestones"))));
    QVERIFY(!w->action(QStringLiteral("actionMilestones"))->icon().isNull());
    // Kitchen remodel: Design sign-off (late), Cabinets fitted, Handover.
    QCOMPARE(m->table()->rowCount(), 3);
    QCOMPARE(m->rows().at(0).title, QStringLiteral("Design sign-off"));
    QCOMPARE(m->rows().at(0).wbs, QStringLiteral("1.2"));
    QVERIFY(m->rows().at(0).late);
    QVERIFY(m->rows().at(2).afterDeadline);
    // Back to the details page and in again with the key.
    w->action(QStringLiteral("actionProjectDetails"))->trigger();
    QVERIFY(w->mainStack()->currentWidget() != m);
    QTest::keyClick(w.get(), Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(w->mainStack()->currentWidget(), m);
  }

  void windowAllProjects() {
    login(owner_);
    auto w = open();
    MilestonesView* m = w->milestones();
    m->allProjectsBox()->setChecked(true);
    QVERIFY(waitFor([&] { return m->rows().size() == 4; }));
    QVERIFY(rowOf(m->rows(), ids_.value(QStringLiteral("Plants ordered"))));
    QVERIFY(!m->table()->isColumnHidden(C::Project));
    // Opening the garden's milestone switches project.
    QVERIFY(m->selectTask(ids_.value(QStringLiteral("Plants ordered"))));
    QTest::keyClick(m->table(), Qt::Key_Return);
    QVERIFY(waitFor([&] { return w->currentProjectId() == garden_; }));
    QCOMPARE(w->mainStack()->currentWidget(), w->outline());
    QCOMPARE(w->outline()->currentTaskId(), ids_.value(QStringLiteral("Plants ordered")));
  }

  void windowJumpsToOutlineAndGantt() {
    login(owner_);
    auto w = open();
    MilestonesView* m = w->milestones();
    const QString handover = ids_.value(QStringLiteral("Handover"));
    // A filter that hides the milestone is cleared; Enter opens the outline.
    w->outline()->setStatusFilter(QStringLiteral("done"));
    QVERIFY(m->selectTask(handover));
    QTest::keyClick(m->table(), Qt::Key_Return);
    QCOMPARE(w->mainStack()->currentWidget(), w->outline());
    QCOMPARE(w->outline()->currentTaskId(), handover);
    QCOMPARE(w->viewTaskId(), handover);
    // Show in Gantt.
    w->action(QStringLiteral("actionMilestones"))->trigger();
    QVERIFY(m->selectTask(ids_.value(QStringLiteral("Cabinets fitted"))));
    m->ganttButton()->click();
    QCOMPARE(w->mainStack()->currentWidget(), w->gantt());
    QCOMPARE(w->gantt()->currentTaskId(), ids_.value(QStringLiteral("Cabinets fitted")));
  }

  void windowFollowsRefreshes() {
    login(owner_);
    auto w = open();
    MilestonesView* m = w->milestones();
    // Someone else marks Design sign-off done: no longer late.
    Task t = store_->task(ids_.value(QStringLiteral("Design sign-off")), true).value();
    t.status = QStringLiteral("done");
    t.percentComplete = 100;
    t.actualStart = t.start;
    t.actualFinish = t.finish;
    QVERIFY(store_->updateTask(t, t.rev, kOther).ok());
    add(kitchen_, QStringLiteral("Final clean"), {}, milestone("2026-11-25"));
    w->session()->refresh();
    QVERIFY(waitFor([&] { return m->rows().size() == 4; }));
    const MilestoneRow* r = rowOf(m->rows(), ids_.value(QStringLiteral("Design sign-off")));
    QVERIFY(r && r->done && !r->late);
    QVERIFY(!m->note()->text().contains(QStringLiteral("late")));
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
    w->resize(1280, 520);
    // The table wants the width: no inspector here.
    w->action(QStringLiteral("actionToggleInspector"))->trigger();
    w->milestones()->allProjectsBox()->setChecked(true);
    QVERIFY(waitFor([&] { return w->milestones()->rows().size() == 4; }));
    w->milestones()->selectTask(ids_.value(QStringLiteral("Handover")));
    w->setTheme(QStringLiteral("light"));
    save(w.get(), QStringLiteral("milestones-light.png"));
    w->setTheme(QStringLiteral("dark"));
    save(w.get(), QStringLiteral("milestones-dark.png"));
    w->setTheme(QStringLiteral("system"));
  }
};

QTEST_MAIN(TestMilestones)
#include "tst_milestones.moc"
