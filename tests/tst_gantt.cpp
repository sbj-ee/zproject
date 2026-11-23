// The read-only Gantt (SPEC.md §8.2 v1 subset, §17, §18; plan PR 8, §3.4).
//  - GanttView on its own, from JSON: bar kinds (auto solid, manual
//    outlined, summary brackets, milestones), the date axis, timescales and
//    their keys, today, unscheduled tasks, deadlines, accessible bars, and
//    a 2,000-row paint benchmark (only the rows in view are painted);
//  - the Gantt in the window, offscreen, against the real zprojectd API on
//    loopback with an in-memory store: View → Gantt (Ctrl+G) and the
//    toolbar, the inspector following the selection, Enter, read-only rows,
//    themes and refreshes.
// With ZPROJECT_SCREENSHOT_DIR set, screenshots() renders
// docs/screenshots/gantt-*.png from the same example data.
#include <QAccessible>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>

#include "app/GanttView.hpp"
#include "app/MainWindow.hpp"
#include "app/OutlineModel.hpp"
#include "app/OutlineView.hpp"
#include "app/TaskInspector.hpp"
#include "app/Theme.hpp"
#include "client/TokenStore.hpp"
#include "core/Version.hpp"
#include "server/Api.hpp"
#include "server/http/HttpServer.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::app;
using Health = AppSession::Health;
using M = OutlineModel;
using Scale = GanttChart::Scale;

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

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

QJsonObject jt(const char* id, const char* title, const char* parent, int order, const char* start = nullptr,
               const char* finish = nullptr, const char* mode = "auto", int pct = 0, const char* type = "task") {
  QJsonObject o{{QStringLiteral("id"), QLatin1String(id)},
                {QStringLiteral("title"), QLatin1String(title)},
                {QStringLiteral("parent"), parent ? QJsonValue(QLatin1String(parent)) : QJsonValue()},
                {QStringLiteral("order"), order},
                {QStringLiteral("status"), pct >= 100 ? QStringLiteral("done") : pct > 0 ? QStringLiteral("in_progress") : QStringLiteral("todo")},
                {QStringLiteral("percentComplete"), pct},
                {QStringLiteral("type"), QLatin1String(type)},
                {QStringLiteral("mode"), QLatin1String(mode)},
                {QStringLiteral("duration"), QLatin1String(type) == QLatin1String("milestone") ? 0 : 3},
                {QStringLiteral("priority"), 2}};
  if (start) o[QStringLiteral("start")] = QLatin1String(start);
  if (finish) o[QStringLiteral("finish")] = QLatin1String(finish);
  return o;
}

QList<QJsonObject> sample() {
  QJsonObject drawings = jt("a2", "Drawings", "a", 2, "2026-09-09", "2026-09-11", "manual", 40);
  QJsonObject permits = jt("p", "Permits", nullptr, 2, "2026-09-14", "2026-09-18", "manual");
  permits[QStringLiteral("deadline")] = QStringLiteral("2026-10-15");
  QJsonObject handover = jt("h", "Handover", nullptr, 5, "2026-12-18", "2026-12-18", "manual", 0, "milestone");
  handover[QStringLiteral("deadline")] = QStringLiteral("2026-12-11");  // missed
  return {jt("a", "Design", nullptr, 1),
          jt("a1", "Survey", "a", 1, "2026-09-07", "2026-09-08", "manual", 100),
          drawings,
          permits,
          jt("b", "Build", nullptr, 3),
          jt("c1", "Cabinets", "b", 1, "2026-10-19", "2026-10-22", "auto", 25),
          jt("c2", "Counters", "b", 2, "2026-10-23", "2026-10-26"),
          jt("s", "Snagging list", nullptr, 4),  // auto, no dates yet
          handover};
}

GanttChart::Bar barOf(const GanttChart& c, const QString& id) {
  for (const GanttChart::Bar& b : c.visibleBars()) {
    if (b.id == id) return b;
  }
  return {};
}

}  // namespace

