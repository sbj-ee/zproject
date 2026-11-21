// /v1 and /mcp through Api::handle (SPEC.md §9, §11, §14, §19): the contract
// with api/openapi.yaml, token scopes, project allowlists, expiry and
// revocation, rate limits, dry-run and confirm, If-Match, the audit log and
// the MCP tools. Tokens here are generated per test run; none is real.
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTest>

#include "server/Api.hpp"
#include "server/Mcp.hpp"
#include "store/Audit.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::server;
using http::Request;
using http::Response;

namespace {

struct Fixture {
  std::unique_ptr<store::SqliteStore> store;
  QDateTime clock = QDateTime::fromString(QStringLiteral("2026-11-21T12:00:00Z"), Qt::ISODate);
  qint64 ms = 0;
  std::unique_ptr<Api> api;
  QByteArray owner;

  Fixture() {
    auto s = store::SqliteStore::open(QStringLiteral(":memory:"));
    if (!s.ok()) qFatal("open: %s", qPrintable(s.error().message));
    store = std::move(s).value();
    ApiOptions o;
    o.allowedHosts = {QStringLiteral("127.0.0.1:8788"), QStringLiteral("server-host.example.ts.net")};
    o.now = [this] { return clock; };
    o.rateClock = [this] { return ms; };
    api = std::make_unique<Api>(*store, o);
    owner = api->createOwnerToken(QStringLiteral("owner")).value().toLatin1();
  }

  Response call(const QByteArray& method, const QByteArray& target, const QByteArray& token = {},
                const QJsonObject& body = {}, const QList<QPair<QByteArray, QByteArray>>& extra = {}) {
    Request r;
    r.method = method;
    r.target = target;
    const qsizetype q = target.indexOf('?');
    r.path = QString::fromUtf8(q < 0 ? target : target.left(q));
    if (q >= 0) r.query = QUrlQuery(QString::fromUtf8(target.mid(q + 1)));
    r.headers.append({QByteArray("host"), QByteArray("127.0.0.1:8788")});
    if (!token.isEmpty()) r.headers.append({QByteArray("authorization"), "Bearer " + token});
    if (!body.isEmpty() || method == "POST" || method == "PATCH") {
      r.headers.append({QByteArray("content-type"), QByteArray("application/json")});
      r.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    }
    for (const auto& h : extra) {
      bool replaced = false;
      for (auto& existing : r.headers) {
        if (existing.first == h.first) {
          existing.second = h.second;
          replaced = true;
        }
      }
      if (!replaced) r.headers.append(h);
    }
    r.peer = QHostAddress(QHostAddress::LocalHost);
    return api->handle(r);
  }

  static QJsonObject json(const Response& r) { return QJsonDocument::fromJson(r.body).object(); }

  QString createProject(const QString& name) {
    const Response r = call("POST", "/v1/projects", owner, {{QStringLiteral("name"), name}});
    if (r.status != 201) qFatal("createProject: %d %s", r.status, r.body.constData());
    return json(r).value(QStringLiteral("id")).toString();
  }
  QString createTask(const QString& project, const QString& title, const QString& parent = {}) {
    QJsonObject b{{QStringLiteral("title"), title}};
    if (!parent.isEmpty()) b.insert(QStringLiteral("parent"), parent);
    const Response r = call("POST", "/v1/projects/" + project.toLatin1() + "/tasks", owner, b);
    if (r.status != 201) qFatal("createTask: %d %s", r.status, r.body.constData());
    return json(r).value(QStringLiteral("id")).toString();
  }
  QByteArray createToken(const QJsonObject& body) {
    const Response r = call("POST", "/v1/tokens", owner, body);
    if (r.status != 201) qFatal("createToken: %d %s", r.status, r.body.constData());
    return json(r).value(QStringLiteral("token")).toString().toLatin1();
  }
  QList<store::AuditEntry> auditRows() {
    store::AuditLog log(store->database());
    return log.since(0, 500).value();
  }
};

QString code(const Response& r) { return Fixture::json(r).value(QStringLiteral("code")).toString(); }

}  // namespace

