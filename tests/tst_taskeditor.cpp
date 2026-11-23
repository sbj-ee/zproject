// The task editor in the inspector (SPEC.md §8.1, §8.3; plan PR 7, §3.3).
// client::validateTaskEdit on its own, then the editor in the window,
// offscreen, against the real zprojectd API on loopback with an in-memory
// store: the server's copy and its rev, locked roll-ups on summaries, local
// checks that send nothing, the ripple preview, plain saves, 412 Keep mine /
// Take theirs, the summary race, History, unsaved-changes questions, a
// read-only token and offline. With ZPROJECT_SCREENSHOT_DIR set,
// screenshots() renders docs/screenshots/taskeditor-*.png from the same
// example data. Tokens are made per run.
#include <QAbstractItemModelTester>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolButton>
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
#include "client/EditFlow.hpp"
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

}  // namespace

class TestTaskEditor : public QObject {
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

  // ---- inspector helpers ----
  static TaskInspector* insp(MainWindow& w) { return w.taskInspector(); }
  // Selects `title` and waits for the server's copy in the editor.
  bool inspect(MainWindow& w, const QString& title) {
    if (!select(w, title)) return false;
    return waitFor([&] {
      return w.taskInspector()->taskId() == ids_.value(title) && !w.taskInspector()->isLoading() &&
             w.taskInspector()->base().value(QStringLiteral("rev")).toInteger() > 0;
    });
  }
  template <typename T>
  static T* fieldOf(MainWindow& w, const char* name) {
    return qobject_cast<T*>(w.taskInspector()->field(QString::fromLatin1(name)));
  }
  static bool readOnly(QWidget* f) {
    if (auto* l = qobject_cast<QLineEdit*>(f)) return l->isReadOnly();
    if (auto* s = qobject_cast<QAbstractSpinBox*>(f)) return s->isReadOnly();
    return !f->isEnabled();
  }
  static bool shown(const QWidget* l) { return l != nullptr && l->isVisible(); }
  static void pick(QComboBox* c, const char* data) { c->setCurrentIndex(c->findData(QString::fromLatin1(data))); }
  void updateOther(const QString& title, const std::function<void(Task&)>& set) {
    Task t = task(title);
    set(t);
    QVERIFY(store_->updateTask(t, t.rev, kOther).ok());
  }