class TestGantt : public QObject {
  Q_OBJECT

  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<store::SqliteStore> store_;
  std::unique_ptr<server::Api> api_;
  std::unique_ptr<server::http::Server> http_;
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
  void updateOther(const QString& title, const std::function<void(Task&)>& set) {
    Task t = task(title);
    set(t);
    QVERIFY(store_->updateTask(t, t.rev, kOther).ok());
  }
  static bool waitHealth(MainWindow& w, Health h, int ms = 5000) {
    return QTest::qWaitFor([&] { return w.session() && w.session()->health() == h; }, ms);
  }
  static bool waitFor(const std::function<bool()>& f, int ms = 5000) { return QTest::qWaitFor(f, ms); }
  // A window, online, on the Kitchen remodel Gantt.
  std::unique_ptr<MainWindow> open() {
    auto w = std::make_unique<MainWindow>(options());
    w->resize(1280, 640);
    w->show();
    if (!QTest::qWaitForWindowExposed(w.get())) qWarning("not exposed");
    if (!waitHealth(*w, Health::Online) || !waitFor([&] { return w->projectList()->count() == 1; })) {
      qWarning("window did not come online");
    }
    w->action(QStringLiteral("actionGantt"))->trigger();
    if (!waitFor([&] { return w->gantt()->model()->taskCount() == ids_.size(); })) qWarning("gantt not filled");
    return w;
  }
  // A Gantt on its own, shown, with the sample tasks.
  static std::unique_ptr<GanttView> standalone(const QList<QJsonObject>& tasks = sample()) {
    auto v = std::make_unique<GanttView>();
    v->resize(1100, 420);
    v->setToday(kToday);
    v->setProject(QStringLiteral("k"), QStringLiteral("Kitchen remodel"));
    v->show();
    if (!QTest::qWaitForWindowExposed(v.get())) qWarning("not exposed");
    v->reload(tasks, {});
    QCoreApplication::processEvents();
    return v;
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
    p.start = QStringLiteral("2026-09-07");
    p.finish = QStringLiteral("2026-12-18");
    kitchen_ = store_->createProject(p, kOther).value().id;
    ids_.clear();
    auto dated = [](const char* mode, const char* start, const char* finish, double days) {
      return [=](Task& t) {
        t.mode = QLatin1String(mode);
        t.start = QLatin1String(start);
        t.finish = QLatin1String(finish);
        t.duration = days;
      };
    };
    add(QStringLiteral("Design"), {});
    add(QStringLiteral("Survey"), QStringLiteral("Design"), [&](Task& t) {
      dated("manual", "2026-09-07", "2026-09-08", 2)(t);
      t.percentComplete = 100;
      t.status = QStringLiteral("done");
      t.actualStart = QStringLiteral("2026-09-07");
      t.actualFinish = QStringLiteral("2026-09-08");
    });
    add(QStringLiteral("Drawings"), QStringLiteral("Design"), [&](Task& t) {
      dated("manual", "2026-09-09", "2026-09-15", 5)(t);
      t.percentComplete = 40;
      t.status = QStringLiteral("in_progress");
      t.actualStart = QStringLiteral("2026-09-09");
    });
    add(QStringLiteral("Permits"), {}, [&](Task& t) {
      dated("manual", "2026-09-16", "2026-10-06", 15)(t);
      t.deadline = QStringLiteral("2026-10-15");
      t.priority = 1;
    });
    add(QStringLiteral("Build"), {});
    add(QStringLiteral("Cabinets"), QStringLiteral("Build"), dated("auto", "2026-10-19", "2026-10-28", 8));
    add(QStringLiteral("Counters"), QStringLiteral("Build"), dated("auto", "2026-10-29", "2026-11-04", 5));
    add(QStringLiteral("Lighting"), QStringLiteral("Build"), dated("manual", "2026-11-02", "2026-11-13", 10));
    add(QStringLiteral("Snagging list"), {});  // auto, not scheduled yet
    add(QStringLiteral("Handover"), {}, [&](Task& t) {
      t.type = QStringLiteral("milestone");
      t.duration = 0;
      t.mode = QStringLiteral("manual");
      t.start = QStringLiteral("2026-11-20");
      t.finish = QStringLiteral("2026-11-20");
      t.deadline = QStringLiteral("2026-11-18");  // missed
    });
  }
  void cleanup() {
    http_.reset();
    api_.reset();
    store_.reset();
    dir_.reset();
  }

