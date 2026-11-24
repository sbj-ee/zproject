// Calendars on the server (scheduling plan S4; SPEC.md §5.1, §6.1, §7.4,
// §9.4):
//  - the store: Standard is there (no base, can't be deleted), create, read,
//    list (Standard first), update, bases (live, no loop, not deeper than 8),
//    delete (refused while a project, task or calendar uses it; its
//    exceptions go with it), exceptions, holiday import (no duplicates), a
//    project's and a task's calendar, the resolved calendars for the
//    scheduler;
//  - the API: the nine operations, scopes, "a token for all projects" to
//    write, dry runs, confirm, If-Match, workWeek checks, project and task
//    `calendar` fields.
#include <QJsonArray>
#include <QJsonDocument>
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
const QString kStd = QString(kStandardCalendarId);

std::unique_ptr<SqliteStore> openMemory() {
  auto r = SqliteStore::open(QStringLiteral(":memory:"));
  if (!r) qFatal("open: %s", qPrintable(r.error().message));
  return std::move(r).value();
}

CalendarDef cal(const char* name, const QString& base = {}) {
  CalendarDef c;
  c.name = QLatin1String(name);
  c.base = base;
  return c;
}

WorkWeek fourTens() {
  WorkWeek w;
  w.working = {true, true, true, true, false, false, false};
  w.hours = {10, 10, 10, 10, 0, 0, 0};
  return w;
}

QJsonArray weekJson(const WorkWeek& w) {
  QJsonArray a;
  for (int i = 0; i < 7; ++i) {
    a.append(QJsonObject{{QStringLiteral("working"), w.working[static_cast<size_t>(i)]},
                         {QStringLiteral("hours"), w.hours[static_cast<size_t>(i)]}});
  }
  return a;
}

struct Server {
  std::unique_ptr<SqliteStore> store = openMemory();
  std::unique_ptr<Api> api;
  QByteArray owner;
  QDateTime clock = QDateTime::fromString(QStringLiteral("2026-11-23T12:00:00Z"), Qt::ISODate);

  Server() {
    ApiOptions o;
    o.allowedHosts = {QStringLiteral("127.0.0.1:8788")};
    o.now = [this] { return clock; };
    api = std::make_unique<Api>(*store, o);
    owner = api->createOwnerToken(QStringLiteral("owner")).value().toLatin1();
  }
  Response call(const QByteArray& method, const QByteArray& target, const QJsonObject& body = {},
                const QList<QPair<QByteArray, QByteArray>>& extra = {}, const QByteArray& token = {}) {
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
    for (const auto& h : extra) r.headers.append(h);
    r.peer = QHostAddress(QHostAddress::LocalHost);
    return api->handle(r);
  }
  static QJsonObject json(const Response& r) { return QJsonDocument::fromJson(r.body).object(); }
  QByteArray token(const QJsonObject& b) {
    const Response r = call("POST", "/v1/tokens", b);
    if (r.status != 201) qFatal("token: %s", r.body.constData());
    return json(r).value(QStringLiteral("token")).toString().toLatin1();
  }
};

QString code(const Response& r) { return Server::json(r).value(QStringLiteral("code")).toString(); }

}  // namespace

class TestCalendars : public QObject {
  Q_OBJECT

 private slots:
  // ---- store ----

  void standardIsSeeded() {
    auto s = openMemory();
    QCOMPARE(s->schemaVersion(), 4);
    auto std = s->calendar(kStd);
    QVERIFY(std.ok());
    QCOMPARE(std->name, QStringLiteral("Standard"));
    QVERIFY(!std->workWeek.has_value());  // the built-in Monday–Friday
    QCOMPARE(std->hoursPerDay, 8.0);
    CalendarDef based = std.value();
    based.base = s->createCalendar(cal("Other"), kCtx).value().id;
    QCOMPARE(s->updateCalendar(based, std->rev, kCtx).error().code, QStringLiteral("validation"));
    auto del = s->deleteCalendar(kStd, std->rev, kCtx);
    QCOMPARE(del.error().code, QStringLiteral("conflict"));
    QCOMPARE(del.error().details.value(QStringLiteral("reason")).toString(), QStringLiteral("standard"));
  }