class TestApi : public QObject {
  Q_OBJECT
 private slots:
  // Every route is in api/openapi.yaml with the same operationId, scope,
  // destructive flag and dry-run flag, and nothing else is.
  void contractWithOpenApi() {
    const QJsonObject doc = QJsonDocument::fromJson(openApiJson()).object();
    QCOMPARE(doc.value(QStringLiteral("openapi")).toString(), QStringLiteral("3.1.0"));
    QSet<QString> spec;
    const QJsonObject paths = doc.value(QStringLiteral("paths")).toObject();
    for (auto p = paths.constBegin(); p != paths.constEnd(); ++p) {
      const QJsonObject item = p.value().toObject();
      for (const char* m : {"get", "post", "patch", "put", "delete"}) {
        if (!item.contains(QLatin1String(m))) continue;
        const QJsonObject op = item.value(QLatin1String(m)).toObject();
        spec.insert(QStringLiteral("%1 %2 %3 %4 %5 %6")
                        .arg(QString::fromLatin1(m).toUpper(), p.key(),
                             op.value(QStringLiteral("operationId")).toString(),
                             op.value(QStringLiteral("x-zproject-scope")).toString())
                        .arg(op.value(QStringLiteral("x-zproject-destructive")).toBool())
                        .arg(op.value(QStringLiteral("x-zproject-dry-run")).toBool()));
        QVERIFY2(op.contains(QStringLiteral("x-zproject-scope")) && op.contains(QStringLiteral("x-zproject-destructive")) &&
                     op.contains(QStringLiteral("x-zproject-dry-run")),
                 qPrintable(p.key()));
      }
    }
    QSet<QString> server;
    for (const RouteInfo& r : Api::routes()) {
      if (r.operationId == QLatin1String("mcp")) continue;  // /mcp is JSON-RPC, not part of the OpenAPI document
      server.insert(QStringLiteral("%1 %2 %3 %4 %5 %6")
                        .arg(QString::fromLatin1(r.method), r.pattern, r.operationId,
                             r.scope.isEmpty() ? QStringLiteral("any") : r.scope)
                        .arg(r.destructive)
                        .arg(r.dryRun));
    }
    QCOMPARE(server - spec, QSet<QString>());
    QCOMPARE(spec - server, QSet<QString>());
  }

  void healthNeedsNoTokenButOpenApiDoes() {
    Fixture f;
    Response r = f.call("GET", "/v1/health");
    QCOMPARE(r.status, 200);
    const QJsonObject h = Fixture::json(r);
    QCOMPARE(h.keys(), (QStringList{QStringLiteral("apiVersion"), QStringLiteral("ok"), QStringLiteral("schemaVersion"),
                                    QStringLiteral("version")}));
    QCOMPARE(f.call("GET", "/v1/openapi.json").status, 401);
    r = f.call("GET", "/v1/openapi.json", f.owner);
    QCOMPARE(r.status, 200);
    QCOMPARE(r.body, openApiJson());
  }

  void transportGuards() {
    Fixture f;
    QCOMPARE(f.call("GET", "/v1/health", {}, {}, {{"host", "evil.example"}}).status, 400);
    QCOMPARE(f.call("GET", "/v1/health", {}, {}, {{"host", "server-host.example.ts.net"}}).status, 200);
    QCOMPARE(f.call("GET", "/v1/health", {}, {}, {{"host", "SERVER-HOST.example.ts.net:443"}}).status, 200);
    Response r = f.call("GET", "/v1/projects", f.owner, {}, {{"origin", "https://evil.example"}});
    QCOMPARE(r.status, 403);
    QCOMPARE(code(r), QStringLiteral("forbidden_origin"));
    QCOMPARE(f.call("GET", "/v1/nope", f.owner).status, 404);
    r = f.call("PUT", "/v1/projects", f.owner);
    QCOMPARE(r.status, 405);
    // Writes must be JSON.
    r = f.call("POST", "/v1/projects", f.owner, {{QStringLiteral("name"), QStringLiteral("x")}},
               {{"content-type", "text/plain"}});
    QCOMPARE(r.status, 415);
  }

