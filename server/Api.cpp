#include "Api.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTextStream>
#include <QUrlQuery>

#include <algorithm>
#include <functional>
#include <optional>

#include "core/Id.hpp"
#include "core/Rollup.hpp"
#include "core/Schedule.hpp"
#include "core/Version.hpp"
#include "server/Auth.hpp"
#include "server/Mcp.hpp"
#include "server/ServerConfig.hpp"
#include "store/Backup.hpp"

namespace zproject::server {

using http::Response;

struct Api::Call {
  const http::Request* req = nullptr;  // the HTTP request (the /mcp one for tool calls)
  QByteArray method;
  QString path;
  QUrlQuery query;
  QJsonObject body;
  bool hasBody = false;
  QByteArray ifMatchHeader;
  QByteArray confirmHeader;
  const Route* route = nullptr;
  QStringList params;
  store::TokenRecord token;
  bool authed = false;
  QString requestId;
  bool dryRun = false;
  QString mcpTool;
  // Filled in by handlers for the audit row.
  QString entityKind;
  QString entityId;
  QJsonObject before;
  QJsonObject after;
  bool hasDiff = false;
  QString confirmId;
};

namespace {

const QString kProject = QStringLiteral("project");
const QString kTask = QStringLiteral("task");
const QString kDependency = QStringLiteral("dependency");
const QString kCalendar = QStringLiteral("calendar");
const QString kCalendarException = QStringLiteral("calendar_exception");
const QString kToken = QStringLiteral("token");

QString compact(const QJsonObject& o) {
  return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

Response withRequestId(Response r, const QString& requestId) {
  r.headers.append({"X-Request-Id", requestId.toLatin1()});
  return r;
}

Response validation(const QString& message, const QJsonObject& details = {}) {
  return Response::error(400, QStringLiteral("validation"), message, details);
}

Response forbiddenProject(const QString& projectId) {
  return Response::error(403, QStringLiteral("forbidden_project"),
                         QStringLiteral("this token is not allowed to access project %1").arg(projectId));
}

bool truthy(const QString& v) { return v == QLatin1String("true") || v == QLatin1String("1"); }

// Writable fields and their JSON types, taken from a default entity's JSON.
// Optional strings appear as null in the template and accept string or null.
std::optional<Response> checkFields(const QJsonObject& body, const QJsonObject& templ, const QSet<QString>& readOnly,
                                    const QSet<QString>& nullableNumbers) {
  for (auto it = body.constBegin(); it != body.constEnd(); ++it) {
    const QString& key = it.key();
    if (readOnly.contains(key)) {
      return validation(QStringLiteral("field '%1' cannot be set").arg(key),
                        QJsonObject{{QStringLiteral("field"), key}});
    }
    if (!templ.contains(key)) {
      return validation(QStringLiteral("unknown field '%1'").arg(key), QJsonObject{{QStringLiteral("field"), key}});
    }
    const QJsonValue t = templ.value(key);
    const QJsonValue v = it.value();
    bool ok = false;
    if (nullableNumbers.contains(key)) {
      ok = v.isDouble() || v.isNull();
    } else if (t.isString()) {
      ok = v.isString();
    } else if (t.isDouble()) {
      ok = v.isDouble();
    } else if (t.isBool()) {
      ok = v.isBool();
    } else if (t.isNull()) {
      ok = v.isString() || v.isNull();
    }
    if (!ok) {
      return validation(QStringLiteral("field '%1' has the wrong type").arg(key),
                        QJsonObject{{QStringLiteral("field"), key}});
    }
  }
  return std::nullopt;
}

const QSet<QString>& baseReadOnly() {
  static const QSet<QString> s{QStringLiteral("id"), QStringLiteral("createdAt"), QStringLiteral("updatedAt"),
                               QStringLiteral("deleted"), QStringLiteral("rev")};
  return s;
}

// A task's sibling order is the server's: it changes only through
// POST /v1/tasks/{id}/move (SPEC.md §7.1), never in a create or PATCH body.
QSet<QString> taskReadOnly() {
  QSet<QString> s = baseReadOnly();
  s.insert(QStringLiteral("order"));
  return s;
}

QJsonObject tokenJson(const store::TokenRecord& t, const QString& nowIso) {
  const bool expired = !t.expiresAt.isEmpty() && t.expiresAt <= nowIso;
  auto orNull = [](const QString& s) { return s.isEmpty() ? QJsonValue() : QJsonValue(s); };
  return QJsonObject{{QStringLiteral("id"), t.id},
                     {QStringLiteral("name"), t.name},
                     {QStringLiteral("kind"), t.kind},
                     {QStringLiteral("scopes"), QJsonArray::fromStringList(t.scopes)},
                     {QStringLiteral("projects"), QJsonArray::fromStringList(t.projects)},
                     {QStringLiteral("createdAt"), t.createdAt},
                     {QStringLiteral("createdBy"), orNull(t.createdBy)},
                     {QStringLiteral("expiresAt"), orNull(t.expiresAt)},
                     {QStringLiteral("lastUsedAt"), orNull(t.lastUsedAt)},
                     {QStringLiteral("revokedAt"), orNull(t.revokedAt)},
                     {QStringLiteral("requireConfirm"), t.requireConfirm},
                     {QStringLiteral("rateLimit"), t.rateLimit.isEmpty() ? QJsonValue() : QJsonValue(t.rateLimit)},
                     {QStringLiteral("active"), t.revokedAt.isEmpty() && !expired}};
}

QJsonObject changeJson(const store::ChangeRecord& c) {
  return QJsonObject{{QStringLiteral("seq"), c.seq},          {QStringLiteral("ts"), c.ts},
                     {QStringLiteral("actor"), c.actor},      {QStringLiteral("entityKind"), c.entityKind},
                     {QStringLiteral("entityId"), c.entityId}, {QStringLiteral("before"), c.before},
                     {QStringLiteral("after"), c.after},
                     {QStringLiteral("requestId"), c.requestId.isEmpty() ? QJsonValue() : QJsonValue(c.requestId)}};
}

QJsonObject auditJson(const store::AuditEntry& e) {
  auto orNull = [](const QString& s) { return s.isEmpty() ? QJsonValue() : QJsonValue(s); };
  auto json = [](const QString& s) {
    return s.isEmpty() ? QJsonValue() : QJsonValue(QJsonDocument::fromJson(s.toUtf8()).object());
  };
  return QJsonObject{{QStringLiteral("seq"), e.seq},
                     {QStringLiteral("ts"), e.ts},
                     {QStringLiteral("tokenId"), orNull(e.tokenId)},
                     {QStringLiteral("tokenName"), orNull(e.tokenName)},
                     {QStringLiteral("tokenKind"), orNull(e.tokenKind)},
                     {QStringLiteral("tsUser"), orNull(e.tsUser)},
                     {QStringLiteral("tsNode"), orNull(e.tsNode)},
                     {QStringLiteral("method"), orNull(e.method)},
                     {QStringLiteral("path"), orNull(e.path)},
                     {QStringLiteral("mcpTool"), orNull(e.mcpTool)},
                     {QStringLiteral("entityKind"), orNull(e.entityKind)},
                     {QStringLiteral("entityId"), orNull(e.entityId)},
                     {QStringLiteral("before"), json(e.before)},
                     {QStringLiteral("after"), json(e.after)},
                     {QStringLiteral("dryRun"), e.dryRun},
                     {QStringLiteral("confirmId"), orNull(e.confirmId)},
                     {QStringLiteral("status"), e.status},
                     {QStringLiteral("requestId"), orNull(e.requestId)},
                     {QStringLiteral("prevHash"), e.prevHash},
                     {QStringLiteral("hash"), e.hash}};
}

// ?limit= (1..500, default 100) and an opaque ?cursor= (an offset).
std::optional<Response> pageParams(const QUrlQuery& q, int* limit, qsizetype* offset) {
  *limit = 100;
  *offset = 0;
  if (q.hasQueryItem(QStringLiteral("limit"))) {
    bool ok = false;
    *limit = q.queryItemValue(QStringLiteral("limit")).toInt(&ok);
    if (!ok || *limit < 1 || *limit > 500) return validation(QStringLiteral("limit must be 1..500"));
  }
  if (q.hasQueryItem(QStringLiteral("cursor"))) {
    const QByteArray raw = QByteArray::fromBase64(q.queryItemValue(QStringLiteral("cursor")).toLatin1(),
                                                  QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    bool ok = false;
    const qlonglong off = raw.startsWith("o:") ? raw.mid(2).toLongLong(&ok) : -1;
    if (!ok || off < 0) return validation(QStringLiteral("invalid cursor"));
    *offset = static_cast<qsizetype>(off);
  }
  return std::nullopt;
}

QJsonObject page(const QJsonArray& all, int limit, qsizetype offset) {
  QJsonArray items;
  for (qsizetype i = offset; i < all.size() && items.size() < limit; ++i) items.append(all.at(i));
  const qsizetype next = offset + items.size();
  QJsonValue cursor;
  if (next < all.size()) {
    cursor = QString::fromLatin1(
        (QByteArray("o:") + QByteArray::number(next))
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
  }
  return QJsonObject{{QStringLiteral("items"), items}, {QStringLiteral("nextCursor"), cursor}};
}

std::optional<Response> sinceParams(const QUrlQuery& q, qint64* since, int* limit) {
  *since = 0;
  *limit = 100;
  if (q.hasQueryItem(QStringLiteral("since"))) {
    bool ok = false;
    *since = q.queryItemValue(QStringLiteral("since")).toLongLong(&ok);
    if (!ok || *since < 0) return validation(QStringLiteral("since must be a sequence number >= 0"));
  }
  if (q.hasQueryItem(QStringLiteral("limit"))) {
    bool ok = false;
    *limit = q.queryItemValue(QStringLiteral("limit")).toInt(&ok);
    if (!ok || *limit < 1 || *limit > 500) return validation(QStringLiteral("limit must be 1..500"));
  }
  return std::nullopt;
}

QJsonObject ripple() {
  return QJsonObject{{QStringLiteral("moved"), QJsonArray()},
                     {QStringLiteral("finishDelta"), 0},
                     {QStringLiteral("criticalChanged"), false},
                     {QStringLiteral("violations"), QJsonArray()}};
}

QJsonValue dateJson(const QDate& d) { return d.isValid() ? QJsonValue(d.toString(Qt::ISODate)) : QJsonValue(); }

QJsonObject violationJson(const ScheduleViolation& v, const QString& projectId) {
  return QJsonObject{{QStringLiteral("project"), projectId},
                     {QStringLiteral("kind"), v.kind},
                     {QStringLiteral("task"), v.task.isEmpty() ? QJsonValue() : QJsonValue(v.task)},
                     {QStringLiteral("other"), v.other.isEmpty() ? QJsonValue() : QJsonValue(v.other)},
                     {QStringLiteral("message"), v.message}};
}

QJsonObject scheduledJson(const ScheduledTask& t) {
  return QJsonObject{{QStringLiteral("id"), t.id},
                     {QStringLiteral("scheduled"), t.scheduled},
                     {QStringLiteral("summary"), t.summary},
                     {QStringLiteral("milestone"), t.milestone},
                     {QStringLiteral("fixed"), t.fixed},
                     {QStringLiteral("start"), dateJson(t.start)},
                     {QStringLiteral("finish"), dateJson(t.finish)},
                     {QStringLiteral("lateStart"), dateJson(t.lateStart)},
                     {QStringLiteral("lateFinish"), dateJson(t.lateFinish)},
                     {QStringLiteral("totalFloat"), t.totalFloat},
                     {QStringLiteral("freeFloat"), t.freeFloat},
                     {QStringLiteral("critical"), t.critical}};
}

// Status from a write's Result, for rippleOf().
template <typename T>
Status statusOf(const Result<T>& r) {
  if (r) return Ok{};
  return r.error();
}

QJsonObject change(const QString& op, const QString& kind, const QString& id, const QJsonObject& before,
                   const QJsonObject& after) {
  return QJsonObject{{QStringLiteral("op"), op},
                     {QStringLiteral("entityKind"), kind},
                     {QStringLiteral("entityId"), id.isEmpty() ? QJsonValue() : QJsonValue(id)},
                     {QStringLiteral("before"), before},
                     {QStringLiteral("after"), after}};
}

const QJsonObject kDeletedBefore{{QStringLiteral("deleted"), false}};
const QJsonObject kDeletedAfter{{QStringLiteral("deleted"), true}};

// For dry runs: the summary-task updates (SPEC.md §5.3) that turning the
// project's tasks `before` into `after` would cause. Rows in `skip` (the
// task the request itself writes) and deleted rows are left out.
void appendRollupChanges(QJsonArray& changes, const QList<Task>& before, QList<Task> after,
                         const QString& skip) {
  rollUpSummaries(after);
  QHash<QString, qsizetype> prev;
  for (qsizetype i = 0; i < before.size(); ++i) prev.insert(before[i].id, i);
  for (const Task& t : after) {
    if (t.deleted || t.id == skip || !prev.contains(t.id)) continue;
    const FieldDiff d = diffFields(toJson(before[prev.value(t.id)]), toJson(t));
    if (!d.empty()) changes.append(change(QStringLiteral("update"), kTask, t.id, d.before, d.after));
  }
}

// For dry runs that write several rows: every task whose row differs
// between `before` and `next` once the summaries are rolled up, one entry
// each (`opFor` names the op, e.g. "move" or "restore"; others "update").
// Rows deleted in both are left out.
QJsonArray treeChanges(const QList<Task>& before, QList<Task> next,
                       const std::function<QString(const QString&)>& opFor) {
  rollUpSummaries(next);
  QHash<QString, qsizetype> prev;
  for (qsizetype i = 0; i < before.size(); ++i) prev.insert(before[i].id, i);
  QJsonArray out;
  for (const Task& t : next) {
    if (!prev.contains(t.id)) continue;
    const Task& b = before[prev.value(t.id)];
    if (t.deleted && b.deleted) continue;
    const FieldDiff d = diffFields(toJson(b), toJson(t));
    if (!d.empty()) out.append(change(opFor(t.id), kTask, t.id, d.before, d.after));
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- routing

const QList<Api::Route>& Api::routeTable() {
  static const QList<Route> table{
      {{"GET", QStringLiteral("/v1/health"), QStringLiteral("getHealth"), QStringLiteral("none")}, &Api::getHealth, false},
      {{"GET", QStringLiteral("/v1/openapi.json"), QStringLiteral("getOpenApi"), QString()}, &Api::getOpenApi, false},
      {{"GET", QStringLiteral("/v1/projects"), QStringLiteral("listProjects"), QStringLiteral("projects:read")},
       &Api::listProjects, false},
      {{"POST", QStringLiteral("/v1/projects"), QStringLiteral("createProject"), QStringLiteral("projects:write"),
        false, true},
       &Api::createProject, true},
      {{"GET", QStringLiteral("/v1/projects/{id}"), QStringLiteral("getProject"), QStringLiteral("projects:read")},
       &Api::getProject, false},
      {{"PATCH", QStringLiteral("/v1/projects/{id}"), QStringLiteral("updateProject"),
        QStringLiteral("projects:write"), false, true},
       &Api::updateProject, true},
      {{"DELETE", QStringLiteral("/v1/projects/{id}"), QStringLiteral("deleteProject"),
        QStringLiteral("projects:write"), true, true},
       &Api::deleteProject, true},
      {{"GET", QStringLiteral("/v1/projects/{id}/wbs"), QStringLiteral("getWbs"), QStringLiteral("tasks:read")},
       &Api::getWbs, false},
      {{"GET", QStringLiteral("/v1/projects/{id}/tasks"), QStringLiteral("listTasks"), QStringLiteral("tasks:read")},
       &Api::listTasks, false},
      {{"POST", QStringLiteral("/v1/projects/{id}/tasks"), QStringLiteral("createTask"), QStringLiteral("tasks:write"),
        false, true},
       &Api::createTask, true},
      {{"GET", QStringLiteral("/v1/tasks/{id}"), QStringLiteral("getTask"), QStringLiteral("tasks:read")},
       &Api::getTask, false},
      {{"PATCH", QStringLiteral("/v1/tasks/{id}"), QStringLiteral("updateTask"), QStringLiteral("tasks:write"), false,
        true},
       &Api::updateTask, true},
      {{"DELETE", QStringLiteral("/v1/tasks/{id}"), QStringLiteral("deleteTask"), QStringLiteral("tasks:write"), true,
        true},
       &Api::deleteTask, true},
      {{"POST", QStringLiteral("/v1/tasks/{id}/move"), QStringLiteral("moveTask"), QStringLiteral("tasks:write"),
        false, true},
       &Api::moveTask, true},
      {{"POST", QStringLiteral("/v1/tasks/{id}/restore"), QStringLiteral("restoreTask"),
        QStringLiteral("tasks:write"), true, true},
       &Api::restoreTask, true},
      {{"GET", QStringLiteral("/v1/tasks/{id}/history"), QStringLiteral("getTaskHistory"),
        QStringLiteral("tasks:read")},
       &Api::getTaskHistory, false},
      {{"GET", QStringLiteral("/v1/calendars"), QStringLiteral("listCalendars"), QStringLiteral("resources:read")},
       &Api::listCalendars, false},
      {{"POST", QStringLiteral("/v1/calendars"), QStringLiteral("createCalendar"), QStringLiteral("resources:write"),
        false, true},
       &Api::createCalendar, true},
      {{"GET", QStringLiteral("/v1/calendars/{id}"), QStringLiteral("getCalendar"), QStringLiteral("resources:read")},
       &Api::getCalendar, false},
      {{"PATCH", QStringLiteral("/v1/calendars/{id}"), QStringLiteral("updateCalendar"),
        QStringLiteral("resources:write"), false, true},
       &Api::updateCalendar, true},
      {{"DELETE", QStringLiteral("/v1/calendars/{id}"), QStringLiteral("deleteCalendar"),
        QStringLiteral("resources:write"), true, true},
       &Api::deleteCalendar, true},
      {{"GET", QStringLiteral("/v1/calendars/{id}/exceptions"), QStringLiteral("listCalendarExceptions"),
        QStringLiteral("resources:read")},
       &Api::listCalendarExceptions, false},
      {{"POST", QStringLiteral("/v1/calendars/{id}/exceptions"), QStringLiteral("addCalendarException"),
        QStringLiteral("resources:write"), false, true},
       &Api::addCalendarException, true},
      {{"DELETE", QStringLiteral("/v1/calendar-exceptions/{id}"), QStringLiteral("deleteCalendarException"),
        QStringLiteral("resources:write"), true, true},
       &Api::deleteCalendarException, true},
      {{"POST", QStringLiteral("/v1/calendars/{id}/import-holidays"), QStringLiteral("importHolidays"),
        QStringLiteral("resources:write"), false, true},
       &Api::importHolidays, true},
      {{"GET", QStringLiteral("/v1/projects/{id}/schedule"), QStringLiteral("getSchedule"),
        QStringLiteral("schedule:read")},
       &Api::getSchedule, false},
      {{"GET", QStringLiteral("/v1/projects/{id}/critical-path"), QStringLiteral("getCriticalPath"),
        QStringLiteral("schedule:read")},
       &Api::getCriticalPath, false},
      {{"POST", QStringLiteral("/v1/projects/{id}/reschedule"), QStringLiteral("rescheduleProject"),
        QStringLiteral("tasks:write"), false, true},
       &Api::rescheduleProject, true},
      {{"GET", QStringLiteral("/v1/projects/{id}/dependencies"), QStringLiteral("listDependencies"),
        QStringLiteral("tasks:read")},
       &Api::listDependencies, false},
      {{"POST", QStringLiteral("/v1/dependencies"), QStringLiteral("createDependency"), QStringLiteral("tasks:write"),
        false, true},
       &Api::createDependency, true},
      {{"GET", QStringLiteral("/v1/dependencies/{id}"), QStringLiteral("getDependency"), QStringLiteral("tasks:read")},
       &Api::getDependency, false},
      {{"PATCH", QStringLiteral("/v1/dependencies/{id}"), QStringLiteral("updateDependency"),
        QStringLiteral("tasks:write"), false, true},
       &Api::updateDependency, true},
      {{"DELETE", QStringLiteral("/v1/dependencies/{id}"), QStringLiteral("deleteDependency"),
        QStringLiteral("tasks:write"), true, true},
       &Api::deleteDependency, true},
      {{"GET", QStringLiteral("/v1/changes"), QStringLiteral("listChanges"), QStringLiteral("tasks:read")},
       &Api::listChanges, false},
      {{"GET", QStringLiteral("/v1/tokens"), QStringLiteral("listTokens"), QStringLiteral("admin")}, &Api::listTokens,
       false},
      {{"POST", QStringLiteral("/v1/tokens"), QStringLiteral("createToken"), QStringLiteral("admin")},
       &Api::createToken, true},
      {{"DELETE", QStringLiteral("/v1/tokens/{id}"), QStringLiteral("revokeToken"), QStringLiteral("admin"), true,
        true},
       &Api::revokeToken, true},
      {{"GET", QStringLiteral("/v1/audit"), QStringLiteral("listAudit"), QStringLiteral("audit:read")},
       &Api::listAudit, false},
      // VACUUM INTO cannot run inside a transaction.
      {{"POST", QStringLiteral("/v1/admin/backup"), QStringLiteral("runBackup"), QStringLiteral("admin")},
       &Api::runBackup, false},
      // MCP: each tool call runs as its own /v1 operation through callAs().
      {{"POST", QStringLiteral("/mcp"), QStringLiteral("mcp"), QString()}, &Api::mcp, false},
  };
  return table;
}

const QList<RouteInfo>& Api::routes() {
  static const QList<RouteInfo> infos = [] {
    QList<RouteInfo> out;
    for (const Route& r : routeTable()) out.append(r.info);
    return out;
  }();
  return infos;
}

const Api::Route* Api::match(const QByteArray& method, const QString& path, QStringList* params,
                             bool* pathKnown) const {
  *pathKnown = false;
  const QStringList segs = path.split(QLatin1Char('/'));
  for (const Route& r : routeTable()) {
    const QStringList pat = r.info.pattern.split(QLatin1Char('/'));
    if (pat.size() != segs.size()) continue;
    QStringList found;
    bool ok = true;
    for (qsizetype i = 0; i < pat.size() && ok; ++i) {
      if (pat[i].startsWith(QLatin1Char('{'))) {
        if (segs[i].isEmpty()) ok = false;
        found.append(segs[i]);
      } else if (pat[i] != segs[i]) {
        ok = false;
      }
    }
    if (!ok) continue;
    *pathKnown = true;
    if (r.info.method == method) {
      *params = found;
      return &r;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------- lifecycle

Api::Api(store::SqliteStore& store, ApiOptions options)
    : store_(store),
      options_(std::move(options)),
      tokens_(store.database()),
      audit_(store.database()),
      limiter_(options_.rateClock),
      mcp_(std::make_unique<Mcp>(*this)) {}

Api::~Api() = default;

QDateTime Api::now() const { return options_.now ? options_.now() : QDateTime::currentDateTimeUtc(); }

QString Api::isoTimestamp(const QDateTime& t) { return t.toUTC().toString(Qt::ISODateWithMs); }

Result<QString> Api::createOwnerToken(const QString& name) {
  const NewToken nt = generateToken();
  store::TokenRecord t;
  t.id = nt.id;
  t.createdAt = isoTimestamp(now());
  t.name = name.trimmed().isEmpty() ? QStringLiteral("owner") : name.trimmed();
  t.kind = QStringLiteral("owner");
  t.secretHash = nt.secretHash;
  t.scopes = {QStringLiteral("admin")};
  t.projects = {QStringLiteral("*")};
  store::Transaction tx(store_.database());
  if (!tx.ok()) return tx.error();
  if (auto s = tokens_.insert(t); !s) return s.error();
  store::AuditEntry e;
  e.ts = t.createdAt;
  e.method = QStringLiteral("CLI");
  e.path = QStringLiteral("zprojectd --create-owner-token");
  e.entityKind = kToken;
  e.entityId = t.id;
  e.after = compact(tokenJson(t, t.createdAt));
  e.status = 201;
  e.requestId = newId();
  if (auto s = audit_.append(e); !s) return s.error();
  if (auto s = tx.commit(); !s) return s.error();
  return nt.text();
}

// ---------------------------------------------------------------- entry points

Response Api::handle(const http::Request& req) {
  Call c;
  c.req = &req;
  c.method = req.method;
  c.path = req.path;
  c.query = req.query;
  c.ifMatchHeader = req.header("if-match");
  c.confirmHeader = req.header("x-zproject-confirm");
  c.requestId = newId();

  // DNS-rebinding and CSRF guards (SPEC.md §14): known Host only, and no
  // browser origins at all (no CORS).
  if (!hostAllowed(options_.allowedHosts, req.header("host"))) {
    return withRequestId(Response::error(400, QStringLiteral("bad_host"), QStringLiteral("unexpected Host header")),
                         c.requestId);
  }
  if (req.hasHeader("origin")) {
    return withRequestId(Response::error(403, QStringLiteral("forbidden_origin"),
                                         QStringLiteral("browser requests are not accepted")),
                         c.requestId);
  }

  bool pathKnown = false;
  c.route = match(req.method, req.path, &c.params, &pathKnown);
  if (!c.route) {
    if (pathKnown) {
      QByteArrayList allowed;
      bool dummy = false;
      QStringList p;
      for (const char* m : {"GET", "POST", "PATCH", "PUT", "DELETE"}) {
        if (match(m, req.path, &p, &dummy)) allowed.append(m);
      }
      Response r = Response::error(405, QStringLiteral("method_not_allowed"), QStringLiteral("method not allowed"));
      r.headers.append({"Allow", allowed.join(", ")});
      return withRequestId(r, c.requestId);
    }
    return withRequestId(Response::error(404, QStringLiteral("not_found"), QStringLiteral("no such endpoint")),
                         c.requestId);
  }
  if (c.route->info.scope == QLatin1String("none")) return withRequestId((this->*c.route->handler)(c), c.requestId);

  if (auto fail = authenticate(req, &c.token, c)) return withRequestId(*fail, c.requestId);
  c.authed = true;

  if (req.method == "POST" || req.method == "PATCH" || req.method == "PUT") {
    if (!req.body.isEmpty() && c.route->info.operationId != QLatin1String("mcp")) {
      const QByteArray ct = req.header("content-type").toLower();
      if (!ct.startsWith("application/json")) {
        Response r = Response::error(415, QStringLiteral("unsupported_media_type"),
                                     QStringLiteral("send Content-Type: application/json"));
        audit(c, r.status);
        return withRequestId(r, c.requestId);
      }
      QJsonParseError perr{};
      const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
      if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        Response r = validation(QStringLiteral("the body must be a JSON object"));
        audit(c, r.status);
        return withRequestId(r, c.requestId);
      }
      c.body = doc.object();
      c.hasBody = true;
    }
  }
  return withRequestId(dispatch(c), c.requestId);
}

Response Api::callAs(const store::TokenRecord& token, const http::Request& outer, const InternalCall& call) {
  Call c;
  c.req = &outer;
  c.method = call.method;
  c.path = call.path;
  for (const auto& [k, v] : call.query) c.query.addQueryItem(k, v);
  c.body = call.body;
  c.hasBody = call.hasBody;
  c.ifMatchHeader = call.ifMatch;
  c.confirmHeader = call.confirm;
  c.mcpTool = call.mcpTool;
  c.token = token;
  c.authed = true;
  c.requestId = newId();
  bool pathKnown = false;
  c.route = match(call.method, call.path, &c.params, &pathKnown);
  if (!c.route || c.route->info.operationId == QLatin1String("mcp")) {
    return Response::error(404, QStringLiteral("not_found"), QStringLiteral("no such operation"));
  }
  return dispatch(c);
}

std::optional<Response> Api::authenticate(const http::Request& req, store::TokenRecord* token, Call& c) {
  const QDateTime t = now();
  const QString nowIso = isoTimestamp(t);
  QString reason;
  const auto parsed = parseBearer(req.header("authorization"));
  if (!parsed) {
    reason = QStringLiteral("missing or malformed bearer token");
  } else {
    c.token.id = parsed->id;
    auto rec = tokens_.find(parsed->id);
    if (!rec || !constantTimeEquals(hashSecret(parsed->secret).toLatin1(), rec->secretHash.toLatin1())) {
      reason = QStringLiteral("unknown token");  // same answer for an unknown id and a wrong secret
    } else if (!rec->revokedAt.isEmpty()) {
      reason = QStringLiteral("token revoked");
      c.token = rec.value();
    } else if (!rec->expiresAt.isEmpty() && rec->expiresAt <= nowIso) {
      reason = QStringLiteral("token expired");
      c.token = rec.value();
    } else {
      *token = rec.value();
      return std::nullopt;
    }
  }
  QString source = QString::fromLatin1(req.header("x-forwarded-for").split(',').value(0).trimmed());
  if (source.isEmpty()) source = req.peer.toString();
  Response r;
  if (const int wait = limiter_.takeFailedAuth(source); wait > 0) {
    r = Response::error(429, QStringLiteral("rate_limited"), QStringLiteral("too many failed authentications"));
    r.headers.append({"Retry-After", QByteArray::number(wait)});
  } else {
    r = Response::error(401, QStringLiteral("unauthenticated"), reason);
    r.headers.append({QByteArray("WWW-Authenticate"), QByteArray("Bearer")});
  }
  audit(c, r.status);
  return r;
}

Response Api::dispatch(Call& c) {
  const RouteInfo& info = c.route->info;
  if (!hasScope(c.token, info.scope)) {
    Response r = Response::error(403, QStringLiteral("forbidden_scope"),
                                 QStringLiteral("this token lacks the '%1' scope").arg(info.scope),
                                 QJsonObject{{QStringLiteral("required"), info.scope}});
    audit(c, r.status);
    return r;
  }
  c.dryRun = truthy(c.query.queryItemValue(QStringLiteral("dryRun")));
  if (c.query.hasQueryItem(QStringLiteral("dryRun")) && !info.dryRun) {
    Response r = validation(QStringLiteral("this operation has no dry run"));
    audit(c, r.status);
    return r;
  }
  if (info.operationId != QLatin1String("mcp")) {
    RateLimiter::Kind kind = RateLimiter::Kind::Read;
    if (c.method != "GET" && !c.dryRun) kind = info.destructive ? RateLimiter::Kind::Destructive : RateLimiter::Kind::Write;
    if (const int wait = limiter_.take(c.token.id, c.token.kind, c.token.rateLimit, kind); wait > 0) {
      Response r = Response::error(429, QStringLiteral("rate_limited"), QStringLiteral("rate limit exceeded"),
                                   QJsonObject{{QStringLiteral("retryAfter"), wait}});
      r.headers.append({"Retry-After", QByteArray::number(wait)});
      audit(c, r.status);
      return r;
    }
  }
  maybeTouch(c.token);

  std::optional<store::Transaction> tx;
  if (c.method != "GET" && c.route->transaction) {
    tx.emplace(store_.database());
    if (!tx->ok()) return storeError(tx->error());
  }
  Response r = (this->*c.route->handler)(c);
  if (tx) {
    if (r.status < 400) {
      audit(c, r.status);
      if (auto s = tx->commit(); !s) {
        tx.reset();
        r = storeError(s.error());
        audit(c, r.status);
      }
    } else {
      tx.reset();  // roll back, then record the refusal on its own
      audit(c, r.status);
    }
  } else {
    audit(c, r.status);
  }
  return r;
}

bool Api::shouldAudit(const Call& c, int status) const {
  if (!c.route || c.route->info.scope == QLatin1String("none")) return false;
  if (c.method != "GET") return c.route->info.operationId != QLatin1String("mcp");
  if (!c.mcpTool.isEmpty()) return true;
  if (c.token.kind == QLatin1String("agent") || c.token.kind == QLatin1String("app")) return true;
  if (status == 401 || status == 403 || status == 429) return true;
  return c.route->info.scope == QLatin1String("admin") || c.route->info.scope == QLatin1String("audit:read");
}

void Api::audit(const Call& c, int status) {
  if (!shouldAudit(c, status)) return;
  store::AuditEntry e;
  e.ts = isoTimestamp(now());
  e.tokenId = c.token.id;
  e.tokenName = c.token.name;
  e.tokenKind = c.token.kind;
  if (c.req) {
    e.tsUser = QString::fromUtf8(c.req->header("tailscale-user-login")).left(256);
    e.tsNode = QString::fromLatin1(c.req->header("x-forwarded-for")).left(256);
  }
  e.method = QString::fromLatin1(c.method);
  QString path = c.path;
  if (!c.query.isEmpty()) path += QLatin1Char('?') + c.query.toString(QUrl::FullyEncoded);
  e.path = path.left(2048);
  e.mcpTool = c.mcpTool;
  e.entityKind = c.entityKind;
  e.entityId = c.entityId;
  if (c.hasDiff) {
    e.before = compact(c.before);
    e.after = compact(c.after);
  }
  e.dryRun = c.dryRun;
  e.confirmId = c.confirmId;
  e.status = status;
  e.requestId = c.requestId;
  if (auto s = audit_.append(e); !s) {
    QTextStream(stderr) << "zprojectd: audit append failed: " << s.error().message << '\n';
  }
}

void Api::maybeTouch(const store::TokenRecord& t) {
  const qint64 ms = now().toMSecsSinceEpoch();
  if (ms - lastTouchMs_.value(t.id, 0) < 60'000) return;
  lastTouchMs_.insert(t.id, ms);
  (void)tokens_.touch(t.id, isoTimestamp(now()));
}

// ---------------------------------------------------------------- helpers

Response Api::storeError(const Error& e) {
  static const QHash<QString, int> statusFor{{QStringLiteral("validation"), 400},
                                             {QStringLiteral("not_found"), 404},
                                             {QStringLiteral("conflict"), 409},
                                             {QStringLiteral("cycle"), 409},
                                             {QStringLiteral("rev_mismatch"), 412}};
  const int status = statusFor.value(e.code, 500);
  if (status == 500) {
    QTextStream(stderr) << "zprojectd: internal error: " << e.message << '\n';
    return Response::error(500, QStringLiteral("internal"), QStringLiteral("internal error"));
  }
  return Response::error(status, e.code, e.message, e.details);
}

Response Api::checkProjectAccess(Call& c, const QString& projectId) {
  if (!projectAllowed(c.token, projectId)) return forbiddenProject(projectId);
  return Response::empty(0);
}

std::optional<qint64> Api::ifMatch(const Call& c, Response* error) const {
  QByteArray v = c.ifMatchHeader.trimmed();
  if (v.isEmpty()) return std::nullopt;
  if (v.startsWith("W/")) v = v.mid(2);
  if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"')) v = v.mid(1, v.size() - 2);
  bool ok = false;
  const qint64 rev = v.toLongLong(&ok);
  if (!ok || rev < 1) {
    *error = validation(QStringLiteral("If-Match must be a revision number"));
    return std::nullopt;
  }
  return rev;
}

QString Api::requestHash(const Call& c) const {
  QList<QPair<QString, QString>> items = c.query.queryItems(QUrl::FullyDecoded);
  items.erase(std::remove_if(items.begin(), items.end(),
                             [](const auto& kv) { return kv.first == QLatin1String("dryRun"); }),
              items.end());
  std::sort(items.begin(), items.end());
  QByteArray data = c.method + '\n' + c.path.toUtf8() + '\n';
  for (const auto& [k, v] : items) data += k.toUtf8() + '=' + v.toUtf8() + '&';
  data += '\n';
  if (c.hasBody) data += QJsonDocument(c.body).toJson(QJsonDocument::Compact);
  return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

Response Api::dryRunResponse(Call& c, const QJsonArray& changes, const QJsonObject& rip) {
  QJsonObject out{{QStringLiteral("dryRun"), true},
                  {QStringLiteral("changes"), changes},
                  {QStringLiteral("ripple"), rip.isEmpty() ? ripple() : rip}};
  if (c.route->info.destructive) {
    const QDateTime t = now();
    const QString id = newId();
    const QString expires = isoTimestamp(t.addSecs(options_.confirmTtlSeconds));
    if (auto s = tokens_.addConfirm(id, c.token.id, requestHash(c), isoTimestamp(t), expires); !s) {
      return storeError(s.error());
    }
    c.confirmId = id;
    out.insert(QStringLiteral("confirmId"), id);
    out.insert(QStringLiteral("confirmExpiresAt"), expires);
  }
  return Response::json(200, out);
}

std::optional<Response> Api::requireConfirm(Call& c) {
  const QString given = QString::fromLatin1(c.confirmHeader.trimmed());
  if (given.isEmpty()) {
    if (!c.token.requireConfirm) return std::nullopt;
    return Response::error(409, QStringLiteral("confirm_required"),
                           QStringLiteral("this token needs a confirmation: repeat the request with ?dryRun=true "
                                          "and send its confirmId in X-Zproject-Confirm"));
  }
  auto ok = tokens_.consumeConfirm(given, c.token.id, requestHash(c), isoTimestamp(now()));
  if (!ok) return storeError(ok.error());
  if (!ok.value()) {
    return Response::error(409, QStringLiteral("confirm_required"),
                           QStringLiteral("the confirmation is unknown, expired, already used or for a different "
                                          "request"));
  }
  c.confirmId = given;
  return std::nullopt;
}

QStringList Api::allProjectIds() {
  QStringList out;
  if (auto ps = store_.projects(); ps) {
    for (const Project& p : ps.value()) out << p.id;
  }
  return out;
}

Result<QJsonObject> Api::rippleOf(const QStringList& projectIds, const std::function<Status()>& write) {
  store::Transaction sandbox(store_.database());  // never committed: everything below is undone
  if (!sandbox.ok()) return sandbox.error();
  QHash<QString, Schedule> before;
  for (const QString& pid : projectIds) {
    if (auto s = store_.projectSchedule(pid); s) before.insert(pid, s.value());
  }
  if (auto s = write(); !s) return s.error();
  QJsonArray moved;
  QJsonArray violations;
  qint64 finishDelta = 0;
  bool criticalChanged = false;
  for (const QString& pid : projectIds) {
    auto after = store_.projectSchedule(pid);
    if (!after || !before.contains(pid)) continue;
    const Ripple r = diffSchedules(before.value(pid), after.value());
    for (const RippleMove& m : r.moved) {
      moved.append(QJsonObject{{QStringLiteral("project"), pid},
                               {QStringLiteral("task"), m.task},
                               {QStringLiteral("startBefore"), dateJson(m.startBefore)},
                               {QStringLiteral("startAfter"), dateJson(m.startAfter)},
                               {QStringLiteral("finishBefore"), dateJson(m.finishBefore)},
                               {QStringLiteral("finishAfter"), dateJson(m.finishAfter)},
                               {QStringLiteral("startDelta"), m.startDelta},
                               {QStringLiteral("finishDelta"), m.finishDelta}});
    }
    if (std::abs(r.finishDelta) > std::abs(finishDelta)) finishDelta = r.finishDelta;
    criticalChanged = criticalChanged || r.criticalChanged;
    for (const ScheduleViolation& v : after->violations) violations.append(violationJson(v, pid));
  }
  return QJsonObject{{QStringLiteral("moved"), moved},
                     {QStringLiteral("finishDelta"), finishDelta},
                     {QStringLiteral("criticalChanged"), criticalChanged},
                     {QStringLiteral("violations"), violations}};
}

// ---------------------------------------------------------------- handlers

Response Api::getHealth(Call&) {
  return Response::json(200, QJsonObject{{QStringLiteral("ok"), true},
                                         {QStringLiteral("version"), appVersion()},
                                         {QStringLiteral("apiVersion"), kApiVersion},
                                         {QStringLiteral("schemaVersion"), store_.schemaVersion()}});
}

Response Api::getOpenApi(Call&) {
  Response r;
  r.body = openApiJson();
  return r;
}

Response Api::listProjects(Call& c) {
  int limit = 0;
  qsizetype offset = 0;
  if (auto e = pageParams(c.query, &limit, &offset)) return *e;
  auto all = store_.projects(truthy(c.query.queryItemValue(QStringLiteral("includeDeleted"))));
  if (!all) return storeError(all.error());
  QJsonArray items;
  for (const Project& p : all.value()) {
    if (projectAllowed(c.token, p.id)) items.append(toJson(p));
  }
  return Response::json(200, page(items, limit, offset));
}

Response Api::createProject(Call& c) {
  if (!allProjects(c.token)) {
    return Response::error(403, QStringLiteral("forbidden_project"),
                           QStringLiteral("a token limited to named projects cannot create projects"));
  }
  if (auto e = checkFields(c.body, toJson(Project{}), baseReadOnly(), {QStringLiteral("budget")})) return *e;
  Project p = projectFromJson(c.body);
  if (auto e = toValidationError(validateProject(p))) return storeError(*e);
  if (c.dryRun) {
    QJsonObject after = toJson(p);
    for (const QString& k : baseReadOnly()) after.remove(k);
    return dryRunResponse(c, QJsonArray{change(QStringLiteral("create"), kProject, {}, {}, after)});
  }
  auto r = store_.createProject(p, {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.entityKind = kProject;
  c.entityId = r->id;
  c.after = toJson(r.value());
  c.hasDiff = true;
  return Response::json(201, toJson(r.value()));
}

Response Api::getProject(Call& c) {
  const QString id = c.params.value(0);
  if (auto a = checkProjectAccess(c, id); a.status) return a;
  auto p = store_.project(id);
  if (!p) return storeError(p.error());
  return Response::json(200, toJson(p.value()));
}

Response Api::updateProject(Call& c) {
  const QString id = c.params.value(0);
  if (auto a = checkProjectAccess(c, id); a.status) return a;
  if (auto e = checkFields(c.body, toJson(Project{}), baseReadOnly(), {QStringLiteral("budget")})) return *e;
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  auto cur = store_.project(id);
  if (!cur) return storeError(cur.error());
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  QJsonObject merged = toJson(cur.value());
  for (auto it = c.body.constBegin(); it != c.body.constEnd(); ++it) merged.insert(it.key(), it.value());
  Project p = projectFromJson(merged);
  p.id = id;
  if (c.dryRun) {
    if (auto e = toValidationError(validateProject(p))) return storeError(*e);
    const FieldDiff d = diffFields(toJson(cur.value()), toJson(p));
    QJsonArray changes;
    if (!d.empty()) changes.append(change(QStringLiteral("update"), kProject, id, d.before, d.after));
    auto rip = rippleOf({id}, [&] { return statusOf(store_.updateProject(p, cur->rev, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, changes, rip.value());
  }
  auto r = store_.updateProject(p, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  const FieldDiff d = diffFields(toJson(cur.value()), toJson(r.value()));
  c.entityKind = kProject;
  c.entityId = id;
  c.before = d.before;
  c.after = d.after;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::deleteProject(Call& c) {
  const QString id = c.params.value(0);
  if (auto a = checkProjectAccess(c, id); a.status) return a;
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  auto cur = store_.project(id);
  if (!cur) return storeError(cur.error());
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  c.entityKind = kProject;
  c.entityId = id;
  if (c.dryRun) {
    auto ts = store_.tasks(id);
    if (!ts) return storeError(ts.error());
    QJsonArray changes{change(QStringLiteral("delete"), kProject, id, kDeletedBefore, kDeletedAfter)};
    for (const Task& t : ts.value()) {
      changes.append(change(QStringLiteral("delete"), kTask, t.id, kDeletedBefore, kDeletedAfter));
    }
    return dryRunResponse(c, changes);
  }
  if (auto refuse = requireConfirm(c)) return *refuse;
  auto r = store_.deleteProject(id, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.before = kDeletedBefore;
  c.after = kDeletedAfter;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::getWbs(Call& c) {
  const QString id = c.params.value(0);
  if (auto a = checkProjectAccess(c, id); a.status) return a;
  if (auto p = store_.project(id); !p) return storeError(p.error());
  auto ts = store_.tasks(id);
  if (!ts) return storeError(ts.error());
  // Tasks come depth-first; number siblings 1, 2, ... under their parent.
  QHash<QString, QString> code;
  QHash<QString, int> childCount;
  QHash<QString, int> depth;
  QJsonArray items;
  for (const Task& t : ts.value()) {
    const QString parent = code.contains(t.parent) ? t.parent : QString();
    const int n = ++childCount[parent];
    code[t.id] = parent.isEmpty() ? QString::number(n) : code[parent] + QLatin1Char('.') + QString::number(n);
    depth[t.id] = parent.isEmpty() ? 0 : depth[parent] + 1;
    items.append(QJsonObject{{QStringLiteral("id"), t.id},
                             {QStringLiteral("wbs"), code[t.id]},
                             {QStringLiteral("depth"), depth[t.id]},
                             {QStringLiteral("title"), t.title},
                             {QStringLiteral("type"), t.type},
                             {QStringLiteral("parent"), parent.isEmpty() ? QJsonValue() : QJsonValue(parent)}});
  }
  return Response::json(200, QJsonObject{{QStringLiteral("projectId"), id}, {QStringLiteral("items"), items}});
}

Response Api::listTasks(Call& c) {
  const QString id = c.params.value(0);
  if (auto a = checkProjectAccess(c, id); a.status) return a;
  int limit = 0;
  qsizetype offset = 0;
  if (auto e = pageParams(c.query, &limit, &offset)) return *e;
  if (auto p = store_.project(id); !p) return storeError(p.error());
  auto ts = store_.tasks(id, truthy(c.query.queryItemValue(QStringLiteral("includeDeleted"))));
  if (!ts) return storeError(ts.error());
  const QString q = c.query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
  const QString status = c.query.queryItemValue(QStringLiteral("status"));
  QJsonArray items;
  for (const Task& t : ts.value()) {
    if (!q.isEmpty() && !t.title.contains(q, Qt::CaseInsensitive) && !t.notes.contains(q, Qt::CaseInsensitive)) {
      continue;
    }
    if (!status.isEmpty() && t.status != status) continue;
    items.append(toJson(t));
  }
  return Response::json(200, page(items, limit, offset));
}

Response Api::createTask(Call& c) {
  const QString projectId = c.params.value(0);
  if (auto a = checkProjectAccess(c, projectId); a.status) return a;
  QJsonObject body = c.body;
  if (body.contains(QStringLiteral("project"))) {
    if (body.value(QStringLiteral("project")).toString() != projectId) {
      return validation(QStringLiteral("project in the body differs from the path"));
    }
    body.remove(QStringLiteral("project"));
  }
  if (auto e = checkFields(body, toJson(Task{}), taskReadOnly(), {})) return *e;
  Task t = taskFromJson(body);
  t.project = projectId;
  if (auto d = store::SqliteStore::checkDerivedDates(Task{}, t, false); !d) return storeError(d.error());
  if (auto e = toValidationError(validateTask(t))) return storeError(*e);
  if (c.dryRun) {
    if (auto p = store_.project(projectId); !p) return storeError(p.error());
    if (!t.parent.isEmpty()) {
      auto parent = store_.task(t.parent);
      if (!parent || parent->project != projectId) {
        return validation(QStringLiteral("parent %1 is not a task of this project").arg(t.parent));
      }
    }
    QJsonObject after = toJson(t);
    for (const QString& k : baseReadOnly()) after.remove(k);
    QJsonArray changes{change(QStringLiteral("create"), kTask, {}, {}, after)};
    if (!t.parent.isEmpty()) {
      auto ts = store_.tasks(projectId);
      if (!ts) return storeError(ts.error());
      QList<Task> next = ts.value();
      Task created = t;
      created.id = QStringLiteral("(new)");
      next.append(created);
      appendRollupChanges(changes, ts.value(), next, created.id);
    }
    auto rip = rippleOf({projectId}, [&] { return statusOf(store_.createTask(t, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, changes, rip.value());
  }
  auto r = store_.createTask(t, {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.entityKind = kTask;
  c.entityId = r->id;
  c.after = toJson(r.value());
  c.hasDiff = true;
  return Response::json(201, toJson(r.value()));
}

Response Api::getTask(Call& c) {
  auto t = store_.task(c.params.value(0));
  if (!t) return storeError(t.error());
  if (auto a = checkProjectAccess(c, t->project); a.status) return a;
  return Response::json(200, toJson(t.value()));
}

Response Api::updateTask(Call& c) {
  const QString id = c.params.value(0);
  auto cur = store_.task(id);
  if (!cur) return storeError(cur.error());
  if (auto a = checkProjectAccess(c, cur->project); a.status) return a;
  QSet<QString> readOnly = taskReadOnly();
  readOnly.insert(QStringLiteral("project"));
  if (auto e = checkFields(c.body, toJson(Task{}), readOnly, {})) return *e;
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  QJsonObject merged = toJson(cur.value());
  for (auto it = c.body.constBegin(); it != c.body.constEnd(); ++it) merged.insert(it.key(), it.value());
  Task t = taskFromJson(merged);
  t.id = id;
  t.project = cur->project;
  if (c.dryRun) {
    auto ts = store_.tasks(cur->project);
    if (!ts) return storeError(ts.error());
    if (auto d = store::SqliteStore::checkDerivedDates(cur.value(), t, summaryIds(ts.value()).contains(id)); !d) {
      return storeError(d.error());
    }
    if (auto e = toValidationError(validateTask(t))) return storeError(*e);
    if (summaryIds(ts.value()).contains(id)) {
      if (auto e = toValidationError(checkSummaryEdit(cur.value(), t))) return storeError(*e);
    }
    if (t.parent != cur->parent) {
      QList<Task> moved = ts.value();
      for (Task& row : moved) {
        if (row.id == id) row.parent = t.parent;
      }
      if (auto s = store_.checkLinkCycles(moved); !s) return storeError(s.error());
    }
    const FieldDiff d = diffFields(toJson(cur.value()), toJson(t));
    QJsonArray changes;
    if (!d.empty()) {
      changes.append(change(QStringLiteral("update"), kTask, id, d.before, d.after));
      QList<Task> next = ts.value();
      for (Task& row : next) {
        if (row.id == id) row = t;
      }
      appendRollupChanges(changes, ts.value(), next, id);
    }
    auto rip = rippleOf({cur->project}, [&] { return statusOf(store_.updateTask(t, cur->rev, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, changes, rip.value());
  }
  auto r = store_.updateTask(t, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  const FieldDiff d = diffFields(toJson(cur.value()), toJson(r.value()));
  c.entityKind = kTask;
  c.entityId = id;
  c.before = d.before;
  c.after = d.after;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::deleteTask(Call& c) {
  const QString id = c.params.value(0);
  auto cur = store_.task(id);
  if (!cur) return storeError(cur.error());
  if (auto a = checkProjectAccess(c, cur->project); a.status) return a;
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  c.entityKind = kTask;
  c.entityId = id;
  if (c.dryRun) {
    auto ts = store_.tasks(cur->project);
    if (!ts) return storeError(ts.error());
    // The task and its subtree (tasks() is depth-first).
    QSet<QString> subtree{id};
    QJsonArray changes;
    QList<Task> next = ts.value();
    for (Task& t : next) {
      if (t.id == id || subtree.contains(t.parent)) {
        subtree.insert(t.id);
        t.deleted = true;
        changes.append(change(QStringLiteral("delete"), kTask, t.id, kDeletedBefore, kDeletedAfter));
      }
    }
    // Its links go with it.
    auto links = store_.linksTouching(cur->project, subtree);
    if (!links) return storeError(links.error());
    for (const Dependency& d : links.value()) {
      changes.append(change(QStringLiteral("delete"), kDependency, d.id, kDeletedBefore, kDeletedAfter));
    }
    appendRollupChanges(changes, ts.value(), next, id);
    auto rip = rippleOf({cur->project}, [&] { return statusOf(store_.deleteTask(id, cur->rev, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, changes, rip.value());
  }
  if (auto refuse = requireConfirm(c)) return *refuse;
  auto r = store_.deleteTask(id, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.before = kDeletedBefore;
  c.after = kDeletedAfter;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::moveTask(Call& c) {
  const QString id = c.params.value(0);
  auto cur = store_.task(id);
  if (!cur) return storeError(cur.error());
  if (auto a = checkProjectAccess(c, cur->project); a.status) return a;
  // Body: {"parent": id | null, "position": n}; both optional (keep the
  // parent; last place).
  QString parent = cur->parent;
  std::optional<int> position;
  for (auto it = c.body.constBegin(); it != c.body.constEnd(); ++it) {
    const QString& key = it.key();
    const QJsonValue v = it.value();
    if (key == QLatin1String("parent")) {
      if (v.isNull()) {
        parent.clear();
      } else if (v.isString() && isValidId(v.toString())) {
        parent = v.toString();
      } else {
        return validation(QStringLiteral("parent must be a task id or null"),
                          QJsonObject{{QStringLiteral("field"), key}});
      }
    } else if (key == QLatin1String("position")) {
      const double d = v.toDouble(-1);
      if (!v.isDouble() || d < 0 || d != static_cast<double>(static_cast<qint64>(d)) || d > 1e9) {
        return validation(QStringLiteral("position must be a whole number, 0 or more"),
                          QJsonObject{{QStringLiteral("field"), key}});
      }
      position = static_cast<int>(d);
    } else {
      return validation(QStringLiteral("unknown field '%1'").arg(key), QJsonObject{{QStringLiteral("field"), key}});
    }
  }
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  c.entityKind = kTask;
  c.entityId = id;
  if (c.dryRun) {
    auto plan = store_.planMove(id, parent, position);
    if (!plan) return storeError(plan.error());
    auto ts = store_.tasks(cur->project);
    if (!ts) return storeError(ts.error());
    QHash<QString, Task> writes;
    for (const Task& t : plan.value()) writes.insert(t.id, t);
    QList<Task> next = ts.value();
    for (Task& row : next) {
      if (writes.contains(row.id)) row = writes.value(row.id);
    }
    auto rip = rippleOf({cur->project}, [&] { return statusOf(store_.moveTask(id, parent, position, cur->rev, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c,
                          treeChanges(ts.value(), next,
                                      [&id](const QString& tid) {
                                        return tid == id ? QStringLiteral("move") : QStringLiteral("update");
                                      }),
                          rip.value());
  }
  auto r = store_.moveTask(id, parent, position, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  const FieldDiff d = diffFields(toJson(cur.value()), toJson(r.value()));
  c.before = d.before;
  c.after = d.after;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::restoreTask(Call& c) {
  const QString id = c.params.value(0);
  auto cur = store_.task(id, true);
  if (!cur) return storeError(cur.error());
  if (auto a = checkProjectAccess(c, cur->project); a.status) return a;
  if (!c.body.isEmpty()) {
    const QString key = c.body.constBegin().key();
    return validation(QStringLiteral("unknown field '%1'").arg(key), QJsonObject{{QStringLiteral("field"), key}});
  }
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  c.entityKind = kTask;
  c.entityId = id;
  if (c.dryRun) {
    auto plan = store_.planRestore(id);
    if (!plan) return storeError(plan.error());
    auto ts = store_.tasks(cur->project, true);
    if (!ts) return storeError(ts.error());
    QSet<QString> restored;
    for (const Task& t : plan.value()) restored.insert(t.id);
    QList<Task> next = ts.value();
    for (Task& row : next) {
      if (restored.contains(row.id)) row.deleted = false;
    }
    auto links = store_.planRestoreLinks(id);
    if (!links) return storeError(links.error());
    QJsonArray changes = treeChanges(ts.value(), next, [&restored](const QString& tid) {
      return restored.contains(tid) ? QStringLiteral("restore") : QStringLiteral("update");
    });
    for (const Dependency& d : links.value()) {
      changes.append(change(QStringLiteral("restore"), kDependency, d.id, kDeletedAfter, kDeletedBefore));
    }
    auto rip = rippleOf({cur->project}, [&] { return statusOf(store_.restoreTask(id, cur->rev, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, changes, rip.value());
  }
  if (auto refuse = requireConfirm(c)) return *refuse;
  auto r = store_.restoreTask(id, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.before = kDeletedAfter;
  c.after = kDeletedBefore;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::getTaskHistory(Call& c) {
  const QString id = c.params.value(0);
  auto t = store_.task(id, true);
  if (!t) return storeError(t.error());
  if (auto a = checkProjectAccess(c, t->project); a.status) return a;
  qint64 since = 0;
  int limit = 0;
  if (auto e = sinceParams(c.query, &since, &limit)) return *e;
  auto rows = store_.changesFor(kTask, id, since, limit);
  if (!rows) return storeError(rows.error());
  QJsonArray items;
  for (const auto& ch : rows.value()) items.append(changeJson(ch));
  return Response::json(200, QJsonObject{{QStringLiteral("items"), items}});
}

// ---------------------------------------------------------------- calendars

namespace {

// Calendars are shared by every project: changing one can move any of them,
// so only a token for all projects may write them.
std::optional<Response> requireAllProjects(const store::TokenRecord& t) {
  if (allProjects(t)) return std::nullopt;
  return Response::error(403, QStringLiteral("forbidden_project"),
                         QStringLiteral("calendars are shared by every project; this token is limited to some"));
}

// A calendar body: the usual field checks, except workWeek (an array of
// seven {working, hours}, or null), which the template can't describe.
std::optional<Response> checkCalendarFields(const QJsonObject& body) {
  QJsonObject rest = body;
  rest.remove(QStringLiteral("workWeek"));
  if (auto e = checkFields(rest, toJson(CalendarDef{}), baseReadOnly(), {})) return e;
  if (!body.contains(QStringLiteral("workWeek"))) return std::nullopt;
  const QJsonValue w = body.value(QStringLiteral("workWeek"));
  if (w.isNull()) return std::nullopt;
  bool ok = w.isArray() && w.toArray().size() == 7;
  for (const auto& d : w.toArray()) {
    const QJsonObject o = d.toObject();
    ok = ok && d.isObject() && o.size() == 2 && o.value(QStringLiteral("working")).isBool() &&
         o.value(QStringLiteral("hours")).isDouble();
  }
  if (ok) return std::nullopt;
  return validation(QStringLiteral("workWeek must be seven {\"working\": bool, \"hours\": number} days, Monday "
                                   "first, or null"),
                    QJsonObject{{QStringLiteral("field"), QStringLiteral("workWeek")}});
}

}  // namespace

Response Api::listCalendars(Call& c) {
  int limit = 0;
  qsizetype offset = 0;
  if (auto e = pageParams(c.query, &limit, &offset)) return *e;
  auto all = store_.calendars(truthy(c.query.queryItemValue(QStringLiteral("includeDeleted"))));
  if (!all) return storeError(all.error());
  QJsonArray items;
  for (const CalendarDef& cal : all.value()) items.append(toJson(cal));
  return Response::json(200, page(items, limit, offset));
}

Response Api::createCalendar(Call& c) {
  if (auto r = requireAllProjects(c.token)) return *r;
  if (auto e = checkCalendarFields(c.body)) return *e;
  const CalendarDef cal = calendarFromJson(c.body);
  if (c.dryRun) {
    if (auto s = store_.checkCalendar(cal); !s) return storeError(s.error());
    QJsonObject after = toJson(cal);
    for (const QString& k : baseReadOnly()) after.remove(k);
    return dryRunResponse(c, QJsonArray{change(QStringLiteral("create"), kCalendar, {}, {}, after)});
  }
  auto r = store_.createCalendar(cal, {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.entityKind = kCalendar;
  c.entityId = r->id;
  c.after = toJson(r.value());
  c.hasDiff = true;
  return Response::json(201, toJson(r.value()));
}

Response Api::getCalendar(Call& c) {
  auto cal = store_.calendar(c.params.value(0));
  if (!cal) return storeError(cal.error());
  return Response::json(200, toJson(cal.value()));
}

Response Api::updateCalendar(Call& c) {
  if (auto r = requireAllProjects(c.token)) return *r;
  const QString id = c.params.value(0);
  auto cur = store_.calendar(id);
  if (!cur) return storeError(cur.error());
  if (auto e = checkCalendarFields(c.body)) return *e;
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  QJsonObject merged = toJson(cur.value());
  for (auto it = c.body.constBegin(); it != c.body.constEnd(); ++it) merged.insert(it.key(), it.value());
  CalendarDef cal = calendarFromJson(merged);
  cal.id = id;
  if (c.dryRun) {
    if (auto s = store_.checkCalendar(cal); !s) return storeError(s.error());
    const FieldDiff d = diffFields(toJson(cur.value()), toJson(cal));
    QJsonArray changes;
    if (!d.empty()) changes.append(change(QStringLiteral("update"), kCalendar, id, d.before, d.after));
    auto rip = rippleOf(allProjectIds(), [&] { return statusOf(store_.updateCalendar(cal, cur->rev, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, changes, rip.value());
  }
  auto r = store_.updateCalendar(cal, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  const FieldDiff d = diffFields(toJson(cur.value()), toJson(r.value()));
  c.entityKind = kCalendar;
  c.entityId = id;
  c.before = d.before;
  c.after = d.after;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::deleteCalendar(Call& c) {
  if (auto r = requireAllProjects(c.token)) return *r;
  const QString id = c.params.value(0);
  auto cur = store_.calendar(id);
  if (!cur) return storeError(cur.error());
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  c.entityKind = kCalendar;
  c.entityId = id;
  if (c.dryRun) {
    if (auto s = store_.checkCalendarDelete(id); !s) return storeError(s.error());
    QJsonArray changes{change(QStringLiteral("delete"), kCalendar, id, kDeletedBefore, kDeletedAfter)};
    auto excs = store_.calendarExceptions(id);
    if (!excs) return storeError(excs.error());
    for (const CalendarException& e : excs.value()) {
      changes.append(change(QStringLiteral("delete"), kCalendarException, e.id, kDeletedBefore, kDeletedAfter));
    }
    return dryRunResponse(c, changes);
  }
  if (auto refuse = requireConfirm(c)) return *refuse;
  auto r = store_.deleteCalendar(id, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.before = kDeletedBefore;
  c.after = kDeletedAfter;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::listCalendarExceptions(Call& c) {
  const QString id = c.params.value(0);
  if (auto cal = store_.calendar(id); !cal) return storeError(cal.error());
  int limit = 0;
  qsizetype offset = 0;
  if (auto e = pageParams(c.query, &limit, &offset)) return *e;
  auto all = store_.calendarExceptions(id, truthy(c.query.queryItemValue(QStringLiteral("includeDeleted"))));
  if (!all) return storeError(all.error());
  QJsonArray items;
  for (const CalendarException& e : all.value()) items.append(toJson(e));
  return Response::json(200, page(items, limit, offset));
}

Response Api::addCalendarException(Call& c) {
  if (auto r = requireAllProjects(c.token)) return *r;
  const QString id = c.params.value(0);
  if (auto cal = store_.calendar(id); !cal) return storeError(cal.error());
  QSet<QString> readOnly = baseReadOnly();
  readOnly.insert(QStringLiteral("calendar"));
  QJsonObject templ = toJson(CalendarException{});
  if (auto e = checkFields(c.body, templ, readOnly, {})) return *e;
  CalendarException e = calendarExceptionFromJson(c.body);
  e.calendar = id;
  if (auto v = toValidationError(validateCalendarException(e))) return storeError(*v);
  if (c.dryRun) {
    QJsonObject after = toJson(e);
    for (const QString& k : baseReadOnly()) after.remove(k);
    auto rip = rippleOf(allProjectIds(), [&] { return statusOf(store_.addCalendarException(e, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, QJsonArray{change(QStringLiteral("create"), kCalendarException, {}, {}, after)},
                          rip.value());
  }
  auto r = store_.addCalendarException(e, {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.entityKind = kCalendarException;
  c.entityId = r->id;
  c.after = toJson(r.value());
  c.hasDiff = true;
  return Response::json(201, toJson(r.value()));
}

Response Api::deleteCalendarException(Call& c) {
  if (auto r = requireAllProjects(c.token)) return *r;
  const QString id = c.params.value(0);
  auto cur = store_.calendarException(id);
  if (!cur) return storeError(cur.error());
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  c.entityKind = kCalendarException;
  c.entityId = id;
  if (c.dryRun) {
    auto rip = rippleOf(allProjectIds(), [&] { return statusOf(store_.deleteCalendarException(id, cur->rev, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c,
                          QJsonArray{change(QStringLiteral("delete"), kCalendarException, id, kDeletedBefore,
                                            kDeletedAfter)},
                          rip.value());
  }
  if (auto refuse = requireConfirm(c)) return *refuse;
  auto r = store_.deleteCalendarException(id, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.before = kDeletedBefore;
  c.after = kDeletedAfter;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::importHolidays(Call& c) {
  if (auto r = requireAllProjects(c.token)) return *r;
  const QString id = c.params.value(0);
  if (!c.body.isEmpty()) {
    const QString key = c.body.constBegin().key();
    return validation(QStringLiteral("unknown field '%1'").arg(key), QJsonObject{{QStringLiteral("field"), key}});
  }
  const QString country = c.query.queryItemValue(QStringLiteral("country"));
  bool ok = false;
  const int year = c.query.queryItemValue(QStringLiteral("year")).toInt(&ok);
  if (!ok) return validation(QStringLiteral("year is required"), QJsonObject{{QStringLiteral("field"), QStringLiteral("year")}});
  c.entityKind = kCalendar;
  c.entityId = id;
  if (c.dryRun) {
    auto plan = store_.planImportHolidays(id, country, year);
    if (!plan) return storeError(plan.error());
    QJsonArray changes;
    for (const CalendarException& e : plan.value()) {
      QJsonObject after = toJson(e);
      for (const QString& k : baseReadOnly()) after.remove(k);
      changes.append(change(QStringLiteral("create"), kCalendarException, {}, {}, after));
    }
    auto rip = rippleOf(allProjectIds(), [&] { return statusOf(store_.importHolidays(id, country, year, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, changes, rip.value());
  }
  auto r = store_.importHolidays(id, country, year, {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  QJsonArray items;
  for (const CalendarException& e : r.value()) items.append(toJson(e));
  c.after = QJsonObject{{QStringLiteral("imported"), static_cast<qint64>(items.size())},
                        {QStringLiteral("country"), country},
                        {QStringLiteral("year"), year}};
  c.hasDiff = true;
  return Response::json(200, QJsonObject{{QStringLiteral("items"), items}});
}

// ---------------------------------------------------------------- schedule

Response Api::getSchedule(Call& c) {
  const QString id = c.params.value(0);
  if (auto a = checkProjectAccess(c, id); a.status) return a;
  auto sched = store_.projectSchedule(id);
  if (!sched) return storeError(sched.error());
  QJsonArray tasks;
  for (const QString& tid : sched->order) tasks.append(scheduledJson(*sched->task(tid)));
  QJsonArray critical;
  for (const QString& tid : sched->critical) critical.append(tid);
  QJsonArray violations;
  for (const ScheduleViolation& v : sched->violations) violations.append(violationJson(v, id));
  return Response::json(200, QJsonObject{{QStringLiteral("projectId"), id},
                                         {QStringLiteral("finish"), dateJson(sched->finish)},
                                         {QStringLiteral("critical"), critical},
                                         {QStringLiteral("violations"), violations},
                                         {QStringLiteral("tasks"), tasks}});
}

Response Api::getCriticalPath(Call& c) {
  const QString id = c.params.value(0);
  if (auto a = checkProjectAccess(c, id); a.status) return a;
  auto sched = store_.projectSchedule(id);
  if (!sched) return storeError(sched.error());
  auto ts = store_.tasks(id);
  if (!ts) return storeError(ts.error());
  QHash<QString, QString> titles;
  for (const Task& t : ts.value()) titles.insert(t.id, t.title);
  QJsonArray tasks;
  for (const QString& tid : sched->critical) {
    QJsonObject o = scheduledJson(*sched->task(tid));
    o.insert(QStringLiteral("title"), titles.value(tid));
    tasks.append(o);
  }
  return Response::json(200, QJsonObject{{QStringLiteral("projectId"), id},
                                         {QStringLiteral("finish"), dateJson(sched->finish)},
                                         {QStringLiteral("tasks"), tasks}});
}

Response Api::rescheduleProject(Call& c) {
  const QString id = c.params.value(0);
  if (auto a = checkProjectAccess(c, id); a.status) return a;
  if (!c.body.isEmpty()) {
    const QString key = c.body.constBegin().key();
    return validation(QStringLiteral("unknown field '%1'").arg(key), QJsonObject{{QStringLiteral("field"), key}});
  }
  if (auto p = store_.project(id); !p) return storeError(p.error());
  c.entityKind = kProject;
  c.entityId = id;
  if (c.dryRun) {
    auto rip = rippleOf({id}, [&] { return statusOf(store_.reschedule(id, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, {}, rip.value());
  }
  auto n = store_.reschedule(id, {c.token.id, c.requestId});
  if (!n) return storeError(n.error());
  c.after = QJsonObject{{QStringLiteral("rescheduled"), n.value()}};
  c.hasDiff = true;
  return Response::json(200, QJsonObject{{QStringLiteral("projectId"), id}, {QStringLiteral("changed"), n.value()}});
}

// ---------------------------------------------------------------- dependencies

namespace {

// A link's own fields; `project` may be given but must match the tasks'.
QSet<QString> dependencyReadOnly() { return baseReadOnly(); }

}  // namespace

Response Api::listDependencies(Call& c) {
  const QString id = c.params.value(0);
  if (auto a = checkProjectAccess(c, id); a.status) return a;
  int limit = 0;
  qsizetype offset = 0;
  if (auto e = pageParams(c.query, &limit, &offset)) return *e;
  if (auto p = store_.project(id); !p) return storeError(p.error());
  auto ds = store_.dependencies(id, truthy(c.query.queryItemValue(QStringLiteral("includeDeleted"))));
  if (!ds) return storeError(ds.error());
  const QString taskFilter = c.query.queryItemValue(QStringLiteral("task"));
  QJsonArray items;
  for (const Dependency& d : ds.value()) {
    if (!taskFilter.isEmpty() && d.predecessor != taskFilter && d.successor != taskFilter) continue;
    items.append(toJson(d));
  }
  return Response::json(200, page(items, limit, offset));
}

Response Api::createDependency(Call& c) {
  if (auto e = checkFields(c.body, toJson(Dependency{}), dependencyReadOnly(), {})) return *e;
  Dependency d = dependencyFromJson(c.body);
  if (auto e = toValidationError(validateDependency(d))) return storeError(*e);
  // Access follows the predecessor's project (both tasks must share it).
  if (auto pred = store_.task(d.predecessor); pred) {
    if (auto a = checkProjectAccess(c, pred->project); a.status) return a;
  }
  if (c.dryRun) {
    auto checked = store_.checkDependency(d);
    if (!checked) return storeError(checked.error());
    QJsonObject after = toJson(checked.value());
    for (const QString& k : baseReadOnly()) after.remove(k);
    auto rip = rippleOf({checked->project}, [&] { return statusOf(store_.createDependency(d, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, QJsonArray{change(QStringLiteral("create"), kDependency, {}, {}, after)}, rip.value());
  }
  auto r = store_.createDependency(d, {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.entityKind = kDependency;
  c.entityId = r->id;
  c.after = toJson(r.value());
  c.hasDiff = true;
  return Response::json(201, toJson(r.value()));
}

Response Api::getDependency(Call& c) {
  auto d = store_.dependency(c.params.value(0));
  if (!d) return storeError(d.error());
  if (auto a = checkProjectAccess(c, d->project); a.status) return a;
  return Response::json(200, toJson(d.value()));
}

Response Api::updateDependency(Call& c) {
  const QString id = c.params.value(0);
  auto cur = store_.dependency(id);
  if (!cur) return storeError(cur.error());
  if (auto a = checkProjectAccess(c, cur->project); a.status) return a;
  QSet<QString> readOnly = dependencyReadOnly();
  for (const char* k : {"project", "predecessor", "successor"}) readOnly.insert(QLatin1String(k));
  if (auto e = checkFields(c.body, toJson(Dependency{}), readOnly, {})) return *e;
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  QJsonObject merged = toJson(cur.value());
  for (auto it = c.body.constBegin(); it != c.body.constEnd(); ++it) merged.insert(it.key(), it.value());
  Dependency d = dependencyFromJson(merged);
  d.id = id;
  if (c.dryRun) {
    if (auto e = toValidationError(validateDependency(d))) return storeError(*e);
    const FieldDiff diff = diffFields(toJson(cur.value()), toJson(d));
    QJsonArray changes;
    if (!diff.empty()) changes.append(change(QStringLiteral("update"), kDependency, id, diff.before, diff.after));
    auto rip = rippleOf({cur->project}, [&] { return statusOf(store_.updateDependency(d, cur->rev, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c, changes, rip.value());
  }
  auto r = store_.updateDependency(d, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  const FieldDiff diff = diffFields(toJson(cur.value()), toJson(r.value()));
  c.entityKind = kDependency;
  c.entityId = id;
  c.before = diff.before;
  c.after = diff.after;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::deleteDependency(Call& c) {
  const QString id = c.params.value(0);
  auto cur = store_.dependency(id);
  if (!cur) return storeError(cur.error());
  if (auto a = checkProjectAccess(c, cur->project); a.status) return a;
  Response err = Response::empty(0);
  const auto expected = ifMatch(c, &err);
  if (err.status) return err;
  if (expected && *expected != cur->rev) return storeError(Error::revMismatch(*expected, cur->rev));
  c.entityKind = kDependency;
  c.entityId = id;
  if (c.dryRun) {
    auto rip = rippleOf({cur->project}, [&] { return statusOf(store_.deleteDependency(id, cur->rev, {c.token.id, c.requestId})); });
    if (!rip) return storeError(rip.error());
    return dryRunResponse(c,
                          QJsonArray{change(QStringLiteral("delete"), kDependency, id, kDeletedBefore, kDeletedAfter)},
                          rip.value());
  }
  if (auto refuse = requireConfirm(c)) return *refuse;
  auto r = store_.deleteDependency(id, expected.value_or(cur->rev), {c.token.id, c.requestId});
  if (!r) return storeError(r.error());
  c.before = kDeletedBefore;
  c.after = kDeletedAfter;
  c.hasDiff = true;
  return Response::json(200, toJson(r.value()));
}

Response Api::listChanges(Call& c) {
  if (!hasScope(c.token, QStringLiteral("projects:read"))) {
    return Response::error(403, QStringLiteral("forbidden_scope"),
                           QStringLiteral("this token lacks the 'projects:read' scope"),
                           QJsonObject{{QStringLiteral("required"), QStringLiteral("projects:read")}});
  }
  if (!allProjects(c.token)) {
    return Response::error(403, QStringLiteral("forbidden_project"),
                           QStringLiteral("the change feed covers every project; this token is limited to named "
                                          "projects"));
  }
  qint64 since = 0;
  int limit = 0;
  if (auto e = sinceParams(c.query, &since, &limit)) return *e;
  auto rows = store_.changesSince(since, limit + 1);
  if (!rows) return storeError(rows.error());
  QJsonArray items;
  qint64 last = since;
  for (qsizetype i = 0; i < rows->size() && i < limit; ++i) {
    items.append(changeJson(rows->at(i)));
    last = rows->at(i).seq;
  }
  return Response::json(200, QJsonObject{{QStringLiteral("items"), items},
                                         {QStringLiteral("nextSince"), last},
                                         {QStringLiteral("more"), rows->size() > limit}});
}

Response Api::listTokens(Call&) {
  auto all = tokens_.list();
  if (!all) return storeError(all.error());
  const QString nowIso = isoTimestamp(now());
  QJsonArray items;
  for (const auto& t : all.value()) items.append(tokenJson(t, nowIso));
  return Response::json(200, QJsonObject{{QStringLiteral("items"), items}});
}

Response Api::createToken(Call& c) {
  static const QSet<QString> allowedKeys{QStringLiteral("name"),          QStringLiteral("kind"),
                                         QStringLiteral("scopes"),        QStringLiteral("projects"),
                                         QStringLiteral("expiresInDays"), QStringLiteral("requireConfirm"),
                                         QStringLiteral("rateLimit")};
  for (auto it = c.body.constBegin(); it != c.body.constEnd(); ++it) {
    if (!allowedKeys.contains(it.key())) {
      return validation(QStringLiteral("unknown field '%1'").arg(it.key()),
                        QJsonObject{{QStringLiteral("field"), it.key()}});
    }
  }
  const QDateTime t = now();
  store::TokenRecord rec;
  rec.name = c.body.value(QStringLiteral("name")).toString().trimmed();
  if (rec.name.isEmpty() || rec.name.size() > 100) return validation(QStringLiteral("name is required (1-100 characters)"));
  rec.kind = c.body.value(QStringLiteral("kind")).toString();
  const bool agent = rec.kind == QLatin1String("agent");
  if (rec.kind != QLatin1String("device") && rec.kind != QLatin1String("app") && !agent) {
    return validation(QStringLiteral("kind must be device, app or agent (the owner token is created on the server)"));
  }
  // Scopes: agents default to read and never get admin (SPEC.md §9.2, Q24).
  const QJsonValue scopes = c.body.value(QStringLiteral("scopes"));
  if (scopes.isUndefined()) {
    rec.scopes = {rec.kind == QLatin1String("device") ? QStringLiteral("write") : QStringLiteral("read")};
  } else {
    if (!scopes.isArray() || scopes.toArray().isEmpty()) return validation(QStringLiteral("scopes must be a non-empty list"));
    for (const auto& v : scopes.toArray()) {
      if (!v.isString() || !knownScopes().contains(v.toString())) {
        return validation(QStringLiteral("unknown scope '%1'").arg(v.toString()));
      }
      rec.scopes.append(v.toString());
    }
    rec.scopes.removeDuplicates();
  }
  if (agent && rec.scopes.contains(QStringLiteral("admin"))) {
    return validation(QStringLiteral("agent tokens cannot have the admin scope"));
  }
  // Projects: agents are limited to named, existing projects.
  const QJsonValue projects = c.body.value(QStringLiteral("projects"));
  if (projects.isUndefined()) {
    if (agent) return validation(QStringLiteral("agent tokens need a list of project ids"));
    rec.projects = {QStringLiteral("*")};
  } else {
    if (!projects.isArray() || projects.toArray().isEmpty()) {
      return validation(QStringLiteral("projects must be a non-empty list of project ids or [\"*\"]"));
    }
    for (const auto& v : projects.toArray()) {
      const QString id = v.toString();
      if (id == QLatin1String("*")) {
        if (agent) return validation(QStringLiteral("agent tokens must name their projects"));
        rec.projects = {QStringLiteral("*")};
        break;
      }
      if (!v.isString() || !isValidId(id)) return validation(QStringLiteral("invalid project id '%1'").arg(id));
      if (auto p = store_.project(id); !p) return validation(QStringLiteral("project %1 does not exist").arg(id));
      rec.projects.append(id);
    }
    rec.projects.removeDuplicates();
  }
  // Expiry: agents 90 days by default and at most agentMaxExpiryDays.
  const QJsonValue days = c.body.value(QStringLiteral("expiresInDays"));
  int expiresInDays = agent ? options_.agentMaxExpiryDays : 0;
  if (!days.isUndefined() && !days.isNull()) {
    if (!days.isDouble() || days.toDouble() != static_cast<double>(days.toInt()) || days.toInt() < 1 ||
        days.toInt() > 3650) {
      return validation(QStringLiteral("expiresInDays must be a whole number of days, 1..3650"));
    }
    expiresInDays = days.toInt();
  } else if (days.isNull() && agent) {
    return validation(QStringLiteral("agent tokens always expire"));
  }
  if (agent && expiresInDays > options_.agentMaxExpiryDays) {
    return validation(QStringLiteral("agent tokens expire after at most %1 days").arg(options_.agentMaxExpiryDays));
  }
  if (expiresInDays > 0) rec.expiresAt = isoTimestamp(t.addDays(expiresInDays));
  const QJsonValue confirm = c.body.value(QStringLiteral("requireConfirm"));
  if (!confirm.isUndefined() && !confirm.isBool()) return validation(QStringLiteral("requireConfirm must be a boolean"));
  rec.requireConfirm = confirm.isBool() ? confirm.toBool() : agent;
  const QJsonValue rate = c.body.value(QStringLiteral("rateLimit"));
  if (!rate.isUndefined() && !rate.isNull()) {
    if (!rate.isObject()) return validation(QStringLiteral("rateLimit must be an object"));
    const QJsonObject rateObject = rate.toObject();
    for (auto it = rateObject.constBegin(); it != rateObject.constEnd(); ++it) {
      static const QSet<QString> keys{QStringLiteral("read"), QStringLiteral("write"), QStringLiteral("destructive"),
                                      QStringLiteral("total")};
      if (!keys.contains(it.key()) || !it.value().isDouble() || it.value().toInt() < 1 ||
          it.value().toInt() > 100000) {
        return validation(QStringLiteral("rateLimit.%1 must be read, write, destructive or total: 1..100000 per "
                                         "minute").arg(it.key()));
      }
    }
    rec.rateLimit = rateObject;
  }
  const NewToken nt = generateToken();
  rec.id = nt.id;
  rec.secretHash = nt.secretHash;
  rec.createdAt = isoTimestamp(t);
  rec.createdBy = c.token.id;
  if (auto s = tokens_.insert(rec); !s) return storeError(s.error());
  const QJsonObject meta = tokenJson(rec, rec.createdAt);
  c.entityKind = kToken;
  c.entityId = rec.id;
  c.after = meta;  // never the secret
  c.hasDiff = true;
  QJsonObject out = meta;
  out.insert(QStringLiteral("token"), nt.text());
  return Response::json(201, out);
}

Response Api::revokeToken(Call& c) {
  const QString id = c.params.value(0);
  auto rec = tokens_.find(id);
  if (!rec) return storeError(rec.error());
  c.entityKind = kToken;
  c.entityId = id;
  if (c.dryRun) {
    return dryRunResponse(c, QJsonArray{change(QStringLiteral("revoke"), kToken, id,
                                               QJsonObject{{QStringLiteral("revokedAt"), QJsonValue()}},
                                               QJsonObject{{QStringLiteral("revokedAt"), QStringLiteral("now")}})});
  }
  if (auto refuse = requireConfirm(c)) return *refuse;
  const QString at = isoTimestamp(now());
  if (rec->revokedAt.isEmpty()) {
    if (auto s = tokens_.revoke(id, at); !s) return storeError(s.error());
    rec.value().revokedAt = at;
    c.before = QJsonObject{{QStringLiteral("revokedAt"), QJsonValue()}};
    c.after = QJsonObject{{QStringLiteral("revokedAt"), at}};
    c.hasDiff = true;
  }
  return Response::json(200, tokenJson(rec.value(), at));
}

Response Api::listAudit(Call& c) {
  qint64 since = 0;
  int limit = 0;
  if (auto e = sinceParams(c.query, &since, &limit)) return *e;
  auto rows = audit_.since(since, limit + 1, c.query.queryItemValue(QStringLiteral("tokenId")));
  if (!rows) return storeError(rows.error());
  QJsonArray items;
  qint64 last = since;
  for (qsizetype i = 0; i < rows->size() && i < limit; ++i) {
    items.append(auditJson(rows->at(i)));
    last = rows->at(i).seq;
  }
  return Response::json(200, QJsonObject{{QStringLiteral("items"), items},
                                         {QStringLiteral("nextSince"), last},
                                         {QStringLiteral("more"), rows->size() > limit}});
}

Response Api::runBackup(Call& c) {
  if (options_.backupDir.isEmpty()) return validation(QStringLiteral("no backup directory is configured"));
  auto r = store::backup(store_, options_.backupDir, now().date());
  if (!r) return storeError(Error::internal(r.error().message));
  c.entityKind = QStringLiteral("backup");
  return Response::json(200, QJsonObject{{QStringLiteral("databaseFile"), r->databaseFile},
                                         {QStringLiteral("exportFile"), r->exportFile},
                                         {QStringLiteral("removed"), QJsonArray::fromStringList(r->removed)}});
}

Response Api::mcp(Call& c) { return mcp_->handle(*c.req, c.token); }

}  // namespace zproject::server
