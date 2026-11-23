// Desktop projects (SPEC.md §7.10, §8.1, §9.3; plan PR 5), offscreen, against
// the real zprojectd API on loopback with an in-memory store: the sidebar and
// all-projects table, the details page, create / edit / delete with dry runs
// and If-Match, the delete preview and its confirmId, the shared 412
// conflict dialog, and the error paths (403, 404, offline). With
// ZPROJECT_SCREENSHOT_DIR set, screenshots() renders docs/screenshots/
// projects-*.png from the same example data. Tokens are made per run.
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include "app/EditDialogs.hpp"
#include "app/MainWindow.hpp"
#include "app/ProjectActions.hpp"
#include "client/ApiClient.hpp"
#include "client/TokenStore.hpp"
#include "server/Api.hpp"
#include "server/http/HttpServer.hpp"
#include "store/Audit.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::app;
using Health = AppSession::Health;

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

class TestProjects : public QObject {
  Q_OBJECT

  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<store::SqliteStore> store_;
  std::unique_ptr<server::Api> api_;
  std::unique_ptr<server::http::Server> http_;
  QDateTime clock_;
  QString owner_;
  QString url_;
  QString kitchen_;
  QString web_;

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
  // zproject-cli logged in on this machine with `token`.
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
  Project project(const QString& id) { return store_->project(id, true).value(); }
  QString makeProject(const QString& name, const QString& status, const QString& rag, const QString& owner,
                      const QString& start, const QString& finish) {
    Project p;
    p.name = name;
    p.status = status;
    p.rag = rag;
    p.owner = owner;
    p.start = start;
    p.finish = finish;
    return store_->createProject(p, kOther).value().id;
  }
  void makeTask(const QString& project, const QString& title) {
    Task t;
    t.project = project;
    t.title = title;
    QVERIFY(store_->createTask(t, kOther).ok());
  }
  // Writes the store saw from the desktop, as "METHOD path dryRun".
  QStringList desktopWrites() {
    QStringList out;
    store::AuditLog log(store_->database());
    const QList<store::AuditEntry> rows = log.since(0, 500).value();
    for (const auto& e : rows) {
      if (e.method == QLatin1String("GET")) continue;
      out << QStringLiteral("%1 %2%3").arg(e.method, e.path.section(QLatin1Char('?'), 0, 0),
                                           e.dryRun ? QStringLiteral(" dry") : QString());
    }
    return out;
  }
  // Edit the selected project once the server's copy is in.
  static ProjectEditor* editCurrent(MainWindow& w) {
    w.action(QStringLiteral("actionEditProject"))->trigger();
    ProjectEditor* e = w.projectActions()->editor();
    if (e != nullptr && !QTest::qWaitFor([e] { return !e->isLoading(); }, 5000)) qWarning("editor still loading");
    return e;
  }
  static bool errorShown(ProjectEditor* e, const QString& field) {
    QLabel* l = e->errorLabel(field);
    return l != nullptr && !l->isHidden();
  }
  static QString errorText(ProjectEditor* e, const QString& field) {
    QLabel* l = e->errorLabel(field);
    return l != nullptr ? l->text() : QString();
  }
  static bool waitHealth(MainWindow& w, Health h, int ms = 5000) {
    return QTest::qWaitFor([&] { return w.session() && w.session()->health() == h; }, ms);
  }
  static bool waitFor(const std::function<bool()>& f, int ms = 5000) { return QTest::qWaitFor(f, ms); }
  static QStringList sidebar(MainWindow& w, bool visibleOnly = true) {
    QStringList out;
    for (int i = 0; i < w.projectList()->count(); ++i) {
      if (!visibleOnly || !w.projectList()->item(i)->isHidden()) out << w.projectList()->item(i)->text();
    }
    return out;
  }
  // A window, online, with the cache filled.
  std::unique_ptr<MainWindow> open(int projects) {
    auto w = std::make_unique<MainWindow>(options());
    w->show();
    if (!QTest::qWaitForWindowExposed(w.get())) qWarning("not exposed");
    if (!waitHealth(*w, Health::Online) || !waitFor([&] { return w->projectList()->count() == projects; })) {
      qWarning("window did not come online with %d projects", projects);
    }
    return w;
  }
  static void refresh(MainWindow& w) {
    const QDateTime before = w.session()->lastRefresh();
    w.session()->refresh();
    if (!QTest::qWaitFor([&] { return w.session()->lastRefresh() != before; }, 5000)) qWarning("no refresh");
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
    clock_ = QDateTime::currentDateTimeUtc();
    server::ApiOptions o;
    o.allowedHosts = {QStringLiteral("127.0.0.1:%1").arg(http_->port())};
    o.now = [this] { return clock_; };
    api_ = std::make_unique<server::Api>(*store_, o);
    owner_ = api_->createOwnerToken(QStringLiteral("owner")).value();
    url_ = QStringLiteral("http://127.0.0.1:%1").arg(http_->port());
    kitchen_ = makeProject(QStringLiteral("Kitchen remodel"), QStringLiteral("active"), QStringLiteral("green"),
                           QStringLiteral("Alex Rivera"), QStringLiteral("2026-09-07"), QStringLiteral("2026-12-18"));
    makeTask(kitchen_, QStringLiteral("Design and permits"));
    makeTask(kitchen_, QStringLiteral("Cabinets installed"));
    web_ = makeProject(QStringLiteral("Website relaunch"), QStringLiteral("on_hold"), QStringLiteral("amber"),
                       QStringLiteral("Sam Lee"), QStringLiteral("2026-10-01"), QStringLiteral("2027-01-29"));
    makeProject(QStringLiteral("Office move"), QStringLiteral("done"), QStringLiteral("green"), QStringLiteral("Sam Lee"),
                QStringLiteral("2026-03-02"), QStringLiteral("2026-06-30"));
    makeProject(QStringLiteral("Q1 board report"), QStringLiteral("active"), QStringLiteral("red"),
                QStringLiteral("Alex Rivera"), QStringLiteral("2027-01-04"), QStringLiteral("2027-02-12"));
    makeProject(QStringLiteral("Old intranet"), QStringLiteral("cancelled"), QStringLiteral("amber"),
                QStringLiteral("Robin Park"), QString(), QString());
  }
  void cleanup() {
    http_.reset();
    api_.reset();
    store_.reset();
    dir_.reset();
  }