  void authenticationFailures() {
    Fixture f;
    Response r = f.call("GET", "/v1/projects");
    QCOMPARE(r.status, 401);
    QCOMPARE(code(r), QStringLiteral("unauthenticated"));
    // Right id, wrong secret.
    QByteArray wrong = f.owner;
    wrong[wrong.size() - 1] = wrong.endsWith('A') ? 'B' : 'A';
    QCOMPARE(f.call("GET", "/v1/projects", wrong).status, 401);
    QCOMPARE(f.call("GET", "/v1/projects", "zpt_garbage").status, 401);
    // 10 failures per minute per source, then 429 even for a valid-looking token.
    for (int i = 0; i < 7; ++i) QCOMPARE(f.call("GET", "/v1/projects", wrong).status, 401);
    r = f.call("GET", "/v1/projects", wrong);
    QCOMPARE(r.status, 429);
    
    // A valid token is unaffected.
    QCOMPARE(f.call("GET", "/v1/projects", f.owner).status, 200);
    // Failures are audited, without any secret.
    bool sawFailure = false;
    for (const auto& e : f.auditRows()) {
      if (e.status == 401) sawFailure = true;
      QVERIFY(!e.path.contains(QStringLiteral("zpt_")));
    }
    QVERIFY(sawFailure);
  }

  void projectsAndTasksCrud() {
    Fixture f;
    const QString p = f.createProject(QStringLiteral("Garage"));
    const QByteArray pp = "/v1/projects/" + p.toLatin1();
    Response r = f.call("GET", pp, f.owner);
    QCOMPARE(r.status, 200);
    QCOMPARE(Fixture::json(r).value(QStringLiteral("rev")).toInt(), 1);

    // Unknown and read-only fields are refused; wrong types too.
    QCOMPARE(code(f.call("PATCH", pp, f.owner, {{QStringLiteral("bogus"), 1}})), QStringLiteral("validation"));
    QCOMPARE(f.call("PATCH", pp, f.owner, {{QStringLiteral("rev"), 5}}).status, 400);
    QCOMPARE(f.call("PATCH", pp, f.owner, {{QStringLiteral("name"), 5}}).status, 400);
    QCOMPARE(f.call("PATCH", pp, f.owner, {{QStringLiteral("name"), QString()}}).status, 400);

    // If-Match
    r = f.call("PATCH", pp, f.owner, {{QStringLiteral("rag"), QStringLiteral("amber")}}, {{"if-match", "1"}});
    QCOMPARE(r.status, 200);
    QCOMPARE(Fixture::json(r).value(QStringLiteral("rev")).toInt(), 2);
    r = f.call("PATCH", pp, f.owner, {{QStringLiteral("rag"), QStringLiteral("red")}}, {{"if-match", "\"1\""}});
    QCOMPARE(r.status, 412);
    QCOMPARE(code(r), QStringLiteral("rev_mismatch"));

    // Dry run writes nothing.
    r = f.call("POST", pp + "/tasks?dryRun=true", f.owner, {{QStringLiteral("title"), QStringLiteral("Dig")}});
    QCOMPARE(r.status, 200);
    QCOMPARE(Fixture::json(r).value(QStringLiteral("changes")).toArray().size(), 1);
    QVERIFY(!Fixture::json(r).contains(QStringLiteral("confirmId")));  // not destructive
    QCOMPARE(Fixture::json(f.call("GET", pp + "/tasks", f.owner)).value(QStringLiteral("items")).toArray().size(), 0);

    const QString a = f.createTask(p, QStringLiteral("Foundation"));
    const QString a1 = f.createTask(p, QStringLiteral("Dig"), a);
    f.createTask(p, QStringLiteral("Pour"), a);
    f.createTask(p, QStringLiteral("Framing"));
    r = f.call("GET", pp + "/wbs", f.owner);
    QCOMPARE(r.status, 200);
    QStringList codes;
    for (const auto& v : Fixture::json(r).value(QStringLiteral("items")).toArray()) {
      codes << v.toObject().value(QStringLiteral("wbs")).toString();
    }
    QCOMPARE(codes, (QStringList{QStringLiteral("1"), QStringLiteral("1.1"), QStringLiteral("1.2"), QStringLiteral("2")}));

    // Pagination
    r = f.call("GET", pp + "/tasks?limit=3", f.owner);
    QCOMPARE(Fixture::json(r).value(QStringLiteral("items")).toArray().size(), 3);
    const QString cursor = Fixture::json(r).value(QStringLiteral("nextCursor")).toString();
    QVERIFY(!cursor.isEmpty());
    r = f.call("GET", pp + "/tasks?limit=3&cursor=" + cursor.toLatin1(), f.owner);
    QCOMPARE(Fixture::json(r).value(QStringLiteral("items")).toArray().size(), 1);
    QVERIFY(Fixture::json(r).value(QStringLiteral("nextCursor")).isNull());
    QCOMPARE(f.call("GET", pp + "/tasks?limit=501", f.owner).status, 400);
    // Search
    r = f.call("GET", pp + "/tasks?q=dig", f.owner);
    QCOMPARE(Fixture::json(r).value(QStringLiteral("items")).toArray().size(), 1);

    // Update and history
    const QByteArray tp = "/v1/tasks/" + a1.toLatin1();
    QCOMPARE(f.call("PATCH", tp, f.owner, {{QStringLiteral("percentComplete"), 50}, {QStringLiteral("actualStart"), QStringLiteral("2026-11-20")}}).status, 200);
    QCOMPARE(f.call("PATCH", tp, f.owner, {{QStringLiteral("project"), p}}).status, 400);
    r = f.call("GET", tp + "/history", f.owner);
    QCOMPARE(Fixture::json(r).value(QStringLiteral("items")).toArray().size(), 2);

    // Delete the parent: its subtree goes too (owner: no confirm needed).
    r = f.call("DELETE", "/v1/tasks/" + a.toLatin1() + "?dryRun=true", f.owner);
    QCOMPARE(Fixture::json(r).value(QStringLiteral("changes")).toArray().size(), 3);
    QVERIFY(Fixture::json(r).contains(QStringLiteral("confirmId")));
    QCOMPARE(f.call("DELETE", "/v1/tasks/" + a.toLatin1(), f.owner).status, 200);
    QCOMPARE(f.call("GET", tp, f.owner).status, 404);
    QCOMPARE(Fixture::json(f.call("GET", pp + "/tasks", f.owner)).value(QStringLiteral("items")).toArray().size(), 1);

    // Change feed
    r = f.call("GET", "/v1/changes?since=0&limit=2", f.owner);
    QCOMPARE(Fixture::json(r).value(QStringLiteral("items")).toArray().size(), 2);
    QVERIFY(Fixture::json(r).value(QStringLiteral("more")).toBool());
  }

