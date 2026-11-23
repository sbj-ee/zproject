// Scheduling live on the server (scheduling plan S5; SPEC.md §5.1, §6, §7.2,
// §9.3, §9.4, §11, §17):
//  - the store keeps auto tasks' and summaries' dates equal to the schedule
//    on every write that can move them (task writes, links, moves, deletes,
//    restores, project start, calendars), with change rows from the
//    writer; those dates are read-only; manual tasks keep theirs; reconcile
//    on open (actor "system");
//  - the API: a dry run's ripple is exactly what the write then does, and
//    writes nothing; GET schedule and critical-path; POST reschedule; dry
//    runs refuse what writes refuse; calendar ripples; MCP get_schedule;
//  - a write with reschedule on a 2,000-task, 4,000-link project ≤ 200 ms.
// Project start: Monday 2 November 2026, Standard calendar.
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include "server/Api.hpp"
#include "server/http/HttpTypes.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::store;
using namespace zproject::server;
using http::Request;
using http::Response;

namespace {

const WriteContext kCtx{QStringLiteral("tok_test"), QStringLiteral("req-1")};

struct Plan {
  std::unique_ptr<SqliteStore> s;
  QString project;
  QHash<QString, QString> id;  // title -> id

  explicit Plan(const QString& path = QStringLiteral(":memory:"), bool withStart = true) {
    auto r = SqliteStore::open(path);
    if (!r) qFatal("open: %s", qPrintable(r.error().message));
    s = std::move(r).value();
    Project p;
    p.name = QStringLiteral("Kitchen");
    if (withStart) p.start = QStringLiteral("2026-11-02");
    project = s->createProject(p, kCtx).value().id;
  }
  QString task(const QString& title, double days = 1, const QString& parent = {},
               const std::function<void(Task&)>& set = {}) {
    Task t;
    t.project = project;
    t.title = title;
    t.duration = days;
    t.parent = parent.isEmpty() ? QString() : id.value(parent);
    if (set) set(t);
    auto r = s->createTask(t, kCtx);
    if (!r) qFatal("createTask %s: %s", qPrintable(title), qPrintable(r.error().message));
    id.insert(title, r->id);
    return r->id;
  }
  void link(const QString& a, const QString& b, int lag = 0) {
    Dependency d;
    d.predecessor = id.value(a);
    d.successor = id.value(b);
    d.lag = lag;
    auto r = s->createDependency(d, kCtx);
    if (!r) qFatal("link: %s", qPrintable(r.error().message));
  }
  Task get(const QString& title) { return s->task(id.value(title), true).value(); }
  QString span(const QString& title) {
    const Task t = get(title);
    return t.start.isEmpty() ? QStringLiteral("unscheduled") : t.start + QLatin1Char('/') + t.finish;
  }
};

struct Server {
  Plan plan;
  std::unique_ptr<Api> api;
  QByteArray owner;
  QDateTime clock = QDateTime::fromString(QStringLiteral("2026-11-23T12:00:00Z"), Qt::ISODate);

  Server() {
    ApiOptions o;
    o.allowedHosts = {QStringLiteral("127.0.0.1:8788")};
    o.now = [this] { return clock; };
    api = std::make_unique<Api>(*plan.s, o);
    owner = api->createOwnerToken(QStringLiteral("owner")).value().toLatin1();
  }
  Response call(const QByteArray& method, const QByteArray& target, const QJsonObject& body = {},
                const QByteArray& token = {}) {
    Request r;
    r.method = method;
    r.target = target;
    const qsizetype q = target.indexOf('?');
    r.path = QString::fromUtf8(q < 0 ? target : target.left(q));
    if (q >= 0) r.query = QUrlQuery(QString::fromUtf8(target.mid(q + 1)));
    r.headers.append({QByteArray("host"), QByteArray("127.0.0.1:8788")});
    r.headers.append({QByteArray("authorization"), "Bearer " + (token.isEmpty() ? owner : token)});
    if (!body.isEmpty() || method == "POST" || method == "PATCH") {
      r.headers.append({QByteArray("content-type"), QByteArray("application/json")});
      r.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    }
    r.peer = QHostAddress(QHostAddress::LocalHost);
    return api->handle(r);
  }
  static QJsonObject json(const Response& r) { return QJsonDocument::fromJson(r.body).object(); }
  QByteArray taskPath(const char* title) { return "/v1/tasks/" + plan.id.value(QLatin1String(title)).toLatin1(); }
  QByteArray projectPath() { return "/v1/projects/" + plan.project.toLatin1(); }
};

qint64 changeCount(SqliteStore& s) { return s.changesSince(0, 500).value().size(); }

}  // namespace