  // ---- list and details ----

  void sidebarTableAndDetails() {
    login(owner_);
    auto w = open(5);
    // Active first, then on hold; done and cancelled behind the toggle.
    QCOMPARE(sidebar(*w), (QStringList{QStringLiteral("Kitchen remodel"), QStringLiteral("Q1 board report"),
                                       QStringLiteral("Website relaunch (on hold)")}));
    QToolButton* closed = w->showClosedButton();
    QVERIFY(!closed->isHidden());
    QCOMPARE(closed->text(), QStringLiteral("Show done and cancelled (2)"));
    closed->click();
    QCOMPARE(sidebar(*w).size(), 5);
    QCOMPARE(sidebar(*w).last(), QStringLiteral("Old intranet (cancelled)"));
    QCOMPARE(closed->text(), QStringLiteral("Hide done and cancelled (2)"));
    closed->click();
    QCOMPARE(sidebar(*w).size(), 3);

    // The details page of the selected project.
    QCOMPARE(w->currentProjectId(), kitchen_);
    QCOMPARE(w->mainStack()->currentIndex(), 1);
    const QString html = w->projectDetails()->text();
    QVERIFY(html.contains(QStringLiteral("Alex Rivera")));
    QVERIFY(html.contains(QStringLiteral("G · Green")));
    QVERIFY(html.contains(QStringLiteral("2026-12-18")));
    QVERIFY(w->findChild<QLabel*>(QStringLiteral("projectSummary"))->text().startsWith(QStringLiteral("2 tasks")));

    // View → All Projects: every project, RAG as a letter (not colour alone).
    w->action(QStringLiteral("actionAllProjects"))->trigger();
    QTableWidget* t = w->projectTable();
    QCOMPARE(w->mainStack()->currentWidget(), t);
    QCOMPARE(t->rowCount(), 5);
    QCOMPARE(t->columnCount(), 7);
    QCOMPARE(t->horizontalHeaderItem(5)->text(), QStringLiteral("Target finish"));
    QCOMPARE(t->item(1, 0)->text(), QStringLiteral("Q1 board report"));
    QCOMPARE(t->item(1, 2)->text(), QStringLiteral("R"));
    QCOMPARE(t->item(1, 2)->data(Qt::AccessibleTextRole).toString(), QStringLiteral("RAG Red"));
    QCOMPARE(t->item(2, 1)->text(), QStringLiteral("On hold"));
    QCOMPARE(t->item(4, 0)->text(), QStringLiteral("Old intranet"));
    // Opening a closed project from the table shows it in the sidebar too.
    emit t->cellActivated(4, 0);
    QCOMPARE(w->mainStack()->currentIndex(), 1);
    QCOMPARE(w->projectList()->currentItem()->text(), QStringLiteral("Old intranet (cancelled)"));
    QVERIFY(w->showClosedButton()->isChecked());
    // A refresh keeps the table showing.
    w->action(QStringLiteral("actionAllProjects"))->trigger();
    refresh(*w);
    QCOMPARE(w->mainStack()->currentWidget(), t);
  }