  // ---- on its own ----

  void barsFromStoredDates() {
    auto v = standalone();
    GanttChart* c = v->chart();
    QTreeView* t = v->table();
    // The table: WBS, title, duration, start, finish, % complete.
    QVERIFY(!t->isColumnHidden(M::Wbs));
    QVERIFY(!t->isColumnHidden(M::Start));
    QVERIFY(t->isColumnHidden(M::Status));
    QVERIFY(t->isColumnHidden(M::Priority));
    QCOMPARE(t->editTriggers(), QAbstractItemView::NoEditTriggers);
    QVERIFY(!v->model()->editable());
    // Kinds.
    QVERIFY(barOf(*c, QStringLiteral("a")).summary);
    QVERIFY(barOf(*c, QStringLiteral("h")).milestone);
    QVERIFY(barOf(*c, QStringLiteral("a2")).manual);
    QVERIFY(!barOf(*c, QStringLiteral("c1")).manual);
    // A summary's bracket spans its subtasks (7–11 Sep).
    const QRect design = barOf(*c, QStringLiteral("a")).rect;
    QCOMPARE(design.left(), static_cast<int>(c->xOf(QDate(2026, 9, 7))));
    QCOMPARE(design.right() + 1, static_cast<int>(c->xOf(QDate(2026, 9, 12))));
    // Bars sit on their table rows.
    const QRect row = barOf(*c, QStringLiteral("a2")).row;
    const QRect cell = t->visualRect(v->model()->indexOf(QStringLiteral("a2"), M::Title));
    QCOMPARE(c->mapTo(v.get(), row.topLeft()).y(), t->viewport()->mapTo(v.get(), cell.topLeft()).y());
    // The bar is 3 days long at the week scale's 12 px a day.
    QCOMPARE(c->scale(), Scale::Week);
    QVERIFY(qAbs(barOf(*c, QStringLiteral("a2")).rect.width() - 36) <= 2);
    // Unscheduled: no bar, "unscheduled" in the table, counted in the note.
    QVERIFY(barOf(*c, QStringLiteral("s")).rect.isNull());
    QCOMPARE(v->unscheduledCount(), 1);
    QVERIFY(v->note()->text().contains(QStringLiteral("1 task has no dates")));
    QVERIFY(t->itemDelegateForColumn(M::Start) != nullptr);
    // Deadlines: Handover's finish is after its deadline.
    QVERIFY(barOf(*c, QStringLiteral("h")).late);
    QVERIFY(!barOf(*c, QStringLiteral("p")).late);
    // The range starts on a Monday a week before the first date.
    QCOMPARE(c->rangeStart(), QDate(2026, 8, 31));
    QVERIFY(c->rangeEnd() >= QDate(2026, 12, 18));
  }

  void unscheduledIsShownInTheTable() {
    auto v = standalone();
    QTreeView* t = v->table();
    const QModelIndex start = v->model()->indexOf(QStringLiteral("s"), M::Start);
    QVERIFY(start.data().toString().isEmpty());
    // Rendered text: grab the cell and compare with a row that has a date.
    const QRect cell = t->visualRect(start);
    QVERIFY(cell.isValid());
    const QImage img = t->viewport()->grab(cell).toImage();
    bool inked = false;
    for (int x = 0; x < img.width() && !inked; ++x) {
      for (int y = 0; y < img.height(); ++y) {
        if (img.pixelColor(x, y) != img.pixelColor(0, 0)) {
          inked = true;
          break;
        }
      }
    }
    QVERIFY2(inked, "the unscheduled cell should say so");
  }