  void agentTokenDefaultsAndLimits() {
    Fixture f;
    const QString p1 = f.createProject(QStringLiteral("One"));
    const QString p2 = f.createProject(QStringLiteral("Two"));
    f.createTask(p1, QStringLiteral("t1"));
    const QString t2 = f.createTask(p2, QStringLiteral("t2"));

    // Policy refusals.
    QCOMPARE(f.call("POST", "/v1/tokens", f.owner, {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("kind"), QStringLiteral("agent")}}).status, 400);  // no projects
    QCOMPARE(f.call("POST", "/v1/tokens", f.owner, {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("kind"), QStringLiteral("agent")}, {QStringLiteral("projects"), QJsonArray{QStringLiteral("*")}}}).status, 400);
    QCOMPARE(f.call("POST", "/v1/tokens", f.owner, {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("kind"), QStringLiteral("agent")}, {QStringLiteral("projects"), QJsonArray{p1}}, {QStringLiteral("scopes"), QJsonArray{QStringLiteral("admin")}}}).status, 400);
    QCOMPARE(f.call("POST", "/v1/tokens", f.owner, {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("kind"), QStringLiteral("agent")}, {QStringLiteral("projects"), QJsonArray{p1}}, {QStringLiteral("expiresInDays"), 91}}).status, 400);
    QCOMPARE(f.call("POST", "/v1/tokens", f.owner, {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("kind"), QStringLiteral("owner")}}).status, 400);
    QCOMPARE(f.call("POST", "/v1/tokens", f.owner, {{QStringLiteral("name"), QStringLiteral("a")}, {QStringLiteral("kind"), QStringLiteral("agent")}, {QStringLiteral("projects"), QJsonArray{QStringLiteral("ffffffffffffffffffff")}}}).status, 400);

    Response r = f.call("POST", "/v1/tokens", f.owner,
                        {{QStringLiteral("name"), QStringLiteral("agent-one")}, {QStringLiteral("kind"), QStringLiteral("agent")},
                         {QStringLiteral("projects"), QJsonArray{p1}}});
    QCOMPARE(r.status, 201);
    const QJsonObject meta = Fixture::json(r);
    QCOMPARE(meta.value(QStringLiteral("scopes")).toArray(), QJsonArray{QStringLiteral("read")});
    QCOMPARE(meta.value(QStringLiteral("expiresAt")).toString(), QStringLiteral("2027-02-19T12:00:00.000Z"));  // 90 days
    QVERIFY(meta.value(QStringLiteral("requireConfirm")).toBool());
    const QByteArray agent = meta.value(QStringLiteral("token")).toString().toLatin1();

    // Read-only, own project only.
    QCOMPARE(f.call("GET", "/v1/projects/" + p1.toLatin1(), agent).status, 200);
    r = f.call("GET", "/v1/projects/" + p2.toLatin1(), agent);
    QCOMPARE(r.status, 403);
    QCOMPARE(code(r), QStringLiteral("forbidden_project"));
    QCOMPARE(f.call("GET", "/v1/tasks/" + t2.toLatin1(), agent).status, 403);
    r = f.call("GET", "/v1/projects", agent);
    QCOMPARE(Fixture::json(r).value(QStringLiteral("items")).toArray().size(), 1);
    r = f.call("POST", "/v1/projects/" + p1.toLatin1() + "/tasks", agent, {{QStringLiteral("title"), QStringLiteral("x")}});
    QCOMPARE(r.status, 403);
    QCOMPARE(code(r), QStringLiteral("forbidden_scope"));
    QCOMPARE(f.call("GET", "/v1/tokens", agent).status, 403);
    QCOMPARE(f.call("GET", "/v1/audit", agent).status, 403);
    QCOMPARE(f.call("GET", "/v1/changes", agent).status, 403);  // limited to named projects

    // The token list shows metadata only.
    r = f.call("GET", "/v1/tokens", f.owner);
    QVERIFY(!r.body.contains("secretHash"));
    QVERIFY(!r.body.contains("zpt_"));

    // Expiry: fine on day 89, refused after 90 days.
    f.clock = f.clock.addDays(89);
    QCOMPARE(f.call("GET", "/v1/projects", agent).status, 200);
    f.clock = f.clock.addDays(2);
    r = f.call("GET", "/v1/projects", agent);
    QCOMPARE(r.status, 401);
    QVERIFY(Fixture::json(r).value(QStringLiteral("error")).toString().contains(QStringLiteral("expired")));
  }

  void revocationIsImmediate() {
    Fixture f;
    const QString p = f.createProject(QStringLiteral("One"));
    const QByteArray dev = f.createToken({{QStringLiteral("name"), QStringLiteral("laptop")}, {QStringLiteral("kind"), QStringLiteral("device")}});
    QCOMPARE(f.call("GET", "/v1/projects/" + p.toLatin1(), dev).status, 200);
    const QByteArray id = dev.mid(4, 20);
    QCOMPARE(f.call("DELETE", "/v1/tokens/" + id, f.owner).status, 200);
    QCOMPARE(f.call("GET", "/v1/projects/" + p.toLatin1(), dev).status, 401);
  }

  void confirmFlow() {
    Fixture f;
    const QString p = f.createProject(QStringLiteral("One"));
    const QString t = f.createTask(p, QStringLiteral("doomed"));
    const QString other = f.createTask(p, QStringLiteral("other"));
    const QByteArray agent = f.createToken({{QStringLiteral("name"), QStringLiteral("writer")}, {QStringLiteral("kind"), QStringLiteral("agent")},
                                            {QStringLiteral("projects"), QJsonArray{p}}, {QStringLiteral("scopes"), QJsonArray{QStringLiteral("write")}}});
    const QByteArray path = "/v1/tasks/" + t.toLatin1();

    Response r = f.call("DELETE", path, agent);
    QCOMPARE(r.status, 409);
    QCOMPARE(code(r), QStringLiteral("confirm_required"));

    r = f.call("DELETE", path + "?dryRun=true", agent);
    QCOMPARE(r.status, 200);
    const QByteArray confirm = Fixture::json(r).value(QStringLiteral("confirmId")).toString().toLatin1();
    QVERIFY(!confirm.isEmpty());
    QCOMPARE(f.call("GET", path, agent).status, 200);  // the dry run deleted nothing

    // Bound to the exact request: another task's delete is refused.
    QCOMPARE(f.call("DELETE", "/v1/tasks/" + other.toLatin1(), agent, {}, {{"x-zproject-confirm", confirm}}).status, 409);
    // ... and to the token: the owner cannot use it.
    QCOMPARE(f.call("DELETE", path, f.owner, {}, {{"x-zproject-confirm", confirm}}).status, 409);
    r = f.call("DELETE", path, agent, {}, {{"x-zproject-confirm", confirm}});
    QCOMPARE(r.status, 200);
    QVERIFY(Fixture::json(r).value(QStringLiteral("deleted")).toBool());
    // Single use.
    r = f.call("DELETE", "/v1/tasks/" + other.toLatin1() + "?dryRun=true", agent);
    const QByteArray c2 = Fixture::json(r).value(QStringLiteral("confirmId")).toString().toLatin1();
    QCOMPARE(f.call("DELETE", path, agent, {}, {{"x-zproject-confirm", confirm}}).status, 404);  // already deleted
    // Expiry: 5 minutes.
    f.clock = f.clock.addSecs(301);
    QCOMPARE(f.call("DELETE", "/v1/tasks/" + other.toLatin1(), agent, {}, {{"x-zproject-confirm", c2}}).status, 409);

    // The audit row of the confirmed delete names the confirmation.
    bool found = false;
    for (const auto& e : f.auditRows()) {
      if (e.method == QLatin1String("DELETE") && e.status == 200 && e.entityId == t && !e.dryRun) {
        QCOMPARE(e.confirmId, QString::fromLatin1(confirm));
        QCOMPARE(e.tokenKind, QStringLiteral("agent"));
        QCOMPARE(e.after, QStringLiteral("{\"deleted\":true}"));
        found = true;
      }
    }
    QVERIFY(found);
  }

  void rateLimitedAgent() {
    Fixture f;
    const QString p = f.createProject(QStringLiteral("One"));
    const QByteArray agent = f.createToken({{QStringLiteral("name"), QStringLiteral("fast")}, {QStringLiteral("kind"), QStringLiteral("agent")},
                                            {QStringLiteral("projects"), QJsonArray{p}}, {QStringLiteral("rateLimit"), QJsonObject{{QStringLiteral("read"), 3}}}});
    for (int i = 0; i < 3; ++i) QCOMPARE(f.call("GET", "/v1/projects", agent).status, 200);
    Response r = f.call("GET", "/v1/projects", agent);
    QCOMPARE(r.status, 429);
    QCOMPARE(code(r), QStringLiteral("rate_limited"));
    bool retry = false;
    for (const auto& h : r.headers) retry = retry || h.first == "Retry-After";
    QVERIFY(retry);
    f.ms += 20'000;
    QCOMPARE(f.call("GET", "/v1/projects", agent).status, 200);
  }

  void auditTrail() {
    Fixture f;
    const QString p = f.createProject(QStringLiteral("One"));
    const QString t = f.createTask(p, QStringLiteral("t"));
    const QByteArray agent = f.createToken({{QStringLiteral("name"), QStringLiteral("reader")}, {QStringLiteral("kind"), QStringLiteral("agent")},
                                            {QStringLiteral("projects"), QJsonArray{p}}});
    const QByteArray dev = f.createToken({{QStringLiteral("name"), QStringLiteral("laptop")}, {QStringLiteral("kind"), QStringLiteral("device")}});
    const int before = static_cast<int>(f.auditRows().size());
    // Agent reads are audited (with the Tailscale identity); device reads are not.
    QCOMPARE(f.call("GET", "/v1/tasks/" + t.toLatin1(), agent, {}, {{"tailscale-user-login", "owner@example.com"}}).status, 200);
    QCOMPARE(f.call("GET", "/v1/tasks/" + t.toLatin1(), dev).status, 200);
    auto rows = f.auditRows();
    QCOMPARE(rows.size(), before + 1);
    QCOMPARE(rows.last().tsUser, QStringLiteral("owner@example.com"));
    QCOMPARE(rows.last().method, QStringLiteral("GET"));
    // A write records before/after.
    QCOMPARE(f.call("PATCH", "/v1/tasks/" + t.toLatin1(), dev, {{QStringLiteral("title"), QStringLiteral("renamed")}}).status, 200);
    rows = f.auditRows();
    QCOMPARE(rows.last().before, QStringLiteral("{\"title\":\"t\"}"));
    QVERIFY(rows.last().after.contains(QStringLiteral("\"title\":\"renamed\"")));
    // Token creation never stores the secret.
    for (const auto& e : rows) QVERIFY2(!e.after.contains(QStringLiteral("zpt_")), qPrintable(e.after));

    // Readable with audit:read, chained, and append-only.
    Response r = f.call("GET", "/v1/audit?since=0&limit=500", f.owner);
    QCOMPARE(r.status, 200);
    QVERIFY(Fixture::json(r).value(QStringLiteral("items")).toArray().size() >= 6);
    store::AuditLog log(f.store->database());
    QVERIFY(log.verify().ok());
    QVERIFY(!f.store->database().exec(QStringLiteral("DELETE FROM audit")).ok());
    QVERIFY(!f.store->database().exec(QStringLiteral("UPDATE audit SET status = 200")).ok());
    QVERIFY(log.verify().ok());
  }

  void failedWriteRollsBackButIsAudited() {
    Fixture f;
    const QString p = f.createProject(QStringLiteral("One"));
    const int changesBefore = static_cast<int>(f.store->changesSince(0)->size());
    Response r = f.call("POST", "/v1/projects/" + p.toLatin1() + "/tasks", f.owner,
                        {{QStringLiteral("title"), QStringLiteral("x")}, {QStringLiteral("parent"), QStringLiteral("ffffffffffffffffffff")}});
    QVERIFY(r.status >= 400);
    QCOMPARE(static_cast<int>(f.store->changesSince(0)->size()), changesBefore);
    QCOMPARE(f.auditRows().last().status, r.status);
  }

  void mcpTools() {
    Fixture f;
    const QString p = f.createProject(QStringLiteral("Garage"));
    f.createTask(p, QStringLiteral("Pour slab"));
    const QByteArray reader = f.createToken({{QStringLiteral("name"), QStringLiteral("reader")}, {QStringLiteral("kind"), QStringLiteral("agent")},
                                             {QStringLiteral("projects"), QJsonArray{p}}});
    const QByteArray writer = f.createToken({{QStringLiteral("name"), QStringLiteral("writer")}, {QStringLiteral("kind"), QStringLiteral("agent")},
                                             {QStringLiteral("projects"), QJsonArray{p}}, {QStringLiteral("scopes"), QJsonArray{QStringLiteral("write")}}});
    int id = 0;
    auto rpc = [&](const QByteArray& token, const QString& method, const QJsonObject& params = {}) {
      QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), ++id}, {QStringLiteral("method"), method}};
      if (!params.isEmpty()) msg.insert(QStringLiteral("params"), params);
      const Response r = f.call("POST", "/mcp", token, msg);
      if (r.status != 200) qFatal("mcp %s: %d %s", qPrintable(method), r.status, r.body.constData());
      return Fixture::json(r);
    };
    auto tool = [&](const QByteArray& token, const QString& name, const QJsonObject& args) {
      const QJsonObject reply =
          rpc(token, QStringLiteral("tools/call"), {{QStringLiteral("name"), name}, {QStringLiteral("arguments"), args}});
      if (!reply.contains(QStringLiteral("result"))) qFatal("tool %s: %s", qPrintable(name), QJsonDocument(reply).toJson().constData());
      return reply.value(QStringLiteral("result")).toObject();
    };

    QCOMPARE(f.call("POST", "/mcp", {}, {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}}).status, 401);
    QJsonObject init = rpc(reader, QStringLiteral("initialize"), {{QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")}});
    QCOMPARE(init.value(QStringLiteral("result")).toObject().value(QStringLiteral("protocolVersion")).toString(), QStringLiteral("2025-06-18"));
    // Notifications get 202 and no body.
    QCOMPARE(f.call("POST", "/mcp", reader, {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), QStringLiteral("notifications/initialized")}}).status, 202);

    auto names = [&](const QByteArray& token) {
      QStringList out;
      for (const auto& t : rpc(token, QStringLiteral("tools/list")).value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray()) {
        out << t.toObject().value(QStringLiteral("name")).toString();
      }
      return out;
    };
    QCOMPARE(names(reader), (QStringList{QStringLiteral("list_projects"), QStringLiteral("get_project"), QStringLiteral("search_tasks"), QStringLiteral("get_task")}));
    QVERIFY(names(writer).contains(QStringLiteral("delete_task")));
    // A read-only agent cannot call a write tool it was not shown.
    // Unknown arguments are a protocol error.
    QVERIFY(rpc(writer, QStringLiteral("tools/call"),
                {{QStringLiteral("name"), QStringLiteral("get_task")},
                 {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("id"), p}, {QStringLiteral("x"), 1}}}})
                .contains(QStringLiteral("error")));
    QJsonObject denied = rpc(reader, QStringLiteral("tools/call"),
                             {{QStringLiteral("name"), QStringLiteral("create_task")},
                              {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("project"), p}, {QStringLiteral("title"), QStringLiteral("x")}}}});
    QVERIFY(denied.contains(QStringLiteral("error")));

    QJsonObject res = tool(reader, QStringLiteral("search_tasks"), {{QStringLiteral("query"), QStringLiteral("slab")}});
    QVERIFY(!res.value(QStringLiteral("isError")).toBool());
    QCOMPARE(res.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("items")).toArray().size(), 1);

    // create -> dry-run delete -> confirm delete.
    res = tool(writer, QStringLiteral("create_task"), {{QStringLiteral("project"), p}, {QStringLiteral("title"), QStringLiteral("Temp")}});
    QVERIFY(!res.value(QStringLiteral("isError")).toBool());
    const QString tid = res.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("id")).toString();
    res = tool(writer, QStringLiteral("update_task"), {{QStringLiteral("id"), tid}, {QStringLiteral("rev"), 1}, {QStringLiteral("notes"), QStringLiteral("poured")}});
    QVERIFY2(!res.value(QStringLiteral("isError")).toBool(), qPrintable(QJsonDocument(res).toJson()));
    res = tool(writer, QStringLiteral("update_task"), {{QStringLiteral("id"), tid}, {QStringLiteral("rev"), 1}, {QStringLiteral("title"), QStringLiteral("stale")}});
    QVERIFY(res.value(QStringLiteral("isError")).toBool());  // 412
    res = tool(writer, QStringLiteral("delete_task"), {{QStringLiteral("id"), tid}});
    const QString confirm = res.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("confirmId")).toString();
    QVERIFY(!confirm.isEmpty());
    QCOMPARE(f.call("GET", "/v1/tasks/" + tid.toLatin1(), f.owner).status, 200);
    res = tool(writer, QStringLiteral("delete_task"), {{QStringLiteral("id"), tid}, {QStringLiteral("confirm_id"), confirm}});
    QVERIFY(!res.value(QStringLiteral("isError")).toBool());
    QCOMPARE(f.call("GET", "/v1/tasks/" + tid.toLatin1(), f.owner).status, 404);

    // Tool calls are audited with the tool name.
    QStringList tools;
    for (const auto& e : f.auditRows()) {
      if (!e.mcpTool.isEmpty()) tools << e.mcpTool;
    }
    QVERIFY(tools.contains(QStringLiteral("delete_task")));
    QVERIFY(tools.contains(QStringLiteral("search_tasks")));

    // Resources
    QJsonObject read = rpc(reader, QStringLiteral("resources/read"), {{QStringLiteral("uri"), QStringLiteral("zproject://projects/%1/summary").arg(p)}});
    const QString text = read.value(QStringLiteral("result")).toObject().value(QStringLiteral("contents")).toArray()[0].toObject().value(QStringLiteral("text")).toString();
    QCOMPARE(QJsonDocument::fromJson(text.toUtf8()).object().value(QStringLiteral("tasks")).toObject().value(QStringLiteral("total")).toInt(), 1);
    read = rpc(reader, QStringLiteral("resources/read"), {{QStringLiteral("uri"), QStringLiteral("zproject://openapi")}});
    QVERIFY(read.contains(QStringLiteral("result")));
    QVERIFY(rpc(reader, QStringLiteral("nope")).contains(QStringLiteral("error")));
  }
};

QTEST_GUILESS_MAIN(TestApi)
#include "tst_api.moc"