  // ---- create ----

  void createProject() {
    login(owner_);
    auto w = open(5);
    QVERIFY(w->action(QStringLiteral("actionNewProject"))->isEnabled());
    w->action(QStringLiteral("actionNewProject"))->trigger();
    ProjectEditor* e = w->projectActions()->editor();
    QVERIFY(e);
    QVERIFY(e->isNew());
    QCOMPARE(e->saveButton()->text(), QStringLiteral("Create Project"));
    // Client-side checks: nothing reaches the server.
    const int writes = static_cast<int>(desktopWrites().size());
    e->saveButton()->click();
    QVERIFY(errorShown(e, QStringLiteral("name")));
    QVERIFY(errorText(e, QStringLiteral("name")).contains(QStringLiteral("is required")));
    e->setText(QStringLiteral("name"), QStringLiteral("Garden"));
    e->setText(QStringLiteral("start"), QStringLiteral("2027-04-31"));
    e->setText(QStringLiteral("finish"), QStringLiteral("2027-03-01"));
    e->setText(QStringLiteral("budget"), QStringLiteral("lots"));
    e->setText(QStringLiteral("color"), QStringLiteral("green"));
    e->saveButton()->click();
    QVERIFY(!errorShown(e, QStringLiteral("name")));
    for (const char* f : {"start", "budget", "color"}) {
      QVERIFY2(errorShown(e, QString::fromLatin1(f)), f);
    }
    QVERIFY(e->message()->text().contains(QStringLiteral("Nothing was sent")));
    QCOMPARE(desktopWrites().size(), writes);

    e->setText(QStringLiteral("start"), QStringLiteral("2027-03-01"));
    e->setText(QStringLiteral("finish"), QStringLiteral("2027-05-28"));
    e->setText(QStringLiteral("budget"), QStringLiteral("1500"));
    e->setText(QStringLiteral("color"), QStringLiteral("#2fa8f0"));
    e->setText(QStringLiteral("owner"), QStringLiteral("Robin Park"));
    e->setText(QStringLiteral("objectives"), QStringLiteral("Raised beds\nDrip irrigation"));
    qobject_cast<QComboBox*>(e->field(QStringLiteral("rag")))->setCurrentIndex(1);
    QPointer<ProjectEditor> guard(e);
    e->saveButton()->click();
    QVERIFY(waitFor([&] { return guard.isNull(); }));  // closed once the server has it
    // A dry run first, then the create.
    QCOMPARE(desktopWrites().mid(writes),
             (QStringList{QStringLiteral("POST /v1/projects dry"), QStringLiteral("POST /v1/projects")}));
    QVERIFY(waitFor([&] { return w->projectList()->currentItem() &&
                                 w->projectList()->currentItem()->text() == QLatin1String("Garden"); }));
    const Project g = project(w->currentProjectId());
    QCOMPARE(g.owner, QStringLiteral("Robin Park"));
    QCOMPARE(g.rag, QStringLiteral("amber"));
    QCOMPARE(g.budget.value_or(0), 1500.0);
    QCOMPARE(g.objectives, QStringLiteral("Raised beds\nDrip irrigation"));
    QCOMPARE(w->projectActions()->lastMessage(), QStringLiteral("Created project Garden."));
    QVERIFY(w->projectDetails()->text().contains(QStringLiteral("Raised beds<br>Drip irrigation")));
  }

