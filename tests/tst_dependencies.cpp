// Dependencies end to end below the desktop (scheduling plan S3; SPEC.md
// §5.1, §5.2, §9.3, §9.4, §11):
//  - the store: create, read, list, update (type and lag only), delete;
//    duplicates; cycles through links and through the task tree, on create,
//    on a move, on a parent change and on a restore; a task delete takes its
//    links with it and a restore brings them back (not those deleted on
//    their own, nor a pair linked again since); a project delete; change rows;
//  - the API: the five operations, dry runs (including what a task delete or
//    restore does to links), confirm, If-Match, 409 cycle with the path,
//    project access, scopes;
//  - MCP: add_dependency.
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

struct Plan {
  std::unique_ptr<SqliteStore> s;
  QString project;
  QHash<QString, QString> id;  // title -> id

  Plan() {
    auto r = SqliteStore::open(QStringLiteral(":memory:"));
    if (!r) qFatal("open: %s", qPrintable(r.error().message));
    s = std::move(r).value();
    Project p;
    p.name = QStringLiteral("Kitchen");
    project = s->createProject(p, kCtx).value().id;
  }
  QString task(const QString& title, const QString& parent = {}) {
    Task t;
    t.project = project;
    t.title = title;
    t.parent = parent.isEmpty() ? QString() : id.value(parent);
    auto r = s->createTask(t, kCtx);
    if (!r) qFatal("createTask: %s", qPrintable(r.error().message));
    id.insert(title, r->id);
    return r->id;
  }
  Dependency dep(const QString& pred, const QString& succ, const QString& type = QStringLiteral("FS"), int lag = 0) {
    Dependency d;
    d.predecessor = id.value(pred, pred);
    d.successor = id.value(succ, succ);
    d.type = type;
    d.lag = lag;
    return d;
  }
  Result<Dependency> link(const QString& pred, const QString& succ, const QString& type = QStringLiteral("FS"),
                          int lag = 0) {
    return s->createDependency(dep(pred, succ, type, lag), kCtx);
  }
  QStringList live() {
    QStringList out;
    const QList<Dependency> links = s->dependencies(project).value();  // a named copy: the Result is a temporary
    for (const Dependency& d : links) {
      out << QStringLiteral("%1>%2").arg(title(d.predecessor), title(d.successor));
    }
    return out;
  }
  QString title(const QString& taskId) const { return id.key(taskId, taskId); }
  Task fresh(const QString& title) { return s->task(id.value(title), true).value(); }
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
  QJsonObject body(const char* pred, const char* succ, const char* type = "FS", int lag = 0) {
    return {{QStringLiteral("predecessor"), plan.id.value(QLatin1String(pred))},
            {QStringLiteral("successor"), plan.id.value(QLatin1String(succ))},
            {QStringLiteral("type"), QLatin1String(type)},
            {QStringLiteral("lag"), lag}};
  }
  QByteArray token(const QJsonObject& b) {
    const Response r = call("POST", "/v1/tokens", b);
    if (r.status != 201) qFatal("token: %s", r.body.constData());
    return json(r).value(QStringLiteral("token")).toString().toLatin1();
  }
};

QString code(const Response& r) { return Server::json(r).value(QStringLiteral("code")).toString(); }

}  // namespace

class TestDependencies : public QObject {
  Q_OBJECT

 private slots:
  // ---- store ----