  void timescalesAndKeys() {
    auto v = standalone();
    GanttChart* c = v->chart();
    QComboBox* box = v->scaleBox();
    QCOMPARE(box->count(), 4);
    QCOMPARE(box->currentText(), QStringLiteral("Week"));
    box->setCurrentIndex(0);
    QCOMPARE(c->scale(), Scale::Day);
    QCOMPARE(c->pxPerDay(), 28.0);
    box->setCurrentIndex(3);
    QCOMPARE(c->scale(), Scale::Quarter);
    // Ctrl+Shift+= / − step through them (the table keeps the focus).
    v->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(v.get()));
    v->table()->setFocus();
    QTest::keyClick(v->table(), Qt::Key_Equal, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(c->scale(), Scale::Month);
    QCOMPARE(box->currentText(), QStringLiteral("Month"));
    QTest::keyClick(v->table(), Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(c->scale(), Scale::Week);
    QTest::keyClick(v->table(), Qt::Key_Minus, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(c->scale(), Scale::Month);
    // Ctrl+wheel over the timeline.
    QWheelEvent in(QPointF(100, 100), c->mapToGlobal(QPointF(100, 100)), {}, QPoint(0, 120), Qt::NoButton,
                   Qt::ControlModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(c, &in);
    QCOMPARE(c->scale(), Scale::Week);
    QCOMPARE(box->currentText(), QStringLiteral("Week"));
    // T: today a third of the way in.
    c->horizontalBar()->setValue(c->horizontalBar()->maximum());
    QTest::keyClick(v->table(), Qt::Key_T);
    const double x = c->xOf(kToday);
    QVERIFY2(qAbs(x - c->width() / 3.0) <= c->pxPerDay() + 1, qPrintable(QString::number(x)));
    c->horizontalBar()->setValue(0);
    v->todayButton()->click();
    QVERIFY(qAbs(c->xOf(kToday) - c->width() / 3.0) <= c->pxPerDay() + 1);
    // A change of scale keeps the middle date in the middle.
    const QDate mid = c->dateAt(c->width() / 2);
    c->setScale(Scale::Day);
    QVERIFY(qAbs(c->dateAt(c->width() / 2).daysTo(mid)) <= 1);
  }

  void arrowsAndEnter() {
    auto v = standalone();
    QSignalSpy current(v.get(), &GanttView::currentTaskChanged);
    QSignalSpy open(v.get(), &GanttView::openTask);
    v->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(v.get()));
    QVERIFY(v->selectTask(QStringLiteral("a")));
    v->table()->setFocus();
    QTest::keyClick(v->table(), Qt::Key_Down);
    QCOMPARE(v->currentTaskId(), QStringLiteral("a1"));
    QTest::keyClick(v->table(), Qt::Key_Down);
    QCOMPARE(v->currentTaskId(), QStringLiteral("a2"));
    QVERIFY(current.count() >= 2);
    QTest::keyClick(v->table(), Qt::Key_Return);
    QCOMPARE(open.count(), 1);
    QCOMPARE(open.first().first().toString(), QStringLiteral("a2"));
    // A click on a bar's row selects it; a double-click opens it.
    GanttChart* c = v->chart();
    const QRect r = barOf(*c, QStringLiteral("c1")).rect;
    QTest::mouseClick(c, Qt::LeftButton, {}, r.center());
    QCOMPARE(v->currentTaskId(), QStringLiteral("c1"));
    QTest::mouseDClick(c, Qt::LeftButton, {}, r.center());
    QCOMPARE(open.last().first().toString(), QStringLiteral("c1"));
    // Read-only: a double-click in the table never starts an edit.
    const QModelIndex title = v->model()->indexOf(QStringLiteral("c1"), M::Title);
    QTest::mouseDClick(v->table()->viewport(), Qt::LeftButton, {}, v->table()->visualRect(title).center());
    QVERIFY(v->table()->findChild<QLineEdit*>() == nullptr || !v->table()->findChild<QLineEdit*>()->isVisible());
  }

  void accessibleBars() {
    auto v = standalone();
    GanttChart* c = v->chart();
    QCOMPARE(c->barText(QStringLiteral("a2")), QStringLiteral("Task 1.2 Drawings, 3 days, 9–11 Sep, 40% complete, manual dates"));
    QCOMPARE(c->barName(QStringLiteral("a")), QStringLiteral("Summary 1 Design"));
    QCOMPARE(c->barDescription(QStringLiteral("a")), QStringLiteral("2 subtasks, 7–11 Sep, 0% complete (rolled up)"));
    QCOMPARE(c->barDescription(QStringLiteral("p")),
             QStringLiteral("3 days, 14–18 Sep, 0% complete, manual dates, deadline 15 Oct"));
    QCOMPARE(c->barText(QStringLiteral("h")),
             QStringLiteral("Milestone 5 Handover, 18 Dec, manual dates, deadline 11 Dec, missed"));
    QCOMPARE(c->barDescription(QStringLiteral("s")), QStringLiteral("3 days, unscheduled, 0% complete"));
    // The timeline is a list of its bars for screen readers.
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(c);
    QVERIFY(iface);
    QCOMPARE(iface->role(), QAccessible::List);
    QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("Gantt timeline"));
    QCOMPARE(iface->childCount(), 9);
    QAccessibleInterface* drawings = nullptr;
    for (int i = 0; i < iface->childCount(); ++i) {
      QAccessibleInterface* ch = iface->child(i);
      QVERIFY(ch);
      QVERIFY(ch->isValid());
      QCOMPARE(iface->indexOfChild(ch), i);
      QCOMPARE(ch->parent(), iface);
      if (ch->text(QAccessible::Name) == QStringLiteral("Task 1.2 Drawings")) drawings = ch;
    }
    QVERIFY(drawings);
    QCOMPARE(drawings->text(QAccessible::Description), QStringLiteral("3 days, 9–11 Sep, 40% complete, manual dates"));
    QCOMPARE(drawings->role(), QAccessible::ListItem);
    QVERIFY(drawings->rect().isValid());
    // The same child comes back for the same bar.
    QCOMPARE(iface->child(2), iface->child(2));
    const QPoint at = drawings->rect().center();
    QCOMPARE(iface->childAt(at.x(), at.y()), drawings);
    // A reload drops the old children.
    v->reload(sample(), {});
    QVERIFY(c->accessibleIds().isEmpty());
    QVERIFY(iface->child(0));
  }

  void themes() {
    auto v = standalone();
    GanttChart* c = v->chart();
    const QPoint inRows(5, c->rowsTop() + 5);
    auto pixel = [&] { return c->grab().toImage().pixelColor(inRows); };
    v->setColors(Theme::colors(QStringLiteral("light")));
    const QColor light = pixel();
    v->setColors(Theme::colors(QStringLiteral("dark")));
    const QColor dark = pixel();
    v->setColors(Theme::colors(QStringLiteral("high-contrast")));
    const QColor hc = pixel();
    QVERIFY(light.lightness() > dark.lightness());
    QVERIFY(hc != light);
    // The auto bar is drawn in the theme's accent.
    v->setColors(Theme::colors(QStringLiteral("light")));
    const QRect r = barOf(*c, QStringLiteral("c2")).rect;
    QCOMPARE(c->grab().toImage().pixelColor(r.left() + 3, r.top() + 2), Theme::colors(QStringLiteral("light")).accent);
  }

  // 2,000 rows (SPEC.md §17: 60 fps scroll and zoom): only the rows in view
  // are laid out and painted, so a frame costs the same at 20 or 2,000 rows.
  void benchmark2000Rows() {
    QList<QJsonObject> tasks;
    QDate d(2026, 1, 5);
    for (int s = 0; s < 100; ++s) {
      const QByteArray sid = "s" + QByteArray::number(s);
      tasks << jt(sid.constData(), "Phase", nullptr, s + 1);
      for (int k = 0; k < 19; ++k) {
        const QByteArray id = sid + "-" + QByteArray::number(k);
        const QByteArray a = d.toString(Qt::ISODate).toLatin1();
        const QByteArray b = d.addDays(3).toString(Qt::ISODate).toLatin1();
        tasks << jt(id.constData(), "Work item", sid.constData(), k + 1, a.constData(), b.constData(),
                    k % 2 ? "manual" : "auto", (k * 7) % 100);
        d = d.addDays(1);
      }
    }
    QCOMPARE(tasks.size(), 2000);
    QElapsedTimer load;
    load.start();
    auto v = standalone(tasks);
    const qint64 loadMs = load.elapsed();
    GanttChart* c = v->chart();
    QCOMPARE(v->model()->taskCount(), 2000);
    c->repaint();
    const int rows = c->lastPaintedRows();
    const int fit = c->height() / std::max(1, v->table()->sizeHintForRow(0)) + 2;
    QVERIFY2(rows > 0 && rows <= fit, qPrintable(QStringLiteral("%1 rows painted, %2 fit").arg(rows).arg(fit)));
    // Scroll through the whole list and across the timeline, a frame a step.
    QScrollBar* vbar = v->table()->verticalScrollBar();
    QScrollBar* hbar = c->horizontalBar();
    constexpr int kFrames = 120;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < kFrames; ++i) {
      vbar->setValue(vbar->maximum() * i / kFrames);
      hbar->setValue(hbar->maximum() * i / kFrames);
      if (i % 30 == 0) c->setScale(i % 60 == 0 ? Scale::Day : Scale::Week);  // zoom too
      c->repaint();
      QVERIFY(c->lastPaintedRows() <= fit);
    }
    const double perFrame = static_cast<double>(t.elapsed()) / kFrames;
    qInfo("2,000 rows: load %lld ms, %.2f ms a frame (%d rows painted)", loadMs, perFrame, rows);
    const double budget = kSanitized ? 100.0 : 16.0;  // 60 fps
    QVERIFY2(perFrame < budget, qPrintable(QStringLiteral("%1 ms a frame").arg(perFrame)));
  }