  // ---- edit ----

  void editSendsIfMatchAfterADryRun() {
    login(owner_);
    auto w = open(5);
    const qint64 rev = project(kitchen_).rev;
    ProjectEditor* e = editCurrent(*w);
    QVERIFY(e && !e->isNew());
    QCOMPARE(qobject_cast<QLineEdit*>(e->field(QStringLiteral("owner")))->text(), QStringLiteral("Alex Rivera"));
    e->setText(QStringLiteral("owner"), QStringLiteral("Sam Lee"));
    e->setText(QStringLiteral("description"), QStringLiteral("Cabinets, counters, lights."));
    QCOMPARE(e->body(), (QJsonObject{{QStringLiteral("owner"), QStringLiteral("Sam Lee")},
                                     {QStringLiteral("description"), QStringLiteral("Cabinets, counters, lights.")}}));
    const int writes = static_cast<int>(desktopWrites().size());
    QPointer<ProjectEditor> guard(e);
    e->saveButton()->click();
    QVERIFY(waitFor([&] { return guard.isNull(); }));
    const QString path = QStringLiteral("/v1/projects/") + kitchen_;
    QCOMPARE(desktopWrites().mid(writes), (QStringList{QStringLiteral("PATCH ") + path + QStringLiteral(" dry"),
                                                       QStringLiteral("PATCH ") + path}));
    const Project p = project(kitchen_);
    QCOMPARE(p.owner, QStringLiteral("Sam Lee"));
    QCOMPARE(p.rev, rev + 1);  // one write, no blind retry
    QVERIFY(waitFor([&] { return w->projectDetails()->text().contains(QStringLiteral("Cabinets, counters")); }));

    // No change: nothing is sent.
    e = editCurrent(*w);
    QVERIFY(e);
    e->saveButton()->click();
    QTest::qWait(20);
    QCOMPARE(w->projectActions()->lastMessage(), QStringLiteral("Nothing changed."));
  }

  // ---- 412: the conflict dialog ----