  void createUpdateAndBases() {
    auto s = openMemory();
    CalendarDef site = cal("Site");
    site.workWeek = fourTens();
    site.hoursPerDay = 10;
    auto a = s->createCalendar(site, kCtx);
    QVERIFY2(a.ok(), qPrintable(a ? QString() : a.error().message));
    QCOMPARE(a->rev, 1);
    QCOMPARE(s->calendar(a->id).value().workWeek.value(), fourTens());
    auto b = s->createCalendar(cal("Night shift", a->id), kCtx);
    QVERIFY(b.ok());
    // Standard first, then by name.
    QStringList names;
    const QList<CalendarDef> all = s->calendars().value();  // named: a temporary Result would dangle
    for (const CalendarDef& c : all) names << c.name;
    QCOMPARE(names, (QStringList{QStringLiteral("Standard"), QStringLiteral("Night shift"), QStringLiteral("Site")}));
    // No loop of bases, no unknown base, valid fields.
    CalendarDef loop = a.value();
    loop.base = b->id;
    auto refused = s->updateCalendar(loop, 1, kCtx);
    QCOMPARE(refused.error().code, QStringLiteral("validation"));
    QCOMPARE(refused.error().details.value(QStringLiteral("field")).toString(), QStringLiteral("base"));
    QCOMPARE(s->createCalendar(cal("X", QStringLiteral("0123456789abcdef0123")), kCtx).error().code,
             QStringLiteral("validation"));
    CalendarDef none = cal("No days");
    WorkWeek w;
    w.working.fill(false);
    w.hours.fill(0);
    none.workWeek = w;
    QCOMPARE(s->createCalendar(none, kCtx).error().code, QStringLiteral("validation"));
    // Update: a new revision only when something changed.
    CalendarDef renamed = a.value();
    renamed.name = QStringLiteral("Site A");
    QCOMPARE(s->updateCalendar(renamed, 1, kCtx).value().rev, 2);
    QCOMPARE(s->updateCalendar(renamed, 2, kCtx).value().rev, 2);
    QCOMPARE(s->updateCalendar(renamed, 1, kCtx).error().code, QStringLiteral("rev_mismatch"));
    QCOMPARE(s->changesFor(QStringLiteral("calendar"), a->id).value().size(), 2);
  }

  void deleteIsRefusedWhileInUse() {
    auto s = openMemory();
    const CalendarDef site = s->createCalendar(cal("Site"), kCtx).value();
    const CalendarDef child = s->createCalendar(cal("Child", site.id), kCtx).value();
    Project p;
    p.name = QStringLiteral("Kitchen");
    p.calendar = site.id;
    const Project kitchen = s->createProject(p, kCtx).value();
    QCOMPARE(kitchen.calendar, site.id);
    Task t;
    t.project = kitchen.id;
    t.title = QStringLiteral("Night work");
    t.calendar = site.id;
    const Task task = s->createTask(t, kCtx).value();
    QCOMPARE(s->task(task.id).value().calendar, site.id);
    auto used = s->deleteCalendar(site.id, site.rev, kCtx);
    QCOMPARE(used.error().code, QStringLiteral("conflict"));
    const QJsonObject by = used.error().details.value(QStringLiteral("usedBy")).toObject();
    QCOMPARE(by.value(QStringLiteral("projects")).toArray().size(), 1);
    QCOMPARE(by.value(QStringLiteral("tasks")).toArray().size(), 1);
    QCOMPARE(by.value(QStringLiteral("calendars")).toArray().first().toString(), child.id);
    // Unused: deletable, with its exceptions.
    QVERIFY(s->deleteCalendar(child.id, child.rev, kCtx).ok());
    Project back = s->project(kitchen.id).value();
    back.calendar.clear();
    QVERIFY(s->updateProject(back, back.rev, kCtx).ok());
    Task tt = s->task(task.id).value();
    tt.calendar.clear();
    QVERIFY(s->updateTask(tt, tt.rev, kCtx).ok());
    CalendarException e;
    e.calendar = site.id;
    e.from = e.to = QDate(2026, 12, 24);
    const CalendarException added = s->addCalendarException(e, kCtx).value();
    QVERIFY(s->deleteCalendar(site.id, site.rev, kCtx).ok());
    QVERIFY(s->calendarException(added.id, true).value().deleted);
    // A deleted calendar can't be named.
    p.calendar = site.id;
    QCOMPARE(s->createProject(p, kCtx).error().code, QStringLiteral("validation"));
    p.calendar = QStringLiteral("not an id");
    QCOMPARE(s->createProject(p, kCtx).error().code, QStringLiteral("validation"));
  }