  void createReadUpdateDelete() {
    Plan p;
    p.task(QStringLiteral("a"));
    p.task(QStringLiteral("b"));
    auto d = p.link(QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("SS"), 2);
    QVERIFY2(d.ok(), qPrintable(d ? QString() : d.error().message));
    QCOMPARE(d->project, p.project);
    QCOMPARE(d->rev, 1);
    QCOMPARE(p.s->dependency(d->id).value().lag, 2);
    QCOMPARE(p.live(), QStringList{QStringLiteral("a>b")});
    // Type and lag change; the tasks don't.
    Dependency u = d.value();
    u.type = QStringLiteral("FF");
    u.lag = -1;
    auto up = p.s->updateDependency(u, 1, kCtx);
    QVERIFY(up.ok());
    QCOMPARE(up->rev, 2);
    QCOMPARE(up->type, QStringLiteral("FF"));
    QCOMPARE(p.s->updateDependency(u, 2, kCtx).value().rev, 2);  // nothing changed: no revision
    u.successor = p.id.value(QStringLiteral("a"));
    QCOMPARE(p.s->updateDependency(u, 2, kCtx).error().code, QStringLiteral("validation"));
    u = up.value();
    QCOMPARE(p.s->updateDependency(u, 1, kCtx).error().code, QStringLiteral("rev_mismatch"));
    auto del = p.s->deleteDependency(d->id, 2, kCtx);
    QVERIFY(del.ok());
    QVERIFY(del->deleted);
    QVERIFY(p.live().isEmpty());
    QCOMPARE(p.s->dependency(d->id).error().code, QStringLiteral("not_found"));
    // Change rows: create, update, delete.
    const auto rows = p.s->changesFor(QStringLiteral("dependency"), d->id).value();
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.at(0).after.value(QStringLiteral("predecessor")).toString(), p.id.value(QStringLiteral("a")));
    QCOMPARE(rows.at(2).after.value(QStringLiteral("deleted")).toBool(), true);
    QCOMPARE(rows.at(2).actor, QStringLiteral("tok_test"));
  }

  void refusals() {
    Plan p;
    p.task(QStringLiteral("a"));
    p.task(QStringLiteral("b"));
    QVERIFY(p.link(QStringLiteral("a"), QStringLiteral("b")).ok());
    // The same pair again, in the same direction.
    auto dup = p.link(QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("SS"));
    QCOMPARE(dup.error().code, QStringLiteral("conflict"));
    QCOMPARE(dup.error().details.value(QStringLiteral("reason")).toString(), QStringLiteral("duplicate"));
    // Self, unknown, other project, bad type and lag.
    QCOMPARE(p.link(QStringLiteral("a"), QStringLiteral("a")).error().code, QStringLiteral("validation"));
    QCOMPARE(p.link(QStringLiteral("a"), QStringLiteral("0123456789abcdef0123")).error().code, QStringLiteral("validation"));
    Project other;
    other.name = QStringLiteral("Other");
    Task t;
    t.project = p.s->createProject(other, kCtx).value().id;
    t.title = QStringLiteral("x");
    p.id.insert(QStringLiteral("x"), p.s->createTask(t, kCtx).value().id);
    auto cross = p.link(QStringLiteral("a"), QStringLiteral("x"));
    QCOMPARE(cross.error().code, QStringLiteral("validation"));
    QCOMPARE(cross.error().details.value(QStringLiteral("field")).toString(), QStringLiteral("successor"));
    QCOMPARE(p.link(QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("SF")).error().code, QStringLiteral("validation"));
    QCOMPARE(p.link(QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("FS"), 4000).error().code,
             QStringLiteral("validation"));
  }

  void cyclesThroughLinksAndTheTree() {
    Plan p;
    p.task(QStringLiteral("a"));
    p.task(QStringLiteral("b"));
    p.task(QStringLiteral("c"));
    QVERIFY(p.link(QStringLiteral("a"), QStringLiteral("b")).ok());
    QVERIFY(p.link(QStringLiteral("b"), QStringLiteral("c")).ok());
    auto loop = p.link(QStringLiteral("c"), QStringLiteral("a"));
    QCOMPARE(loop.error().code, QStringLiteral("cycle"));
    const QJsonArray path = loop.error().details.value(QStringLiteral("path")).toArray();
    QCOMPARE(path.size(), 4);
    QCOMPARE(path.first(), path.last());
    // A task and its own summary.
    p.task(QStringLiteral("s"));
    p.task(QStringLiteral("k"), QStringLiteral("s"));
    QCOMPARE(p.link(QStringLiteral("k"), QStringLiteral("s")).error().code, QStringLiteral("cycle"));
    QCOMPARE(p.link(QStringLiteral("s"), QStringLiteral("k")).error().code, QStringLiteral("cycle"));
    // Moving a linked task under the summary it links to closes a loop.
    p.task(QStringLiteral("m"));
    QVERIFY(p.link(QStringLiteral("m"), QStringLiteral("s")).ok());
    const Task m = p.fresh(QStringLiteral("m"));
    QCOMPARE(p.s->planMove(m.id, p.id.value(QStringLiteral("s")), std::nullopt).error().code, QStringLiteral("cycle"));
    QCOMPARE(p.s->moveTask(m.id, p.id.value(QStringLiteral("s")), std::nullopt, m.rev, kCtx).error().code,
             QStringLiteral("cycle"));
    // So does a parent change through PATCH.
    Task viaPatch = m;
    viaPatch.parent = p.id.value(QStringLiteral("s"));
    QCOMPARE(p.s->updateTask(viaPatch, m.rev, kCtx).error().code, QStringLiteral("cycle"));
    QVERIFY(p.fresh(QStringLiteral("m")).parent.isEmpty());  // nothing written
    // Unrelated moves are fine.
    const Task c = p.fresh(QStringLiteral("c"));
    QVERIFY(p.s->moveTask(c.id, p.id.value(QStringLiteral("s")), std::nullopt, c.rev, kCtx).ok());
  }

  void taskDeleteTakesLinksAndRestoreBringsThemBack() {
    Plan p;
    p.task(QStringLiteral("a"));
    p.task(QStringLiteral("s"));
    p.task(QStringLiteral("k"), QStringLiteral("s"));
    p.task(QStringLiteral("z"));
    QVERIFY(p.link(QStringLiteral("a"), QStringLiteral("k")).ok());
    QVERIFY(p.link(QStringLiteral("k"), QStringLiteral("z")).ok());
    QVERIFY(p.link(QStringLiteral("a"), QStringLiteral("z")).ok());
    // Deleting s deletes k and both of k's links.
    QSet<QString> sub{p.id.value(QStringLiteral("s")), p.id.value(QStringLiteral("k"))};
    QCOMPARE(p.s->linksTouching(p.project, sub).value().size(), 2);
    const Task s = p.fresh(QStringLiteral("s"));
    QVERIFY(p.s->deleteTask(s.id, s.rev, kCtx).ok());
    QCOMPARE(p.live(), QStringList{QStringLiteral("a>z")});
    // Restoring s brings them back.
    QCOMPARE(p.s->planRestoreLinks(s.id).value().size(), 2);
    QVERIFY(p.s->restoreTask(s.id, p.fresh(QStringLiteral("s")).rev, kCtx).ok());
    QCOMPARE(p.live().size(), 3);
    // A link deleted on its own stays deleted when its task comes back.
    Dependency az;
    const QList<Dependency> links = p.s->dependencies(p.project).value();
    for (const Dependency& d : links) {
      if (p.title(d.predecessor) == QLatin1String("k")) az = d;
    }
    QVERIFY(p.s->deleteDependency(az.id, az.rev, kCtx).ok());
    const Task k = p.fresh(QStringLiteral("k"));
    QVERIFY(p.s->deleteTask(k.id, k.rev, kCtx).ok());
    QVERIFY(p.s->restoreTask(k.id, p.fresh(QStringLiteral("k")).rev, kCtx).ok());
    QVERIFY(!p.live().contains(QStringLiteral("k>z")));
    QVERIFY(p.live().contains(QStringLiteral("a>k")));
  }

  void restoreRefusesALoopMadeMeanwhile() {
    // x → y → w; y is deleted (taking both links), then w → x is linked.
    // Bringing y back with its links would close x → y → w → x.
    Plan q;
    q.task(QStringLiteral("x"));
    q.task(QStringLiteral("y"));
    q.task(QStringLiteral("w"));
    QVERIFY(q.link(QStringLiteral("x"), QStringLiteral("y")).ok());
    QVERIFY(q.link(QStringLiteral("y"), QStringLiteral("w")).ok());
    const Task y = q.fresh(QStringLiteral("y"));
    QVERIFY(q.s->deleteTask(y.id, y.rev, kCtx).ok());
    QVERIFY(q.link(QStringLiteral("w"), QStringLiteral("x")).ok());  // fine while y is away
    const auto refused = q.s->restoreTask(y.id, q.fresh(QStringLiteral("y")).rev, kCtx);
    QCOMPARE(refused.error().code, QStringLiteral("cycle"));
    QVERIFY(q.fresh(QStringLiteral("y")).deleted);  // nothing written
    QCOMPARE(q.s->planRestoreLinks(y.id).error().code, QStringLiteral("cycle"));
  }

  void projectDeleteTakesLinks() {
    Plan p;
    p.task(QStringLiteral("a"));
    p.task(QStringLiteral("b"));
    QVERIFY(p.link(QStringLiteral("a"), QStringLiteral("b")).ok());
    const Project pr = p.s->project(p.project).value();
    QVERIFY(p.s->deleteProject(p.project, pr.rev, kCtx).ok());
    QVERIFY(p.s->dependencies(p.project).value().isEmpty());
    QCOMPARE(p.s->dependencies(p.project, true).value().size(), 1);
  }

  // ---- API ----

  void apiCrud() {
    Server f;
    f.plan.task(QStringLiteral("a"));
    f.plan.task(QStringLiteral("b"));
    // Dry run first: nothing written.
    Response r = f.call("POST", "/v1/dependencies?dryRun=true", f.body("a", "b", "SS", 1));
    QCOMPARE(r.status, 200);
    const QJsonArray changes = Server::json(r).value(QStringLiteral("changes")).toArray();
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changes[0].toObject().value(QStringLiteral("entityKind")).toString(), QStringLiteral("dependency"));
    QCOMPARE(changes[0].toObject().value(QStringLiteral("after")).toObject().value(QStringLiteral("project")).toString(),
             f.plan.project);
    QVERIFY(f.plan.live().isEmpty());
    r = f.call("POST", "/v1/dependencies", f.body("a", "b", "SS", 1));
    QCOMPARE(r.status, 201);
    const QString id = Server::json(r).value(QStringLiteral("id")).toString();
    const QByteArray path = "/v1/dependencies/" + id.toLatin1();
    QCOMPARE(f.call("GET", path).status, 200);
    r = f.call("GET", "/v1/projects/" + f.plan.project.toLatin1() + "/dependencies");
    QCOMPARE(Server::json(r).value(QStringLiteral("items")).toArray().size(), 1);
    r = f.call("GET", "/v1/projects/" + f.plan.project.toLatin1() + "/dependencies?task=" +
                          f.plan.id.value(QStringLiteral("b")).toLatin1());
    QCOMPARE(Server::json(r).value(QStringLiteral("items")).toArray().size(), 1);
    // PATCH: type and lag, If-Match honoured; the tasks are read-only.
    r = f.call("PATCH", path, {{QStringLiteral("lag"), 3}}, {{"if-match", "1"}});
    QCOMPARE(r.status, 200);
    QCOMPARE(Server::json(r).value(QStringLiteral("lag")).toInt(), 3);
    QCOMPARE(f.call("PATCH", path, {{QStringLiteral("lag"), 4}}, {{"if-match", "1"}}).status, 412);
    r = f.call("PATCH", path, {{QStringLiteral("successor"), f.plan.id.value(QStringLiteral("a"))}});
    QCOMPARE(r.status, 400);
    QCOMPARE(f.call("PATCH", path, {{QStringLiteral("type"), QStringLiteral("XX")}}).status, 400);
    QCOMPARE(f.call("PATCH", path, {{QStringLiteral("lag"), 1.5}}).status, 400);
    QCOMPARE(f.call("POST", "/v1/dependencies", {{QStringLiteral("predecessor"), 3}}).status, 400);
    QCOMPARE(f.call("POST", "/v1/dependencies", {{QStringLiteral("weight"), 3}}).status, 400);
    // Duplicate: 409.
    r = f.call("POST", "/v1/dependencies", f.body("a", "b"));
    QCOMPARE(r.status, 409);
    QCOMPARE(code(r), QStringLiteral("conflict"));
    // DELETE: destructive, dry run gives a confirmId.
    r = f.call("DELETE", path + "?dryRun=true");
    QCOMPARE(r.status, 200);
    QVERIFY(!Server::json(r).value(QStringLiteral("confirmId")).toString().isEmpty());
    QCOMPARE(f.call("DELETE", path).status, 200);
    QCOMPARE(f.call("GET", path).status, 404);
  }

  void apiCycleAndTaskOperations() {
    Server f;
    f.plan.task(QStringLiteral("a"));
    f.plan.task(QStringLiteral("b"));
    f.plan.task(QStringLiteral("s"));
    f.plan.task(QStringLiteral("k"), QStringLiteral("s"));
    QCOMPARE(f.call("POST", "/v1/dependencies", f.body("a", "b")).status, 201);
    Response r = f.call("POST", "/v1/dependencies", f.body("b", "a"));
    QCOMPARE(r.status, 409);
    QCOMPARE(code(r), QStringLiteral("cycle"));
    QCOMPARE(Server::json(r).value(QStringLiteral("details")).toObject().value(QStringLiteral("path")).toArray().size(), 3);
    QCOMPARE(f.call("POST", "/v1/dependencies?dryRun=true", f.body("k", "s")).status, 409);
    // Moving b under s while b → s... first link a → s, then move a under s.
    QCOMPARE(f.call("POST", "/v1/dependencies", f.body("a", "s")).status, 201);
    const QByteArray a = f.plan.id.value(QStringLiteral("a")).toLatin1();
    r = f.call("POST", "/v1/tasks/" + a + "/move?dryRun=true", {{QStringLiteral("parent"), f.plan.id.value(QStringLiteral("s"))}});
    QCOMPARE(r.status, 409);
    r = f.call("PATCH", "/v1/tasks/" + a + "?dryRun=true", {{QStringLiteral("parent"), f.plan.id.value(QStringLiteral("s"))}});
    QCOMPARE(r.status, 409);
    // A task delete's dry run lists its links; the restore's brings them back.
    const QByteArray b = f.plan.id.value(QStringLiteral("b")).toLatin1();
    r = f.call("DELETE", "/v1/tasks/" + b + "?dryRun=true");
    QStringList kinds;
    for (const auto& c : Server::json(r).value(QStringLiteral("changes")).toArray()) {
      kinds << c.toObject().value(QStringLiteral("entityKind")).toString();
    }
    QCOMPARE(kinds, (QStringList{QStringLiteral("task"), QStringLiteral("dependency")}));
    QCOMPARE(f.call("DELETE", "/v1/tasks/" + b).status, 200);
    QCOMPARE(f.plan.live(), QStringList{QStringLiteral("a>s")});
    r = f.call("POST", "/v1/tasks/" + b + "/restore?dryRun=true");
    kinds.clear();
    for (const auto& c : Server::json(r).value(QStringLiteral("changes")).toArray()) {
      if (c.toObject().value(QStringLiteral("op")).toString() == QLatin1String("restore")) {
        kinds << c.toObject().value(QStringLiteral("entityKind")).toString();
      }
    }
    QCOMPARE(kinds, (QStringList{QStringLiteral("task"), QStringLiteral("dependency")}));
    QCOMPARE(f.call("POST", "/v1/tasks/" + b + "/restore").status, 200);
    QCOMPARE(f.plan.live().size(), 2);
  }

  void apiAccessAndConfirm() {
    Server f;
    f.plan.task(QStringLiteral("a"));
    f.plan.task(QStringLiteral("b"));
    Project other;
    other.name = QStringLiteral("Other");
    const QString otherId = f.plan.s->createProject(other, kCtx).value().id;
    // A read-only agent for the project: can list, can't write.
    const QByteArray reader = f.token({{QStringLiteral("name"), QStringLiteral("r")},
                                       {QStringLiteral("kind"), QStringLiteral("agent")},
                                       {QStringLiteral("projects"), QJsonArray{f.plan.project}}});
    QCOMPARE(f.call("GET", "/v1/projects/" + f.plan.project.toLatin1() + "/dependencies", {}, {}, reader).status, 200);
    Response r = f.call("POST", "/v1/dependencies", f.body("a", "b"), {}, reader);
    QCOMPARE(r.status, 403);
    // A writer limited to another project can't link these tasks.
    const QByteArray stranger = f.token({{QStringLiteral("name"), QStringLiteral("w")},
                                         {QStringLiteral("kind"), QStringLiteral("agent")},
                                         {QStringLiteral("scopes"), QJsonArray{QStringLiteral("write")}},
                                         {QStringLiteral("projects"), QJsonArray{otherId}}});
    r = f.call("POST", "/v1/dependencies", f.body("a", "b"), {}, stranger);
    QCOMPARE(r.status, 403);
    QCOMPARE(code(r), QStringLiteral("forbidden_project"));
    // A writer for this project whose token needs confirmations.
    const QByteArray writer = f.token({{QStringLiteral("name"), QStringLiteral("w2")},
                                       {QStringLiteral("kind"), QStringLiteral("agent")},
                                       {QStringLiteral("scopes"), QJsonArray{QStringLiteral("write")}},
                                       {QStringLiteral("projects"), QJsonArray{f.plan.project}}});
    r = f.call("POST", "/v1/dependencies", f.body("a", "b"), {}, writer);
    QCOMPARE(r.status, 201);
    const QByteArray path = "/v1/dependencies/" + Server::json(r).value(QStringLiteral("id")).toString().toLatin1();
    r = f.call("DELETE", path, {}, {}, writer);
    QCOMPARE(r.status, 409);
    QCOMPARE(code(r), QStringLiteral("confirm_required"));
    const QString confirm =
        Server::json(f.call("DELETE", path + "?dryRun=true", {}, {}, writer)).value(QStringLiteral("confirmId")).toString();
    QCOMPARE(f.call("DELETE", path, {}, {{"x-zproject-confirm", confirm.toLatin1()}}, writer).status, 200);
  }

  void mcpAddDependency() {
    Server f;
    f.plan.task(QStringLiteral("a"));
    f.plan.task(QStringLiteral("b"));
    const QByteArray writer = f.token({{QStringLiteral("name"), QStringLiteral("agent")},
                                       {QStringLiteral("kind"), QStringLiteral("agent")},
                                       {QStringLiteral("scopes"), QJsonArray{QStringLiteral("write")}},
                                       {QStringLiteral("projects"), QJsonArray{f.plan.project}}});
    auto rpc = [&](const QJsonObject& params, const QByteArray& token) {
      const Response r = f.call("POST", "/mcp",
                                {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                 {QStringLiteral("id"), 1},
                                 {QStringLiteral("method"), QStringLiteral("tools/call")},
                                 {QStringLiteral("params"), params}},
                                {}, token);
      return Server::json(r);
    };
    QJsonObject res = rpc({{QStringLiteral("name"), QStringLiteral("add_dependency")},
                           {QStringLiteral("arguments"),
                            QJsonObject{{QStringLiteral("predecessor"), f.plan.id.value(QStringLiteral("a"))},
                                        {QStringLiteral("successor"), f.plan.id.value(QStringLiteral("b"))},
                                        {QStringLiteral("lag"), 2}}}},
                          writer)
                          .value(QStringLiteral("result"))
                          .toObject();
    QVERIFY2(!res.value(QStringLiteral("isError")).toBool(), qPrintable(QJsonDocument(res).toJson()));
    QCOMPARE(res.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("lag")).toInt(), 2);
    // The loop back is refused, as an error result the agent can read.
    res = rpc({{QStringLiteral("name"), QStringLiteral("add_dependency")},
               {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("predecessor"), f.plan.id.value(QStringLiteral("b"))},
                                                         {QStringLiteral("successor"), f.plan.id.value(QStringLiteral("a"))}}}},
              writer)
              .value(QStringLiteral("result"))
              .toObject();
    QVERIFY(res.value(QStringLiteral("isError")).toBool());
    // Bad ids are refused before any call.
    const QJsonObject bad = rpc({{QStringLiteral("name"), QStringLiteral("add_dependency")},
                                 {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("predecessor"), QStringLiteral("x")},
                                                                           {QStringLiteral("successor"), QStringLiteral("y")}}}},
                                writer);
    QVERIFY(bad.contains(QStringLiteral("error")));
  }
};

QTEST_GUILESS_MAIN(TestDependencies)
#include "tst_dependencies.moc"