  // ---- in the window ----

  void windowViewMenuToolbarAndInspector() {
    login(owner_);
    auto w = open();
    GanttView* g = w->gantt();
    QCOMPARE(w->mainStack()->currentWidget(), g);
    QCOMPARE(g->title()->text(), QStringLiteral("Kitchen remodel — Gantt"));
    QCOMPARE(g->unscheduledCount(), 1);
    // View → Gantt is Ctrl+G and on the toolbar, after Outline.
    QAction* gantt = w->action(QStringLiteral("actionGantt"));
    QCOMPARE(gantt->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_G));
    QVERIFY(w->toolBar()->actions().contains(gantt));
    QVERIFY(!gantt->icon().isNull());
    const QList<QAction*> bar = w->toolBar()->actions();
    QCOMPARE(bar.indexOf(gantt), bar.indexOf(w->action(QStringLiteral("actionOutline"))) + 1);
    w->action(QStringLiteral("actionOutline"))->trigger();
    QCOMPARE(w->mainStack()->currentWidget(), w->outline());
    w->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(w.get()));
    QTest::keyClick(w.get(), Qt::Key_G, Qt::ControlModifier);
    QCOMPARE(w->mainStack()->currentWidget(), g);
    // Selecting a row shows the task in the inspector.
    QVERIFY(g->selectTask(ids_.value(QStringLiteral("Cabinets"))));
    TaskInspector* i = w->taskInspector();
    QVERIFY(waitFor([&] { return i->isVisible() && i->taskId() == ids_.value(QStringLiteral("Cabinets")); }));
    // Enter opens it on Details, title focused.
    w->action(QStringLiteral("actionToggleInspector"))->trigger();  // hide it first
    QVERIFY(!i->isVisible());
    g->table()->setFocus();
    QTest::keyClick(g->table(), Qt::Key_Return);
    QVERIFY(waitFor([&] { return i->isVisible(); }));
    QCOMPARE(i->tabs()->currentIndex(), 0);
    QVERIFY(waitFor([&] { return i->field(QStringLiteral("title"))->hasFocus(); }));
    // Structural outline actions stay with the outline.
    QVERIFY(!w->action(QStringLiteral("actionIndentTask"))->isEnabled());
    QVERIFY(w->action(QStringLiteral("actionEditTask"))->isEnabled());
  }

  // The title bar names the build's version and the project (SPEC.md §8.1).
  void windowTitleShowsVersionAndProject() {
    QCOMPARE(MainWindow::titleFor({}), QStringLiteral("zproject %1[*]").arg(appVersion()));
    QVERIFY(!appVersion().isEmpty());
    QCOMPARE(appVersion(), QStringLiteral(ZPROJECT_TEST_VERSION));
    login(owner_);
    auto w = open();
    QCOMPARE(w->windowTitle(), QStringLiteral("zproject %1 — Kitchen remodel[*]").arg(appVersion()));
    Project shed;
    shed.name = QStringLiteral("Garden shed");
    const QString shedId = store_->createProject(shed, kOther).value().id;
    w->session()->refresh();
    QVERIFY(waitFor([&] { return w->projectList()->count() == 2; }));
    w->selectProject(shedId);
    QVERIFY(waitFor([&] { return w->windowTitle().endsWith(QStringLiteral("— Garden shed[*]")); }));
    QVERIFY(w->windowTitle().startsWith(QStringLiteral("zproject %1 ").arg(appVersion())));
    // Unsaved task edits mark the title.
    w->selectProject(kitchen_);
    w->action(QStringLiteral("actionGantt"))->trigger();
    QVERIFY(waitFor([&] { return w->windowTitle().endsWith(QStringLiteral("— Kitchen remodel[*]")); }));
    QVERIFY(w->gantt()->selectTask(ids_.value(QStringLiteral("Cabinets"))));
    TaskInspector* i = w->taskInspector();
    QVERIFY(waitFor([&] { return i->taskId() == ids_.value(QStringLiteral("Cabinets")) && !i->isLoading(); }));
    QVERIFY(!w->isWindowModified());
    i->setText(QStringLiteral("title"), QStringLiteral("Cabinets (oak)"));
    QVERIFY(w->isWindowModified());
    i->revert();
    QVERIFY(!w->isWindowModified());
  }

  void windowFollowsRefreshesAndThemes() {
    login(owner_);
    auto w = open();
    GanttView* g = w->gantt();
    GanttChart* c = g->chart();
    QCOMPARE(c->today(), kToday);
    const QString cabinets = ids_.value(QStringLiteral("Cabinets"));
    const int before = barOf(*c, cabinets).rect.left();
    updateOther(QStringLiteral("Cabinets"), [](Task& t) {
      t.start = QStringLiteral("2026-10-26");
      t.finish = QStringLiteral("2026-11-04");
    });
    w->session()->refresh();
    QVERIFY(waitFor([&] { return barOf(*c, cabinets).rect.left() > before; }));
    QCOMPARE(barOf(*c, cabinets).rect.left() - before, static_cast<int>(7 * c->pxPerDay()));
    // Themes reach the timeline.
    const QPoint inRows(5, c->rowsTop() + 5);
    w->setTheme(QStringLiteral("light"));
    const QColor light = c->grab().toImage().pixelColor(inRows);
    w->setTheme(QStringLiteral("dark"));
    const QColor dark = c->grab().toImage().pixelColor(inRows);
    QVERIFY(light.lightness() > dark.lightness());
    w->setTheme(QStringLiteral("system"));
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
    GanttView* g = w->gantt();
    // The timeline wants the width: no sidebar or inspector here.
    w->action(QStringLiteral("actionToggleSidebar"))->trigger();
    w->action(QStringLiteral("actionToggleInspector"))->trigger();
    // Wide enough for the whole project at week scale, milestone included.
    w->resize(1600, 560);
    QTest::qWait(100);
    QVERIFY(g->selectTask(ids_.value(QStringLiteral("Drawings"))));
    QCoreApplication::processEvents();
    QTest::qWait(50);
    g->chart()->scrollToDate(QDate(2026, 10, 1));
    w->setTheme(QStringLiteral("light"));
    save(w.get(), QStringLiteral("gantt-light.png"));
    w->setTheme(QStringLiteral("dark"));
    save(w.get(), QStringLiteral("gantt-dark.png"));
    w->setTheme(QStringLiteral("system"));
  }
};

QTEST_MAIN(TestGantt)
#include "tst_gantt.moc"