  void exceptionsAndHolidays() {
    auto s = openMemory();
    CalendarException e;
    e.calendar = kStd;
    e.from = QDate(2026, 12, 24);
    e.to = QDate(2026, 12, 24);
    e.kind = QStringLiteral("non_working");
    e.label = QStringLiteral("Christmas Eve");
    QVERIFY(s->addCalendarException(e, kCtx).ok());
    e.to = QDate(2026, 12, 20);
    QCOMPARE(s->addCalendarException(e, kCtx).error().code, QStringLiteral("validation"));
    // Import: eleven for 2026; again: none new.
    auto plan = s->planImportHolidays(kStd, QStringLiteral("US"), 2026);
    QCOMPARE(plan.value().size(), 11);
    QVERIFY(s->calendarExceptions(kStd).value().size() == 1);  // a plan writes nothing
    QCOMPARE(s->importHolidays(kStd, QStringLiteral("US"), 2026, kCtx).value().size(), 11);
    QCOMPARE(s->importHolidays(kStd, QStringLiteral("US"), 2026, kCtx).value().size(), 0);
    QCOMPARE(s->calendarExceptions(kStd).value().size(), 12);
    QCOMPARE(s->planImportHolidays(kStd, QStringLiteral("FR"), 2026).error().code, QStringLiteral("validation"));
    QCOMPARE(s->planImportHolidays(kStd, QStringLiteral("US"), 1900).error().code, QStringLiteral("validation"));
    // One deleted, a re-import puts it back.
    const QList<CalendarException> all = s->calendarExceptions(kStd).value();
    CalendarException july;
    for (const CalendarException& x : all) {
      if (x.from == QDate(2026, 7, 3)) july = x;
    }
    QVERIFY(s->deleteCalendarException(july.id, july.rev, kCtx).ok());
    QCOMPARE(s->importHolidays(kStd, QStringLiteral("US"), 2026, kCtx).value().size(), 1);
    // The scheduler's view: Standard takes the holidays.
    const auto cals = s->workCalendars().value();
    QVERIFY(cals.contains(kStd));
    QVERIFY(!cals.value(kStd).isWorking(QDate(2026, 11, 26)));  // Thanksgiving
    QVERIFY(!cals.value(kStd).isWorking(QDate(2026, 12, 24)));  // the non-working day
    QVERIFY(cals.value(kStd).isWorking(QDate(2026, 11, 23)));
  }

  void workCalendarsFollowBases() {
    auto s = openMemory();
    CalendarDef site = cal("Site");
    site.workWeek = fourTens();
    const QString siteId = s->createCalendar(site, kCtx).value().id;
    const QString childId = s->createCalendar(cal("Child", siteId), kCtx).value().id;
    CalendarException e;
    e.calendar = siteId;
    e.from = e.to = QDate(2026, 11, 23);
    QVERIFY(s->addCalendarException(e, kCtx).ok());
    const auto cals = s->workCalendars().value();
    const WorkCalendar child = cals.value(childId);
    QVERIFY(!child.isWorking(QDate(2026, 11, 27)));  // Friday: the site's four-day week
    QVERIFY(!child.isWorking(QDate(2026, 11, 23)));  // the site's exception
    QVERIFY(child.isWorking(QDate(2026, 11, 24)));
  }

  // ---- API ----