  void conflict_data() {
    QTest::addColumn<int>("choice");
    QTest::newRow("take theirs") << int(ConflictDialog::Choice::TakeTheirs);
    QTest::newRow("keep mine") << int(ConflictDialog::Choice::KeepMine);
    QTest::newRow("cancel") << int(ConflictDialog::Choice::Cancel);
  }
  void conflict() {
    QFETCH(int, choice);
    login(owner_);
    auto w = open(5);
    ProjectEditor* e = editCurrent(*w);
    QVERIFY(e);
    e->setText(QStringLiteral("owner"), QStringLiteral("Sam Lee"));               // both
    e->setText(QStringLiteral("description"), QStringLiteral("New cabinets"));    // only mine
    // Meanwhile someone else saves.
    Project other = project(kitchen_);
    other.owner = QStringLiteral("Robin Park");
    other.rag = QStringLiteral("amber");
    QVERIFY(store_->updateProject(other, other.rev, kOther).ok());
    const qint64 theirRev = other.rev + 1;

    const int writes = static_cast<int>(desktopWrites().size());
    e->saveButton()->click();
    QVERIFY2(waitFor([&] { return w->projectActions()->conflict() != nullptr; }),
             qPrintable(w->projectActions()->lastMessage() + QLatin1Char('|') + desktopWrites().join(QLatin1Char(','))));
    ConflictDialog* d = w->projectActions()->conflict();
    QCOMPARE(desktopWrites().mid(writes),
             QStringList{QStringLiteral("PATCH /v1/projects/") + kitchen_ + QStringLiteral(" dry")});  // the 412
    QVERIFY(d->summary()->text().contains(QStringLiteral("Changed by tok_robin")));
    QVERIFY(d->summary()->text().contains(QStringLiteral("1 field was changed on both sides")));
    QHash<QString, QStringList> rows;
    for (int i = 0; i < d->table()->rowCount(); ++i) {
      rows.insert(d->table()->item(i, 0)->text(),
                  {d->table()->item(i, 1)->text(), d->table()->item(i, 2)->text(),
                   d->table()->item(i, 3)->data(Qt::UserRole).toString()});
    }
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.value(QStringLiteral("Owner")),
             (QStringList{QStringLiteral("Sam Lee"), QStringLiteral("Robin Park"), QStringLiteral("conflict")}));
    QCOMPARE(rows.value(QStringLiteral("Description")).value(2), QStringLiteral("mine"));
    QCOMPARE(rows.value(QStringLiteral("RAG")), (QStringList{QStringLiteral("green"), QStringLiteral("amber"),
                                                             QStringLiteral("theirs")}));
    QVERIFY(d->cancelButton()->isDefault());  // nothing is overwritten by Enter