 private slots:
  void init() {
    if (QByteArray(QTest::currentTestFunction()) == "validationRules") return;
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

  void initTestCase() {
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  }

  // client::validateTaskEdit on its own: core/Model's rules on the edited
  // copy, plus the summary rules.
  void validationRules() {
    const QJsonObject base{{QStringLiteral("id"), QStringLiteral("t1")},     {QStringLiteral("title"), QStringLiteral("Drawings")},
                           {QStringLiteral("type"), QStringLiteral("task")}, {QStringLiteral("duration"), 3},
                           {QStringLiteral("status"), QStringLiteral("todo")}, {QStringLiteral("percentComplete"), 0},
                           {QStringLiteral("constraint"), QStringLiteral("ASAP")}, {QStringLiteral("rev"), 4}};
    auto fields = [](const QList<FieldError>& errs) {
      QStringList out;
      for (const FieldError& e : errs) out << e.field;
      return out;
    };
    QVERIFY(client::validateTaskEdit(base, {{QStringLiteral("title"), QStringLiteral("Drawings v2")}}, false).isEmpty());
    QCOMPARE(fields(client::validateTaskEdit(base, {{QStringLiteral("title"), QStringLiteral("  ")}}, false)),
             QStringList{QStringLiteral("title")});
    QCOMPARE(fields(client::validateTaskEdit(base, {{QStringLiteral("type"), QStringLiteral("milestone")}}, false)),
             QStringList{QStringLiteral("duration")});
    QCOMPARE(fields(client::validateTaskEdit(base, {{QStringLiteral("percentComplete"), 30}}, false)),
             QStringList{QStringLiteral("actualStart")});
    QCOMPARE(fields(client::validateTaskEdit(
                 base, {{QStringLiteral("actualStart"), QStringLiteral("2026-09-09")}, {QStringLiteral("actualFinish"), QStringLiteral("2026-09-11")}},
                 false)),
             QStringList{QStringLiteral("percentComplete")});
    QCOMPARE(fields(client::validateTaskEdit(base, {{QStringLiteral("constraint"), QStringLiteral("SNET")}}, false)),
             QStringList{QStringLiteral("constraintDate")});
    QCOMPARE(fields(client::validateTaskEdit(base, {{QStringLiteral("deadline"), QStringLiteral("2026-13-01")}}, false)),
             QStringList{QStringLiteral("deadline")});
    QCOMPARE(fields(client::validateTaskEdit(base, {{QStringLiteral("start"), QStringLiteral("2026-09-11")}, {QStringLiteral("finish"), QStringLiteral("2026-09-09")}}, false)),
             QStringList{QStringLiteral("finish")});
    // A summary: its rolled-up fields and "milestone" are refused.
    QCOMPARE(fields(client::validateTaskEdit(base, {{QStringLiteral("status"), QStringLiteral("done")}}, true)),
             QStringList{QStringLiteral("status")});
    QVERIFY(fields(client::validateTaskEdit(base, {{QStringLiteral("type"), QStringLiteral("milestone")}, {QStringLiteral("duration"), 0}}, true))
                .contains(QStringLiteral("type")));
    QVERIFY(client::validateTaskEdit(base, {{QStringLiteral("priority"), 1}}, true).isEmpty());
    QCOMPARE(taskFieldLabel(QStringLiteral("finish")), QStringLiteral("Finish"));
    QCOMPARE(fieldLabel(QStringLiteral("finish")), QStringLiteral("Target finish"));
    QCOMPARE(taskFieldLabel(QStringLiteral("constraintDate")), QStringLiteral("Constraint date"));
  }

  void showsTheServersCopy() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Drawings")));
    TaskInspector* i = insp(*w);
    QVERIFY(i->isVisible());
    QCOMPARE(w->findChild<QStackedWidget*>(QStringLiteral("inspectorStack"))->currentWidget(), i);
    QCOMPARE(i->header()->text(), QStringLiteral("1.2 Drawings"));
    QCOMPARE(i->text(QStringLiteral("title")), QStringLiteral("Drawings"));
    QCOMPARE(fieldOf<QComboBox>(*w, "status")->currentData().toString(), QStringLiteral("in_progress"));
    QCOMPARE(fieldOf<QSpinBox>(*w, "percentComplete")->value(), 40);
    QCOMPARE(fieldOf<QComboBox>(*w, "mode")->currentData().toString(), QStringLiteral("manual"));
    QCOMPARE(i->text(QStringLiteral("start")), QStringLiteral("2026-09-09"));
    QCOMPARE(i->text(QStringLiteral("actualStart")), QStringLiteral("2026-09-09"));
    QCOMPARE(fieldOf<QDoubleSpinBox>(*w, "duration")->value(), 3.0);
    QCOMPARE(i->base().value(QStringLiteral("rev")).toInteger(), task(QStringLiteral("Drawings")).rev);
    QVERIFY(!i->isDirty());
    QVERIFY(!i->saveButton()->isEnabled());
    QVERIFY(!readOnly(fieldOf<QSpinBox>(*w, "percentComplete")));
    QVERIFY(!i->rollupNote()->isVisible());
    QCOMPARE(i->tabs()->count(), 3);
    QCOMPARE(i->tabs()->tabText(1), QStringLiteral("Schedule"));
    // A summary: its rolled-up fields are locked and say why.
    QVERIFY(inspect(*w, QStringLiteral("Design")));
    QVERIFY(i->rollupNote()->isVisible());
    QVERIFY(i->rollupNote()->text().contains(QStringLiteral("rolled up from 2 child tasks")));
    QVERIFY(readOnly(fieldOf<QSpinBox>(*w, "percentComplete")));
    QVERIFY(readOnly(fieldOf<QComboBox>(*w, "status")));
    QVERIFY(readOnly(fieldOf<QLineEdit>(*w, "actualStart")));
    QVERIFY(readOnly(fieldOf<QComboBox>(*w, "type")));
    QCOMPARE(fieldOf<QSpinBox>(*w, "percentComplete")->toolTip(), QStringLiteral("Rolled up from 2 child tasks"));
    QVERIFY(!readOnly(fieldOf<QLineEdit>(*w, "title")));
    QVERIFY(!i->values().contains(QStringLiteral("percentComplete")));
    // Details again: the project's text.
    w->action(QStringLiteral("actionProjectDetails"))->trigger();
    QVERIFY(waitFor([&] {
      return w->findChild<QStackedWidget*>(QStringLiteral("inspectorStack"))->currentWidget() !=
             static_cast<QWidget*>(i);
    }));
  }