  void apiCalendars() {
    Server f;
    Response r = f.call("GET", "/v1/calendars");
    QCOMPARE(r.status, 200);
    QCOMPARE(Server::json(r).value(QStringLiteral("items")).toArray().size(), 1);
    // Create with a work week; the dry run writes nothing.
    const QJsonObject body{{QStringLiteral("name"), QStringLiteral("Site")},
                           {QStringLiteral("workWeek"), weekJson(fourTens())},
                           {QStringLiteral("hoursPerDay"), 10}};
    QCOMPARE(f.call("POST", "/v1/calendars?dryRun=true", body).status, 200);
    QCOMPARE(f.store->calendars().value().size(), 1);
    r = f.call("POST", "/v1/calendars", body);
    QCOMPARE(r.status, 201);
    const QString id = Server::json(r).value(QStringLiteral("id")).toString();
    QCOMPARE(Server::json(r).value(QStringLiteral("workWeek")).toArray().size(), 7);
    const QByteArray path = "/v1/calendars/" + id.toLatin1();
    QCOMPARE(f.call("GET", path).status, 200);
    // Bad weeks: not seven days, not objects, no working day.
    QCOMPARE(f.call("POST", "/v1/calendars", {{QStringLiteral("name"), QStringLiteral("x")},
                                              {QStringLiteral("workWeek"), QJsonArray{1, 2}}}).status, 400);
    QJsonArray off = weekJson(fourTens());
    for (int i = 0; i < 7; ++i) off[i] = QJsonObject{{QStringLiteral("working"), false}, {QStringLiteral("hours"), 0}};
    QCOMPARE(f.call("POST", "/v1/calendars", {{QStringLiteral("name"), QStringLiteral("x")},
                                              {QStringLiteral("workWeek"), off}}).status, 400);
    QCOMPARE(f.call("POST", "/v1/calendars", {{QStringLiteral("name"), QStringLiteral("x")},
                                              {QStringLiteral("colour"), QStringLiteral("red")}}).status, 400);
    // PATCH back to the inherited week; If-Match honored.
    r = f.call("PATCH", path, {{QStringLiteral("workWeek"), QJsonValue()}}, {{"if-match", "1"}});
    QCOMPARE(r.status, 200);
    QVERIFY(Server::json(r).value(QStringLiteral("workWeek")).isNull());
    QCOMPARE(f.call("PATCH", path, {{QStringLiteral("name"), QStringLiteral("y")}}, {{"if-match", "1"}}).status, 412);
    // Exceptions and holidays.
    r = f.call("POST", path + "/exceptions", {{QStringLiteral("from"), QStringLiteral("2026-12-24")},
                                              {QStringLiteral("label"), QStringLiteral("Eve")}});
    QCOMPARE(r.status, 201);
    QCOMPARE(Server::json(r).value(QStringLiteral("to")).toString(), QStringLiteral("2026-12-24"));
    const QByteArray exc = "/v1/calendar-exceptions/" + Server::json(r).value(QStringLiteral("id")).toString().toLatin1();
    QCOMPARE(f.call("POST", path + "/exceptions", {{QStringLiteral("from"), QStringLiteral("24/12/2026")}}).status, 400);
    QCOMPARE(f.call("POST", path + "/exceptions", {{QStringLiteral("from"), QStringLiteral("2026-12-24")},
                                                   {QStringLiteral("calendar"), id}}).status, 400);
    r = f.call("POST", path + "/import-holidays?country=US&year=2027&dryRun=true");
    QCOMPARE(r.status, 200);
    QCOMPARE(Server::json(r).value(QStringLiteral("changes")).toArray().size(), 12);
    r = f.call("POST", path + "/import-holidays?country=US&year=2027");
    QCOMPARE(r.status, 200);
    QCOMPARE(Server::json(r).value(QStringLiteral("items")).toArray().size(), 12);
    QCOMPARE(f.call("POST", path + "/import-holidays?country=US").status, 400);
    r = f.call("GET", path + "/exceptions");
    QCOMPARE(Server::json(r).value(QStringLiteral("items")).toArray().size(), 13);
    QCOMPARE(f.call("DELETE", exc).status, 200);
    // Projects and tasks name a calendar.
    r = f.call("POST", "/v1/projects", {{QStringLiteral("name"), QStringLiteral("Kitchen")}, {QStringLiteral("calendar"), id}});
    QCOMPARE(r.status, 201);
    const QByteArray proj = Server::json(r).value(QStringLiteral("id")).toString().toLatin1();
    QCOMPARE(Server::json(r).value(QStringLiteral("calendar")).toString(), id);
    r = f.call("POST", "/v1/projects/" + proj + "/tasks", {{QStringLiteral("title"), QStringLiteral("t")},
                                                         {QStringLiteral("calendar"), QStringLiteral("0123456789abcdef0123")}});
    QCOMPARE(r.status, 400);
    // In use: 409 with what uses it; the dry run says so too.
    r = f.call("DELETE", path + "?dryRun=true");
    QCOMPARE(r.status, 409);
    QCOMPARE(Server::json(r).value(QStringLiteral("details")).toObject().value(QStringLiteral("reason")).toString(),
             QStringLiteral("in_use"));
    QCOMPARE(f.call("PATCH", "/v1/projects/" + proj, {{QStringLiteral("calendar"), QJsonValue()}}).status, 200);
    r = f.call("DELETE", path + "?dryRun=true");
    QCOMPARE(r.status, 200);
    QCOMPARE(Server::json(r).value(QStringLiteral("changes")).toArray().size(), 13);  // it and 12 holidays
    QCOMPARE(f.call("DELETE", path).status, 200);
    QCOMPARE(f.call("GET", path).status, 404);
    QCOMPARE(code(f.call("DELETE", "/v1/calendars/" + kStd.toLatin1())), QStringLiteral("conflict"));
  }