    QPointer<ProjectEditor> guard(e);
    switch (static_cast<ConflictDialog::Choice>(choice)) {
      case ConflictDialog::Choice::TakeTheirs:
        d->takeTheirsButton()->click();
        QVERIFY(waitFor([&] { return guard.isNull(); }));
        QCOMPARE(project(kitchen_).owner, QStringLiteral("Robin Park"));
        QCOMPARE(project(kitchen_).description, QStringLiteral("New cabinets"));
        QCOMPARE(project(kitchen_).rag, QStringLiteral("amber"));
        QCOMPARE(project(kitchen_).rev, theirRev + 1);
        break;
      case ConflictDialog::Choice::KeepMine:
        d->keepMineButton()->click();
        QVERIFY(waitFor([&] { return guard.isNull(); }));
        QCOMPARE(project(kitchen_).owner, QStringLiteral("Sam Lee"));
        QCOMPARE(project(kitchen_).description, QStringLiteral("New cabinets"));
        QCOMPARE(project(kitchen_).rag, QStringLiteral("amber"));  // theirs, untouched by me
        break;
      case ConflictDialog::Choice::Cancel:
        d->cancelButton()->click();
        QTest::qWait(100);
        QVERIFY(!guard.isNull());
        QVERIFY(e->isVisible());
        QVERIFY(e->message()->text().startsWith(QStringLiteral("Not saved")));
        QCOMPARE(project(kitchen_).owner, QStringLiteral("Robin Park"));
        QCOMPARE(project(kitchen_).rev, theirRev);
        e->reject();
        return;
    }
    // A second dry run on the server's revision, then the PATCH; no blind retry.
    const QStringList after = desktopWrites().mid(writes);
    QCOMPARE(after.size(), 3);
    QVERIFY(after[1].endsWith(QStringLiteral(" dry")));
    QVERIFY(!after[2].endsWith(QStringLiteral(" dry")));
  }

  // ---- delete ----

  void deleteShowsPreviewFirst() {
    login(owner_);
    auto w = open(5);
    w->action(QStringLiteral("actionDeleteProject"))->trigger();
    QVERIFY(waitFor([&] { return w->projectActions()->preview() != nullptr; }));
    PreviewDialog* d = w->projectActions()->preview();
    QVERIFY(d->intro()->text().contains(QStringLiteral("Delete project Kitchen remodel and its 2 tasks?")));
    QCOMPARE(d->table()->rowCount(), 3);
    QCOMPARE(d->table()->item(1, 1)->text(), QStringLiteral("Task Design and permits"));
    QVERIFY(d->cancelButton()->isDefault());
    d->cancelButton()->click();
    QVERIFY(!project(kitchen_).deleted);
    QVERIFY(waitFor([&] { return w->projectActions()->preview() == nullptr; }));

    w->action(QStringLiteral("actionDeleteProject"))->trigger();
    QVERIFY(waitFor([&] { return w->projectActions()->preview() != nullptr; }));
    w->projectActions()->preview()->confirmButton()->click();
    QVERIFY(waitFor([&] { return project(kitchen_).deleted; }));
    QVERIFY(waitFor([&] { return !sidebar(*w).contains(QStringLiteral("Kitchen remodel")); }));
    QVERIFY2(w->projectActions()->lastMessage().startsWith(QStringLiteral("Deleted project Kitchen remodel.")),
             qPrintable(w->projectActions()->lastMessage()));
    QCOMPARE(store_->tasks(kitchen_).value().size(), 0);
  }

  void deleteWithConfirmingToken() {
    // A device token that needs confirmations: the dry run's confirmId goes
    // with the delete; an expired one means a fresh preview, not a retry.
    login(makeToken({{QStringLiteral("name"), QStringLiteral("laptop")},
                     {QStringLiteral("kind"), QStringLiteral("device")},
                     {QStringLiteral("requireConfirm"), true}}));
    auto w = open(5);
    w->action(QStringLiteral("actionDeleteProject"))->trigger();
    QVERIFY(waitFor([&] { return w->projectActions()->preview() != nullptr; }));
    clock_ = clock_.addSecs(301);  // the confirmation expires
    PreviewDialog* firstRaw = w->projectActions()->preview();
    QVERIFY(firstRaw);
    QPointer<PreviewDialog> first(firstRaw);
    firstRaw->confirmButton()->click();
    QVERIFY(waitFor([&] {
      return w->projectActions()->preview() != nullptr && w->projectActions()->preview() != first.data();
    }));
    PreviewDialog* again = w->projectActions()->preview();
    QVERIFY(again->intro()->text().startsWith(QStringLiteral("The confirmation expired")));
    QVERIFY(!project(kitchen_).deleted);
    again->confirmButton()->click();
    QVERIFY(waitFor([&] { return project(kitchen_).deleted; }));
    bool confirmed = false;
    store::AuditLog log(store_->database());
    const QList<store::AuditEntry> rows = log.since(0, 500).value();
    for (const auto& e : rows) {
      if (e.method == QLatin1String("DELETE") && !e.dryRun && e.status == 200) confirmed = !e.confirmId.isEmpty();
    }
    QVERIFY(confirmed);
  }

  // ---- errors ----

  void readOnlyTokenIsTold() {
    login(makeToken({{QStringLiteral("name"), QStringLiteral("viewer")},
                     {QStringLiteral("kind"), QStringLiteral("app")},
                     {QStringLiteral("scopes"), QJsonArray{QStringLiteral("read")}}}));
    auto w = open(5);
    ProjectEditor* e = editCurrent(*w);
    QVERIFY(e);
    e->setText(QStringLiteral("owner"), QStringLiteral("Sam Lee"));
    e->saveButton()->click();
    QVERIFY(waitFor([&] { return e->message()->text().contains(QStringLiteral("projects:write")); }));
    QVERIFY(e->isVisible());
    QCOMPARE(project(kitchen_).owner, QStringLiteral("Alex Rivera"));
    e->reject();
  }

  void deletedElsewhere() {
    login(owner_);
    auto w = open(5);
    ProjectEditor* ed = editCurrent(*w);
    QVERIFY(ed);
    QPointer<ProjectEditor> e(ed);
    QVERIFY(store_->deleteProject(kitchen_, project(kitchen_).rev, kOther).ok());
    ed->setText(QStringLiteral("owner"), QStringLiteral("Sam Lee"));
    ed->saveButton()->click();
    QVERIFY(waitFor([&] { return e.isNull(); }));
    QCOMPARE(w->projectActions()->lastMessage(), QStringLiteral("This project was deleted elsewhere. The list is being refreshed."));
    QVERIFY(waitFor([&] { return !sidebar(*w).contains(QStringLiteral("Kitchen remodel")); }));
  }

  void offlineTurnsEditingOff() {
    login(owner_);
    auto w = open(5);
    QVERIFY(w->findChild<QPushButton*>(QStringLiteral("editProjectButton"))->isEnabled());
    http_.reset();  // the listener and its open connections
    w->session()->refresh();
    QVERIFY2(waitHealth(*w, Health::Offline), qPrintable(QString::number(int(w->session()->health()))));
    for (const char* a : {"actionNewProject", "actionEditProject", "actionDeleteProject"}) {
      QVERIFY2(!w->action(QString::fromLatin1(a))->isEnabled(), a);
    }
    QVERIFY(!w->findChild<QPushButton*>(QStringLiteral("editProjectButton"))->isEnabled());
    QVERIFY(w->projectActions()->editProject(kitchen_) == nullptr);
    QCOMPARE(w->projectActions()->lastMessage(), QStringLiteral("Editing is off until the server answers."));
    QCOMPARE(sidebar(*w).size(), 3);  // the cache still shows them
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
    Project k = project(kitchen_);
    k.description = QStringLiteral("New cabinets, counters and lighting; keep the existing floor.");
    k.objectives = QStringLiteral("Usable by the holidays\nUnder budget");
    k.sponsor = QStringLiteral("Jordan Diaz");
    k.budget = 42000;
    QVERIFY(store_->updateProject(k, k.rev, kOther).ok());
    login(owner_);
    auto w = open(5);
    w->resize(1100, 640);
    w->setTheme(QStringLiteral("light"));
    save(w.get(), QStringLiteral("projects-details-light.png"));
    w->setTheme(QStringLiteral("dark"));
    save(w.get(), QStringLiteral("projects-details-dark.png"));
    w->setTheme(QStringLiteral("light"));
    w->showClosedButton()->setChecked(true);
    w->action(QStringLiteral("actionAllProjects"))->trigger();
    save(w.get(), QStringLiteral("projects-table-light.png"));
    w->setTheme(QStringLiteral("high-contrast"));
    save(w.get(), QStringLiteral("projects-table-high-contrast.png"));
    w->setTheme(QStringLiteral("light"));
    w->selectProject(kitchen_);
    w->showProjectDetails();

    ProjectEditor* e = editCurrent(*w);
    e->setText(QStringLiteral("finish"), QStringLiteral("2026-12-40"));
    e->saveButton()->click();
    save(e, QStringLiteral("project-editor.png"));
    e->setText(QStringLiteral("finish"), QStringLiteral("2026-12-18"));
    e->setText(QStringLiteral("owner"), QStringLiteral("Sam Lee"));
    e->setText(QStringLiteral("description"), QStringLiteral("New cabinets and counters."));
    Project other = project(kitchen_);
    other.owner = QStringLiteral("Robin Park");
    other.rag = QStringLiteral("amber");
    QVERIFY(store_->updateProject(other, other.rev, kOther).ok());
    e->saveButton()->click();
    QVERIFY(waitFor([&] { return w->projectActions()->conflict() != nullptr; }));
    save(w->projectActions()->conflict(), QStringLiteral("project-conflict-light.png"));
    w->setTheme(QStringLiteral("dark"));
    save(w->projectActions()->conflict(), QStringLiteral("project-conflict-dark.png"));
    w->setTheme(QStringLiteral("light"));
    w->projectActions()->conflict()->cancelButton()->click();
    e->reject();
    QTest::qWait(50);

    w->action(QStringLiteral("actionDeleteProject"))->trigger();
    QVERIFY(waitFor([&] { return w->projectActions()->preview() != nullptr; }));
    save(w->projectActions()->preview(), QStringLiteral("project-delete-preview.png"));
    w->projectActions()->preview()->cancelButton()->click();
    w->setTheme(QStringLiteral("system"));
  }
};

QTEST_MAIN(TestProjects)
#include "tst_projects.moc"