  void leafEditPreviewsTheRollup() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Drawings")));
    TaskInspector* i = insp(*w);
    const int before = static_cast<int>(desktopWrites().size());
    fieldOf<QSpinBox>(*w, "percentComplete")->setValue(80);
    QVERIFY(i->isDirty());
    QVERIFY(i->saveButton()->isEnabled());
    QCOMPARE(i->body(), (QJsonObject{{QStringLiteral("percentComplete"), 80}}));
    i->saveButton()->click();
    PreviewDialog* d = waitPreview(*w);
    QVERIFY(d);
    bool design = false;
    for (int r = 0; r < d->table()->rowCount(); ++r) {
      if (d->table()->item(r, 1)->text().contains(QStringLiteral("Design")) &&
          d->table()->item(r, 2)->text().contains(QStringLiteral("% complete 64 → 88"))) {
        design = true;
      }
    }
    QVERIFY2(design, "the preview shows Design rolling up");
    d->confirmButton()->click();
    QVERIFY(waitFor([&] { return task(QStringLiteral("Drawings")).percentComplete == 80; }));
    QCOMPARE(task(QStringLiteral("Design")).percentComplete, 88);
    QVERIFY(waitFor([&] { return !i->isDirty() && !i->isLoading() &&
                                 i->base().value(QStringLiteral("rev")).toInteger() == task(QStringLiteral("Drawings")).rev; }));
    QCOMPARE(desktopWrites().mid(before),
             (QStringList{QStringLiteral("PATCH /v1/tasks/Drawings dry"), QStringLiteral("PATCH /v1/tasks/Drawings")}));
    // Cancelling a preview keeps the edits.
    fieldOf<QSpinBox>(*w, "percentComplete")->setValue(90);
    i->saveButton()->click();
    d = waitPreview(*w);
    QVERIFY(d);
    d->cancelButton()->click();
    QVERIFY(idle(*w));
    QVERIFY(i->isDirty());
    QCOMPARE(task(QStringLiteral("Drawings")).percentComplete, 80);
  }

  void plainEditSavesDirectly() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    TaskInspector* i = insp(*w);
    const int before = static_cast<int>(desktopWrites().size());
    i->setText(QStringLiteral("title"), QStringLiteral("Permits and inspections"));
    i->setText(QStringLiteral("notes"), QStringLiteral("Call the **city office** first."));
    fieldOf<QSpinBox>(*w, "priority")->setValue(3);
    QCOMPARE(i->body().keys(), (QStringList{QStringLiteral("notes"), QStringLiteral("priority"), QStringLiteral("title")}));
    // The notes preview renders the Markdown.
    i->previewButton()->click();
    QVERIFY(i->notesPreview()->isVisible());
    QVERIFY(i->notesPreview()->toHtml().contains(QStringLiteral("city office")));
    i->previewButton()->click();
    // Ctrl+S saves.
    w->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(w.get()));
    fieldOf<QLineEdit>(*w, "title")->setFocus();
    QVERIFY(w->action(QStringLiteral("actionSaveTask"))->isEnabled());
    QTest::keyClick(fieldOf<QLineEdit>(*w, "title"), Qt::Key_S, Qt::ControlModifier);
    QVERIFY(waitFor([&] { return task(QStringLiteral("Permits")).title == QStringLiteral("Permits and inspections"); }));
    const Task t = store_->task(ids_.value(QStringLiteral("Permits"))).value();
    QCOMPARE(t.priority, 3);
    QCOMPARE(t.notes, QStringLiteral("Call the **city office** first."));
    QCOMPARE(w->taskActions()->preview(), nullptr);  // nothing ripples: no preview
    QCOMPARE(desktopWrites().mid(before),
             (QStringList{QStringLiteral("PATCH /v1/tasks/Permits dry"), QStringLiteral("PATCH /v1/tasks/Permits")}));
    QVERIFY(waitFor([&] { return !i->isDirty() && i->header()->text() == QStringLiteral("2 Permits and inspections"); }));
  }

  void checksBeforeSending() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    TaskInspector* i = insp(*w);
    const int before = static_cast<int>(desktopWrites().size());
    pick(fieldOf<QComboBox>(*w, "constraint"), "SNET");
    i->setText(QStringLiteral("deadline"), QStringLiteral("2026-02-30"));
    QVERIFY(!i->save());
    QVERIFY(shown(i->errorLabel(QStringLiteral("constraintDate"))));
    QCOMPARE(i->errorLabel(QStringLiteral("constraintDate"))->text(), QStringLiteral("⚠ Constraint date is required for SNET"));
    QVERIFY(shown(i->errorLabel(QStringLiteral("deadline"))));
    QCOMPARE(i->tabs()->currentIndex(), 1);  // the tab with the first problem
    QVERIFY(i->message()->text().contains(QStringLiteral("Nothing was sent")));
    QTest::qWait(100);
    QCOMPARE(desktopWrites().size(), before);
    // Fixed: it goes.
    i->setText(QStringLiteral("constraintDate"), QStringLiteral("2026-09-14"));
    i->setText(QStringLiteral("deadline"), QStringLiteral("2026-10-30"));
    QVERIFY(i->save());
    QVERIFY(waitFor([&] { return task(QStringLiteral("Permits")).constraintType == QStringLiteral("SNET"); }));
    QCOMPARE(task(QStringLiteral("Permits")).deadline, QStringLiteral("2026-10-30"));
    QVERIFY(!shown(i->errorLabel(QStringLiteral("constraintDate"))));
  }

  void fieldRules() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Cabinets")));
    TaskInspector* i = insp(*w);
    // Work starts: the actual start defaults to the scheduled start.
    fieldOf<QSpinBox>(*w, "percentComplete")->setValue(10);
    QCOMPARE(i->text(QStringLiteral("actualStart")), QStringLiteral("2026-10-19"));
    QVERIFY(i->message()->text().contains(QStringLiteral("Actual start set to 2026-10-19")));
    QVERIFY(i->save());
    PreviewDialog* d = waitPreview(*w);  // Build rolls up
    QVERIFY(d);
    d->confirmButton()->click();
    QVERIFY(waitFor([&] { return task(QStringLiteral("Cabinets")).percentComplete == 10; }));
    QCOMPARE(task(QStringLiteral("Cabinets")).actualStart, QStringLiteral("2026-10-19"));
    QVERIFY(waitFor([&] { return !i->isDirty() && !i->isLoading(); }));
    // Milestone: no duration.
    pick(fieldOf<QComboBox>(*w, "type"), "milestone");
    QCOMPARE(fieldOf<QDoubleSpinBox>(*w, "duration")->value(), 0.0);
    QVERIFY(readOnly(fieldOf<QDoubleSpinBox>(*w, "duration")));
    pick(fieldOf<QComboBox>(*w, "type"), "task");
    QCOMPARE(fieldOf<QDoubleSpinBox>(*w, "duration")->value(), 1.0);
    // Auto mode: the schedule owns the dates.
    pick(fieldOf<QComboBox>(*w, "mode"), "auto");
    QVERIFY(readOnly(fieldOf<QLineEdit>(*w, "start")));
    QVERIFY(fieldOf<QLineEdit>(*w, "start")->toolTip().contains(QStringLiteral("Manual")));
    pick(fieldOf<QComboBox>(*w, "mode"), "manual");
    QVERIFY(!readOnly(fieldOf<QLineEdit>(*w, "start")));
    // ASAP: no constraint date.
    QVERIFY(readOnly(fieldOf<QLineEdit>(*w, "constraintDate")));
    pick(fieldOf<QComboBox>(*w, "constraint"), "FNLT");
    QVERIFY(!readOnly(fieldOf<QLineEdit>(*w, "constraintDate")));
    i->setText(QStringLiteral("constraintDate"), QStringLiteral("2026-10-30"));
    pick(fieldOf<QComboBox>(*w, "constraint"), "ASAP");
    QCOMPARE(i->text(QStringLiteral("constraintDate")), QString());
    // Revert puts the server's copy back.
    i->setText(QStringLiteral("title"), QStringLiteral("Cabinets (oak)"));
    QVERIFY(i->revertButton()->isEnabled());
    i->revertButton()->click();
    QVERIFY(!i->isDirty());
    QCOMPARE(i->text(QStringLiteral("title")), QStringLiteral("Cabinets"));
  }

  void conflictKeepMine() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    TaskInspector* i = insp(*w);
    i->setText(QStringLiteral("title"), QStringLiteral("Permits (mine)"));
    // Meanwhile someone else renames it and changes its priority.
    updateOther(QStringLiteral("Permits"), [](Task& t) {
      t.title = QStringLiteral("Permits (theirs)");
      t.priority = 4;
    });
    QVERIFY(i->save());
    QVERIFY(waitFor([&] { return w->taskActions()->conflict() != nullptr; }));
    ConflictDialog* c = w->taskActions()->conflict();
    QVERIFY(c->table()->rowCount() >= 2);  // title (both changed) and priority (theirs)
    QVERIFY(c->keepMineButton()->isEnabled());
    c->keepMineButton()->click();
    QVERIFY(waitFor([&] { return store_->task(ids_.value(QStringLiteral("Permits"))).value().title == QStringLiteral("Permits (mine)"); }));
    QCOMPARE(store_->task(ids_.value(QStringLiteral("Permits"))).value().priority, 4);  // theirs, untouched by me
    QVERIFY(waitFor([&] { return !i->isDirty() && i->text(QStringLiteral("title")) == QStringLiteral("Permits (mine)") &&
                                 fieldOf<QSpinBox>(*w, "priority")->value() == 4; }));
  }

  void conflictTakeTheirs() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    TaskInspector* i = insp(*w);
    i->setText(QStringLiteral("title"), QStringLiteral("Permits (mine)"));
    updateOther(QStringLiteral("Permits"), [](Task& t) { t.title = QStringLiteral("Permits (theirs)"); });
    QVERIFY(i->save());
    QVERIFY(waitFor([&] { return w->taskActions()->conflict() != nullptr; }));
    w->taskActions()->conflict()->takeTheirsButton()->click();
    QVERIFY(waitFor([&] { return !i->isDirty() && i->text(QStringLiteral("title")) == QStringLiteral("Permits (theirs)"); }));
    QCOMPARE(task(QStringLiteral("Permits")).title, QStringLiteral("Permits (theirs)"));
  }

  void summaryRaceIsExplained() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    TaskInspector* i = insp(*w);
    fieldOf<QSpinBox>(*w, "percentComplete")->setValue(30);
    // Another computer gives Permits a subtask before the save arrives.
    // Within Permits' own dates: only its progress becomes derived (new dates
    // would move its rev, and the save would meet a 412 first).
    add(QStringLiteral("Zoning"), QStringLiteral("Permits"), [](Task& t) {
      t.mode = QStringLiteral("manual");
      t.start = QStringLiteral("2026-09-14");
      t.finish = QStringLiteral("2026-09-18");
      t.duration = 5;
    });
    QVERIFY(i->save());
    QVERIFY(waitFor([&] { return i->message()->text().contains(QStringLiteral("now has subtasks")); }));
    QCOMPARE(task(QStringLiteral("Permits")).percentComplete, 0);
    // The refresh shows it as a summary, locked.
    QVERIFY(waitFor([&] { return i->isSummary(); }, 8000));
  }

  void historyTab() {
    login(owner_);
    auto w = open();
    updateOther(QStringLiteral("Drawings"), [](Task& t) { t.priority = 1; });
    w->session()->refresh();
    QVERIFY(inspect(*w, QStringLiteral("Drawings")));
    TaskInspector* i = insp(*w);
    fieldOf<QSpinBox>(*w, "percentComplete")->setValue(60);
    QVERIFY(i->save());
    PreviewDialog* d = waitPreview(*w);
    QVERIFY(d);
    d->confirmButton()->click();
    QVERIFY(waitFor([&] { return task(QStringLiteral("Drawings")).percentComplete == 60; }));
    i->tabs()->setCurrentIndex(2);
    QTableWidget* h = i->historyTable();
    QVERIFY(waitFor([&] { return h->rowCount() == 3; }));
    // Newest first: my change, theirs, the create.
    QCOMPARE(h->item(0, 2)->text(), QStringLiteral("% complete: 40 → 60"));
    QCOMPARE(h->item(0, 1)->text(), QStringLiteral("This computer"));
    QVERIFY(h->item(0, 1)->toolTip().startsWith(QStringLiteral("zpt_")));
    QCOMPARE(h->item(1, 2)->text(), QStringLiteral("Priority: 2 → 1"));
    QCOMPARE(h->item(1, 1)->text(), QStringLiteral("zpt_tok_robin"));
    QCOMPARE(h->item(2, 2)->text(), QStringLiteral("Created “Drawings”"));
    QCOMPARE(i->historyStatus()->text(), QStringLiteral("3 changes, newest first."));
    // Another task: its own history.
    QVERIFY(inspect(*w, QStringLiteral("Handover")));
    QVERIFY(waitFor([&] { return h->rowCount() == 1; }));
  }

  void unsavedChangesAsk() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    TaskInspector* i = insp(*w);
    i->setText(QStringLiteral("title"), QStringLiteral("Permits (draft)"));
    // Another task: asked first; Discard drops the edits.
    QVERIFY(select(*w, QStringLiteral("Cabinets")));
    QVERIFY(waitFor([&] { return w->unsavedBox() != nullptr; }));
    QCOMPARE(i->taskId(), ids_.value(QStringLiteral("Permits")));
    QVERIFY(w->unsavedBox()->text().contains(QStringLiteral("2 Permits")));
    w->unsavedBox()->button(QMessageBox::Discard)->click();
    QVERIFY(waitFor([&] { return i->taskId() == ids_.value(QStringLiteral("Cabinets")); }));
    QCOMPARE(task(QStringLiteral("Permits")).title, QStringLiteral("Permits"));
    // Save sends them, then moves on.
    QVERIFY(waitFor([&] { return !i->isLoading(); }));
    i->setText(QStringLiteral("title"), QStringLiteral("Cabinets (oak)"));
    QVERIFY(select(*w, QStringLiteral("Counters")));
    QVERIFY(waitFor([&] { return w->unsavedBox() != nullptr; }));
    w->unsavedBox()->button(QMessageBox::Save)->click();
    QVERIFY(waitFor([&] { return task(QStringLiteral("Cabinets")).title == QStringLiteral("Cabinets (oak)"); }));
    QVERIFY(waitFor([&] { return i->taskId() == ids_.value(QStringLiteral("Counters")); }));
    // Save with a field that fails its checks: stays on the task.
    QVERIFY(waitFor([&] { return !i->isLoading() && !w->taskActions()->busy(); }));
    i->setText(QStringLiteral("title"), QString());
    QVERIFY(select(*w, QStringLiteral("Survey")));
    QVERIFY(waitFor([&] { return w->unsavedBox() != nullptr; }));
    w->unsavedBox()->button(QMessageBox::Save)->click();
    QVERIFY(waitFor([&] { return w->outline()->currentTaskId() == ids_.value(QStringLiteral("Counters")); }));
    QCOMPARE(i->taskId(), ids_.value(QStringLiteral("Counters")));
    QVERIFY(shown(i->errorLabel(QStringLiteral("title"))));
  }

  void quitWithUnsavedChangesAsks() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    insp(*w)->setText(QStringLiteral("title"), QStringLiteral("Permits (draft)"));
    w->close();
    auto* box = w->findChild<QMessageBox*>(QStringLiteral("quitUnsavedBox"));
    QVERIFY(box);
    QVERIFY(w->isVisible());
    box->button(QMessageBox::Cancel)->click();
    QTest::qWait(50);
    QVERIFY(w->isVisible());
    QVERIFY(insp(*w)->isDirty());
    w->close();
    box = w->findChild<QMessageBox*>(QStringLiteral("quitUnsavedBox"));
    QVERIFY(box);
    box->button(QMessageBox::Discard)->click();
    QVERIFY(waitFor([&] { return !w->isVisible(); }));
  }

  void followsTheServerWhileUntouched() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    TaskInspector* i = insp(*w);
    updateOther(QStringLiteral("Permits"), [](Task& t) { t.priority = 4; });
    w->session()->refresh();
    QVERIFY(waitFor([&] { return fieldOf<QSpinBox>(*w, "priority")->value() == 4 && !i->isLoading(); }, 8000));
    QCOMPARE(i->base().value(QStringLiteral("rev")).toInteger(), task(QStringLiteral("Permits")).rev);
    // With edits: they stay.
    i->setText(QStringLiteral("title"), QStringLiteral("Permits (draft)"));
    updateOther(QStringLiteral("Permits"), [](Task& t) { t.priority = 3; });
    w->session()->refresh();
    QTest::qWait(500);
    QCOMPARE(i->text(QStringLiteral("title")), QStringLiteral("Permits (draft)"));
    QCOMPARE(fieldOf<QSpinBox>(*w, "priority")->value(), 4);
  }

  void readOnlyTokenIsTold() {
    login(makeToken({{QStringLiteral("name"), QStringLiteral("viewer")},
                     {QStringLiteral("kind"), QStringLiteral("app")},
                     {QStringLiteral("scopes"), QJsonArray{QStringLiteral("read")}}}));
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    TaskInspector* i = insp(*w);
    i->setText(QStringLiteral("title"), QStringLiteral("Permits (viewer)"));
    QVERIFY(i->save());
    QVERIFY(waitFor([&] { return i->message()->text().contains(QStringLiteral("can't change tasks")); }));
    QCOMPARE(task(QStringLiteral("Permits")).title, QStringLiteral("Permits"));
    QVERIFY(i->isDirty());
  }

  void offlineIsReadOnly() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    TaskInspector* i = insp(*w);
    http_.reset();
    w->session()->refresh();
    QVERIFY(waitHealth(*w, Health::Offline));
    QVERIFY(waitFor([&] { return readOnly(fieldOf<QLineEdit>(*w, "title")); }));
    QVERIFY(!i->saveButton()->isEnabled());
    QVERIFY(i->message()->text().contains(QStringLiteral("Read-only")));
    QVERIFY(!w->action(QStringLiteral("actionSaveTask"))->isEnabled());
  }

  void altEnterOpensTheEditor() {
    login(owner_);
    auto w = open();
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    w->action(QStringLiteral("actionToggleInspector"))->trigger();  // hide it
    QVERIFY(waitFor([&] { return !insp(*w)->isVisible(); }));
    insp(*w)->tabs()->setCurrentIndex(2);
    key(*w, Qt::Key_Return, Qt::AltModifier);
    QVERIFY(waitFor([&] { return insp(*w)->isVisible(); }));
    QCOMPARE(insp(*w)->tabs()->currentIndex(), 0);
    QVERIFY(waitFor([&] { return fieldOf<QLineEdit>(*w, "title")->hasFocus(); }));
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
    updateOther(QStringLiteral("Drawings"), [](Task& t) {
      t.notes = QStringLiteral("Elevations for the **north wall**; check the window sizes with the supplier.");
    });
    login(owner_);
    auto w = open();
    w->resize(1280, 700);
    QVERIFY(inspect(*w, QStringLiteral("Drawings")));
    w->setTheme(QStringLiteral("light"));
    save(w.get(), QStringLiteral("taskeditor-details-light.png"));
    w->setTheme(QStringLiteral("dark"));
    save(w.get(), QStringLiteral("taskeditor-details-dark.png"));
    w->setTheme(QStringLiteral("high-contrast"));
    QVERIFY(inspect(*w, QStringLiteral("Design")));
    save(w.get(), QStringLiteral("taskeditor-summary-high-contrast.png"));
    w->setTheme(QStringLiteral("light"));
    QVERIFY(inspect(*w, QStringLiteral("Permits")));
    TaskInspector* i = insp(*w);
    pick(fieldOf<QComboBox>(*w, "constraint"), "SNET");
    i->setText(QStringLiteral("deadline"), QStringLiteral("2026-02-30"));
    QVERIFY(!i->save());
    save(w.get(), QStringLiteral("taskeditor-schedule-errors.png"));
    i->revert();
    QVERIFY(inspect(*w, QStringLiteral("Drawings")));
    fieldOf<QSpinBox>(*w, "percentComplete")->setValue(70);
    QVERIFY(i->save());
    PreviewDialog* d = waitPreview(*w);
    QVERIFY(d);
    save(d, QStringLiteral("taskeditor-ripple-preview.png"));
    d->confirmButton()->click();
    QVERIFY(waitFor([&] { return task(QStringLiteral("Drawings")).percentComplete == 70; }));
    QVERIFY(waitFor([&] { return !i->isDirty() && !i->isLoading(); }));
    i->tabs()->setCurrentIndex(2);
    QVERIFY(waitFor([&] { return i->historyTable()->rowCount() == 3; }));
    auto* dock = w->findChild<QDockWidget*>(QStringLiteral("inspectorDock"));
    QVERIFY(dock);
    w->resizeDocks({dock}, {460}, Qt::Horizontal);
    save(w.get(), QStringLiteral("taskeditor-history.png"));
    w->setTheme(QStringLiteral("system"));
  }
};

QTEST_MAIN(TestTaskEditor)
#include "tst_taskeditor.moc"