  void apiWritersNeedAllProjects() {
    Server f;
    Project p;
    p.name = QStringLiteral("Kitchen");
    const QString pid = f.store->createProject(p, kCtx).value().id;
    // A writer limited to one project can read calendars but not write them.
    const QByteArray limited = f.token({{QStringLiteral("name"), QStringLiteral("w")},
                                        {QStringLiteral("kind"), QStringLiteral("device")},
                                        {QStringLiteral("scopes"), QJsonArray{QStringLiteral("write")}},
                                        {QStringLiteral("projects"), QJsonArray{pid}}});
    QCOMPARE(f.call("GET", "/v1/calendars", {}, {}, limited).status, 200);
    Response r = f.call("POST", "/v1/calendars", {{QStringLiteral("name"), QStringLiteral("x")}}, {}, limited);
    QCOMPARE(r.status, 403);
    QCOMPARE(code(r), QStringLiteral("forbidden_project"));
    QCOMPARE(f.call("POST", "/v1/calendars/" + kStd.toLatin1() + "/import-holidays?country=US&year=2026", {}, {}, limited).status,
             403);
    // A read-only agent lacks resources:write entirely.
    const QByteArray reader = f.token({{QStringLiteral("name"), QStringLiteral("r")},
                                       {QStringLiteral("kind"), QStringLiteral("agent")},
                                       {QStringLiteral("projects"), QJsonArray{pid}}});
    QCOMPARE(code(f.call("POST", "/v1/calendars", {{QStringLiteral("name"), QStringLiteral("x")}}, {}, reader)),
             QStringLiteral("forbidden_scope"));
    // A device for every project can.
    const QByteArray device = f.token({{QStringLiteral("name"), QStringLiteral("d")},
                                       {QStringLiteral("kind"), QStringLiteral("device")}});
    QCOMPARE(f.call("POST", "/v1/calendars", {{QStringLiteral("name"), QStringLiteral("x")}}, {}, device).status, 201);
  }

  void calendarJson() {
    CalendarDef c = cal("Site");
    c.id = QStringLiteral("0123456789abcdef0123");
    c.base = kStd;
    c.workWeek = fourTens();
    c.hoursPerDay = 10;
    const CalendarDef back = calendarFromJson(toJson(c));
    QCOMPARE(back.name, c.name);
    QCOMPARE(back.base, c.base);
    QCOMPARE(back.workWeek.value(), fourTens());
    QCOMPARE(back.hoursPerDay, 10.0);
    QVERIFY(toJson(cal("Plain")).value(QStringLiteral("workWeek")).isNull());
    // A malformed week decodes to one validation refuses.
    QJsonObject bad = toJson(c);
    bad[QStringLiteral("workWeek")] = QJsonArray{1, 2, 3};
    QVERIFY(!validateCalendar(calendarFromJson(bad)).isEmpty());
    CalendarException e;
    e.calendar = kStd;
    e.from = QDate(2026, 12, 24);
    e.to = QDate(2026, 12, 26);
    e.kind = QStringLiteral("custom_hours");
    e.hours = 4;
    const CalendarException eb = calendarExceptionFromJson(toJson(e));
    QCOMPARE(eb.from, e.from);
    QCOMPARE(eb.to, e.to);
    QCOMPARE(eb.hours, 4.0);
    // `to` defaults to `from`; loose dates are refused.
    QCOMPARE(calendarExceptionFromJson({{QStringLiteral("from"), QStringLiteral("2026-12-24")}}).to, QDate(2026, 12, 24));
    QVERIFY(!calendarExceptionFromJson({{QStringLiteral("from"), QStringLiteral("2026-12-4")}}).from.isValid());
  }
};

QTEST_GUILESS_MAIN(TestCalendars)
#include "tst_calendars.moc"