class TestScheduling : public QObject {
  Q_OBJECT

 private slots:
  // ---- store ----

  void datesFollowWrites() {
    Plan p;
    p.task(QStringLiteral("a"), 3);
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("2026-11-02/2026-11-04"));
    p.task(QStringLiteral("b"), 2);
    QCOMPARE(p.span(QStringLiteral("b")), QStringLiteral("2026-11-02/2026-11-03"));  // no link yet
    p.link(QStringLiteral("a"), QStringLiteral("b"));
    QCOMPARE(p.span(QStringLiteral("b")), QStringLiteral("2026-11-05/2026-11-06"));
    // A longer predecessor pushes it; the change row is the writer's.
    Task a = p.get(QStringLiteral("a"));
    a.duration = 5;
    QVERIFY(p.s->updateTask(a, a.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("b")), QStringLiteral("2026-11-09/2026-11-10"));
    const auto rows = p.s->changesFor(QStringLiteral("task"), p.id.value(QStringLiteral("b"))).value();
    QCOMPARE(rows.last().actor, kCtx.actor);
    QCOMPARE(rows.last().after.value(QStringLiteral("start")).toString(), QStringLiteral("2026-11-09"));
    // Summaries span their subtasks.
    p.task(QStringLiteral("s"));
    p.task(QStringLiteral("k1"), 2, QStringLiteral("s"));
    p.task(QStringLiteral("k2"), 4, QStringLiteral("s"));
    QCOMPARE(p.span(QStringLiteral("s")), QStringLiteral("2026-11-02/2026-11-05"));
    // Deleting the predecessor frees the successor; restoring brings both back.
    a = p.get(QStringLiteral("a"));
    QVERIFY(p.s->deleteTask(a.id, a.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("b")), QStringLiteral("2026-11-02/2026-11-03"));
    QVERIFY(p.s->restoreTask(a.id, p.get(QStringLiteral("a")).rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("b")), QStringLiteral("2026-11-09/2026-11-10"));
    // Moving b under s makes it part of s's span.
    const Task b = p.get(QStringLiteral("b"));
    QVERIFY(p.s->moveTask(b.id, p.id.value(QStringLiteral("s")), std::nullopt, b.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("s")), QStringLiteral("2026-11-02/2026-11-10"));
  }

  void derivedDatesAreReadOnly() {
    Plan p;
    p.task(QStringLiteral("a"), 3);
    Task a = p.get(QStringLiteral("a"));
    a.start = QStringLiteral("2026-11-10");
    auto r = p.s->updateTask(a, a.rev, kCtx);
    QCOMPARE(r.error().code, QStringLiteral("validation"));
    QCOMPARE(r.error().details.value(QStringLiteral("field")).toString(), QStringLiteral("start"));
    // Creating an auto task with dates is refused too.
    Task dated;
    dated.project = p.project;
    dated.title = QStringLiteral("x");
    dated.start = QStringLiteral("2026-11-10");
    dated.finish = QStringLiteral("2026-11-10");
    QCOMPARE(p.s->createTask(dated, kCtx).error().code, QStringLiteral("validation"));
    // SNET is the way to place it.
    a = p.get(QStringLiteral("a"));
    a.constraintType = QStringLiteral("SNET");
    a.constraintDate = QStringLiteral("2026-11-10");
    QVERIFY(p.s->updateTask(a, a.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("2026-11-10/2026-11-12"));
    // Manual: keeps its dates when switched, then the user sets them.
    a = p.get(QStringLiteral("a"));
    a.mode = QStringLiteral("manual");
    QVERIFY(p.s->updateTask(a, a.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("2026-11-10/2026-11-12"));
    a = p.get(QStringLiteral("a"));
    a.start = QStringLiteral("2026-11-16");
    a.finish = QStringLiteral("2026-11-16");
    QVERIFY(p.s->updateTask(a, a.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("2026-11-16/2026-11-16"));
    // Back to auto: the schedule's dates again.
    a = p.get(QStringLiteral("a"));
    a.mode = QStringLiteral("auto");
    QVERIFY(p.s->updateTask(a, a.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("2026-11-10/2026-11-12"));
    // A summary's dates come from its subtasks, whatever its mode.
    p.task(QStringLiteral("s"));
    p.task(QStringLiteral("k"), 2, QStringLiteral("s"));
    Task s = p.get(QStringLiteral("s"));
    s.mode = QStringLiteral("manual");
    s.start = QStringLiteral("2026-12-01");
    QCOMPARE(p.s->updateTask(s, s.rev, kCtx).error().code, QStringLiteral("validation"));
  }

  void calendarsAndProjectStartMoveDates() {
    Plan p;
    p.task(QStringLiteral("a"), 3);
    // Thursday 5 Nov a holiday on Standard: nothing moves (a ends Wednesday)...
    CalendarException e;
    e.calendar = QString(kStandardCalendarId);
    e.from = e.to = QDate(2026, 11, 3);  // ...but Tuesday 3 does.
    const CalendarException added = p.s->addCalendarException(e, kCtx).value();
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("2026-11-02/2026-11-05"));
    QVERIFY(p.s->deleteCalendarException(added.id, added.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("2026-11-02/2026-11-04"));
    // The project starts a week later.
    Project pr = p.s->project(p.project).value();
    pr.start = QStringLiteral("2026-11-09");
    QVERIFY(p.s->updateProject(pr, pr.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("2026-11-09/2026-11-11"));
    // Its own four-day calendar.
    CalendarDef site;
    site.name = QStringLiteral("Site");
    WorkWeek w;
    w.working = {false, true, true, true, true, false, false};  // Tue-Fri
    w.hours = {0, 8, 8, 8, 8, 0, 0};
    site.workWeek = w;
    const QString siteId = p.s->createCalendar(site, kCtx).value().id;
    pr = p.s->project(p.project).value();
    pr.calendar = siteId;
    QVERIFY(p.s->updateProject(pr, pr.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("2026-11-10/2026-11-12"));
  }

  void noProjectStartLeavesAutoTasksUnscheduled() {
    Plan p(QStringLiteral(":memory:"), false);
    p.task(QStringLiteral("a"), 2);
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("unscheduled"));
    const Schedule sched = p.s->projectSchedule(p.project).value();
    QCOMPARE(sched.violations.first().kind, QStringLiteral("no_start"));
    Task a = p.get(QStringLiteral("a"));
    a.constraintType = QStringLiteral("SNET");
    a.constraintDate = QStringLiteral("2026-11-04");
    QVERIFY(p.s->updateTask(a, a.rev, kCtx).ok());
    QCOMPARE(p.span(QStringLiteral("a")), QStringLiteral("2026-11-04/2026-11-05"));
  }

  void reconcileOnOpen() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("plan.db"));
    QString aid;
    {
      Plan p(path);
      aid = p.task(QStringLiteral("a"), 3);
      // Stale dates written behind the store's back (an older version, or a restore).
      QVERIFY(p.s->database().exec(QStringLiteral("UPDATE task SET start = '2026-12-01', finish = '2026-12-03'")).ok());
    }
    auto r = SqliteStore::open(path);
    QVERIFY(r.ok());
    auto s = std::move(r).value();
    const Task a = s->task(aid).value();
    QCOMPARE(a.start, QStringLiteral("2026-11-02"));
    const auto rows = s->changesFor(QStringLiteral("task"), aid).value();
    QCOMPARE(rows.last().actor, QStringLiteral("system"));
    // Opening again finds nothing to do.
    const qint64 n = changeCount(*s);
    s.reset();
    auto again = SqliteStore::open(path);
    QCOMPARE(changeCount(*again.value()), n);
  }

  // ---- API ----

  void dryRunRippleIsWhatTheWriteDoes() {
    Server f;
    f.plan.task(QStringLiteral("a"), 3);
    f.plan.task(QStringLiteral("b"), 2);
    f.plan.task(QStringLiteral("c"), 1);
    f.plan.link(QStringLiteral("a"), QStringLiteral("b"));
    f.plan.link(QStringLiteral("b"), QStringLiteral("c"));
    const qint64 rowsBefore = changeCount(*f.plan.s);
    const qint64 revB = f.plan.get(QStringLiteral("b")).rev;
    Response r = f.call("PATCH", f.taskPath("a") + "?dryRun=true", {{QStringLiteral("duration"), 5}});
    QCOMPARE(r.status, 200);
    const QJsonObject ripple = Server::json(r).value(QStringLiteral("ripple")).toObject();
    const QJsonArray moved = ripple.value(QStringLiteral("moved")).toArray();
    QCOMPARE(moved.size(), 3);  // a's finish, b, c
    // c finished Mon 9 Nov; with a two days longer it finishes Wed 11.
    QCOMPARE(ripple.value(QStringLiteral("finishDelta")).toInt(), 2);
    QVERIFY(!ripple.value(QStringLiteral("criticalChanged")).toBool());
    // Nothing written.
    QCOMPARE(changeCount(*f.plan.s), rowsBefore);
    QCOMPARE(f.plan.get(QStringLiteral("b")).rev, revB);
    // The write moves exactly those, to exactly those dates.
    QCOMPARE(f.call("PATCH", f.taskPath("a"), {{QStringLiteral("duration"), 5}}).status, 200);
    for (const auto& v : moved) {
      const QJsonObject m = v.toObject();
      const Task t = f.plan.s->task(m.value(QStringLiteral("task")).toString()).value();
      QCOMPARE(t.start, m.value(QStringLiteral("startAfter")).toString());
      QCOMPARE(t.finish, m.value(QStringLiteral("finishAfter")).toString());
    }
    // A dry run refuses what the write refuses (a hand-set auto date).
    r = f.call("PATCH", f.taskPath("b") + "?dryRun=true", {{QStringLiteral("start"), QStringLiteral("2026-12-01")}});
    QCOMPARE(r.status, 400);
    QCOMPARE(Server::json(r).value(QStringLiteral("details")).toObject().value(QStringLiteral("field")).toString(),
             QStringLiteral("start"));
    // A link's dry run, and a calendar's, ripple too.
    f.plan.task(QStringLiteral("d"), 1);
    r = f.call("POST", "/v1/dependencies?dryRun=true",
               {{QStringLiteral("predecessor"), f.plan.id.value(QStringLiteral("c"))},
                {QStringLiteral("successor"), f.plan.id.value(QStringLiteral("d"))}});
    QCOMPARE(Server::json(r).value(QStringLiteral("ripple")).toObject().value(QStringLiteral("moved")).toArray().size(), 1);
    r = f.call("POST", "/v1/calendars/" + QString(kStandardCalendarId).toLatin1() + "/exceptions?dryRun=true",
               {{QStringLiteral("from"), QStringLiteral("2026-11-03")}});
    QVERIFY(!Server::json(r).value(QStringLiteral("ripple")).toObject().value(QStringLiteral("moved")).toArray().isEmpty());
    QVERIFY(f.plan.s->calendarExceptions(QString(kStandardCalendarId)).value().isEmpty());
  }

  void scheduleEndpoints() {
    Server f;
    f.plan.task(QStringLiteral("a"), 3);
    f.plan.task(QStringLiteral("b"), 1);
    f.plan.task(QStringLiteral("c"), 1);
    f.plan.link(QStringLiteral("a"), QStringLiteral("c"));
    f.plan.link(QStringLiteral("b"), QStringLiteral("c"));
    f.plan.task(QStringLiteral("m"), 1, {}, [](Task& t) {  // a manual task too early for its link
      t.mode = QStringLiteral("manual");
      t.start = t.finish = QStringLiteral("2026-11-03");
    });
    f.plan.link(QStringLiteral("a"), QStringLiteral("m"));
    Response r = f.call("GET", f.projectPath() + "/schedule");
    QCOMPARE(r.status, 200);
    const QJsonObject s = Server::json(r);
    QCOMPARE(s.value(QStringLiteral("tasks")).toArray().size(), 4);
    QCOMPARE(s.value(QStringLiteral("finish")).toString(), QStringLiteral("2026-11-05"));
    QJsonObject b;
    for (const auto& v : s.value(QStringLiteral("tasks")).toArray()) {
      if (v.toObject().value(QStringLiteral("id")).toString() == f.plan.id.value(QStringLiteral("b"))) b = v.toObject();
    }
    QCOMPARE(b.value(QStringLiteral("totalFloat")).toInt(), 2);
    QVERIFY(!b.value(QStringLiteral("critical")).toBool());
    QCOMPARE(s.value(QStringLiteral("violations")).toArray().first().toObject().value(QStringLiteral("kind")).toString(),
             QStringLiteral("link_broken"));
    r = f.call("GET", f.projectPath() + "/critical-path");
    QCOMPARE(r.status, 200);
    const QJsonArray crit = Server::json(r).value(QStringLiteral("tasks")).toArray();
    QVERIFY(crit.size() >= 2);
    QCOMPARE(crit.first().toObject().value(QStringLiteral("title")).toString(), QStringLiteral("a"));
    // Reschedule: nothing stale; its dry run writes nothing.
    r = f.call("POST", f.projectPath() + "/reschedule");
    QCOMPARE(Server::json(r).value(QStringLiteral("changed")).toInt(), 0);
    QCOMPARE(f.call("POST", f.projectPath() + "/reschedule?dryRun=true").status, 200);
    QCOMPARE(f.call("POST", f.projectPath() + "/reschedule", {{QStringLiteral("x"), 1}}).status, 400);
    // MCP: get_schedule for a read-only agent.
    const Response tok = f.call("POST", "/v1/tokens",
                                {{QStringLiteral("name"), QStringLiteral("agent")},
                                 {QStringLiteral("kind"), QStringLiteral("agent")},
                                 {QStringLiteral("projects"), QJsonArray{f.plan.project}}});
    const QByteArray agent = Server::json(tok).value(QStringLiteral("token")).toString().toLatin1();
    r = f.call("POST", "/mcp",
               {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), 1},
                {QStringLiteral("method"), QStringLiteral("tools/call")},
                {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("get_critical_path")},
                                                       {QStringLiteral("arguments"),
                                                        QJsonObject{{QStringLiteral("project"), f.plan.project}}}}}},
               agent);
    const QJsonObject res = Server::json(r).value(QStringLiteral("result")).toObject();
    QVERIFY2(!res.value(QStringLiteral("isError")).toBool(), r.body.constData());
    QVERIFY(!res.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("tasks")).toArray().isEmpty());
  }

  // SPEC.md §17: writes with reschedule ≤ 200 ms (2,000 tasks, 4,000 links).
  void writeLatencyAtScale() {
    Server f;
    Database& db = f.plan.s->database();
    QRandomGenerator rng(17);
    QStringList ids;
    QVERIFY(db.exec(QStringLiteral("BEGIN")).ok());
    for (int i = 0; i < 2000; ++i) {
      const QString id = QStringLiteral("%1").arg(i + 1, 20, 10, QLatin1Char('0'));  // hex-safe: digits only
      ids << id;
      QVERIFY(db.exec(QStringLiteral("INSERT INTO task (id, createdAt, updatedAt, project, ord, title, duration) "
                                     "VALUES ('%1', 'x', 'x', '%2', %3, 'task %3', %4)")
                          .arg(id, f.plan.project)
                          .arg(i + 1)
                          .arg(1 + rng.bounded(8)))
                  .ok());
    }
    int links = 0;
    QSet<QPair<int, int>> pairs;
    while (links < 4000) {
      const int a = static_cast<int>(rng.bounded(1999));
      const int b = a + 1 + static_cast<int>(rng.bounded(1999 - a));
      if (pairs.contains({a, b})) continue;
      pairs.insert({a, b});
      QVERIFY(db.exec(QStringLiteral("INSERT INTO dependency (id, createdAt, updatedAt, project, predecessor, successor) "
                                     "VALUES ('%1', 'x', 'x', '%2', '%3', '%4')")
                          .arg(QStringLiteral("d%1").arg(links, 19, 10, QLatin1Char('0')).replace(QLatin1Char('d'), QLatin1Char('a')),
                               f.plan.project, ids.at(a), ids.at(b)))
                  .ok());
      ++links;
    }
    QVERIFY(db.exec(QStringLiteral("COMMIT")).ok());
    QVERIFY(f.plan.s->reschedule(f.plan.project, kCtx).ok());  // dates for all, once
    // One edit at the front of the chain: the timed write.
    QElapsedTimer t;
    t.start();
    const Response r = f.call("PATCH", "/v1/tasks/" + ids.first().toLatin1(), {{QStringLiteral("duration"), 9}});
    const qint64 ms = t.elapsed();
    QCOMPARE(r.status, 200);
    qInfo("PATCH with reschedule, 2,000 tasks / 4,000 links: %lld ms", ms);
#if defined(__SANITIZE_ADDRESS__) || !defined(QT_NO_DEBUG)
    QVERIFY2(ms < 800, qPrintable(QString::number(ms)));
#else
    QVERIFY2(ms < 200, qPrintable(QString::number(ms)));
#endif
  }
};

QTEST_GUILESS_MAIN(TestScheduling)
#include "tst_scheduling.moc"
