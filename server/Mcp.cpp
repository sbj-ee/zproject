#include "Mcp.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include "core/Id.hpp"
#include "core/Version.hpp"
#include "server/Api.hpp"
#include "server/Auth.hpp"

namespace zproject::server {
namespace {

using http::Response;

QJsonObject schema(const QJsonObject& properties, const QStringList& required = {}) {
  QJsonObject s{{QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("properties"), properties},
                {QStringLiteral("additionalProperties"), false}};
  if (!required.isEmpty()) s.insert(QStringLiteral("required"), QJsonArray::fromStringList(required));
  return s;
}

QJsonObject prop(const QString& type, const QString& description) {
  return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("description"), description}};
}

QJsonObject idProp(const QString& what) {
  return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                     {QStringLiteral("pattern"), QStringLiteral("^[0-9a-f]{20}$")},
                     {QStringLiteral("description"), what}};
}

QJsonObject taskFields() {
  return QJsonObject{
      {QStringLiteral("title"), prop(QStringLiteral("string"), QStringLiteral("Task title"))},
      {QStringLiteral("parent"), idProp(QStringLiteral("Parent task id (omit for top level)"))},
      {QStringLiteral("type"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                           {QStringLiteral("enum"), QJsonArray{QStringLiteral("task"),
                                                                               QStringLiteral("milestone")}}}},
      {QStringLiteral("duration"), prop(QStringLiteral("number"), QStringLiteral("Working days; 0 for milestones"))},
      {QStringLiteral("start"), prop(QStringLiteral("string"), QStringLiteral("YYYY-MM-DD"))},
      {QStringLiteral("finish"), prop(QStringLiteral("string"), QStringLiteral("YYYY-MM-DD"))},
      {QStringLiteral("deadline"), prop(QStringLiteral("string"), QStringLiteral("YYYY-MM-DD"))},
      {QStringLiteral("notes"), prop(QStringLiteral("string"), QStringLiteral("Markdown notes"))},
      {QStringLiteral("percentComplete"), prop(QStringLiteral("integer"), QStringLiteral("0-100"))},
      {QStringLiteral("priority"), prop(QStringLiteral("integer"), QStringLiteral("1 (highest) to 4"))},
  };
}

QJsonObject rpcResult(const QJsonValue& id, const QJsonObject& result) {
  return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
                     {QStringLiteral("result"), result}};
}

QJsonObject rpcError(const QJsonValue& id, int code, const QString& message) {
  return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), id},
                     {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                                                           {QStringLiteral("message"), message}}}};
}

QJsonObject bodyOf(const Response& r) { return QJsonDocument::fromJson(r.body).object(); }

QJsonObject toolResult(const Response& r) {
  const QJsonObject body = bodyOf(r);
  return QJsonObject{
      {QStringLiteral("content"),
       QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                              {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(body).toJson(
                                                           QJsonDocument::Compact))}}}},
      {QStringLiteral("structuredContent"), body},
      {QStringLiteral("isError"), r.status >= 400}};
}

}  // namespace

const QStringList& Mcp::supportedProtocolVersions() {
  static const QStringList v{QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"),
                             QStringLiteral("2024-11-05")};
  return v;
}

const QList<Mcp::Tool>& Mcp::tools() {
  static const QList<Tool> t = [] {
    QJsonObject create = taskFields();
    create.insert(QStringLiteral("project"), idProp(QStringLiteral("Project id")));
    QJsonObject update = taskFields();
    update.insert(QStringLiteral("id"), idProp(QStringLiteral("Task id")));
    update.insert(QStringLiteral("rev"),
                  prop(QStringLiteral("integer"), QStringLiteral("Expected revision (If-Match); optional")));
    update.insert(QStringLiteral("status"),
                  QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                              {QStringLiteral("enum"), QJsonArray{QStringLiteral("todo"), QStringLiteral("in_progress"),
                                                                  QStringLiteral("done")}}});
    return QList<Tool>{
        {QStringLiteral("list_projects"), QStringLiteral("List projects"),
         QStringLiteral("List the projects this token may see. Read-only."), QStringLiteral("projects:read"), false,
         schema({})},
        {QStringLiteral("get_project"), QStringLiteral("Get a project"),
         QStringLiteral("One project's details. Read-only."), QStringLiteral("projects:read"), false,
         schema({{QStringLiteral("id"), idProp(QStringLiteral("Project id"))}}, {QStringLiteral("id")})},
        {QStringLiteral("search_tasks"), QStringLiteral("Search tasks"),
         QStringLiteral("Find tasks by text in the title or notes, optionally in one project and by status. "
                        "Read-only."),
         QStringLiteral("tasks:read"), false,
         schema({{QStringLiteral("query"), prop(QStringLiteral("string"), QStringLiteral("Text to look for"))},
                 {QStringLiteral("project"), idProp(QStringLiteral("Limit to one project"))},
                 {QStringLiteral("status"),
                  QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                              {QStringLiteral("enum"), QJsonArray{QStringLiteral("todo"), QStringLiteral("in_progress"),
                                                                  QStringLiteral("done")}}}},
                 {QStringLiteral("limit"), prop(QStringLiteral("integer"), QStringLiteral("At most this many (1-200, "
                                                                                           "default 50)"))}})},
        {QStringLiteral("get_task"), QStringLiteral("Get a task"), QStringLiteral("One task's details. Read-only."),
         QStringLiteral("tasks:read"), false,
         schema({{QStringLiteral("id"), idProp(QStringLiteral("Task id"))}}, {QStringLiteral("id")})},
        {QStringLiteral("get_schedule"), QStringLiteral("Get a project's schedule"),
         QStringLiteral("Every task's early and late dates, total and free float (working days) and whether it is "
                        "critical, the critical tasks, the forecast finish, and schedule violations (a manual task "
                        "breaking a link, a missed deadline or finish-no-later-than date). Read-only."),
         QStringLiteral("schedule:read"), false,
         schema({{QStringLiteral("project"), idProp(QStringLiteral("Project id"))}}, {QStringLiteral("project")})},
        {QStringLiteral("get_critical_path"), QStringLiteral("Get the critical path"),
         QStringLiteral("The tasks with no float, in order of start, with titles and dates: what delays the project "
                        "if it slips. Read-only."),
         QStringLiteral("schedule:read"), false,
         schema({{QStringLiteral("project"), idProp(QStringLiteral("Project id"))}}, {QStringLiteral("project")})},
        {QStringLiteral("create_task"), QStringLiteral("Create a task"),
         QStringLiteral("Add a task to a project. Writes; not destructive; no confirmation needed."),
         QStringLiteral("tasks:write"), false, schema(create, {QStringLiteral("project"), QStringLiteral("title")})},
        {QStringLiteral("update_task"), QStringLiteral("Update a task"),
         QStringLiteral("Change fields of a task. Writes; not destructive; no confirmation needed. Pass rev to "
                        "refuse the change if the task moved on."),
         QStringLiteral("tasks:write"), false, schema(update, {QStringLiteral("id")})},
        {QStringLiteral("add_dependency"), QStringLiteral("Link two tasks"),
         QStringLiteral("Make one task depend on another in the same project: FS (default) starts the successor "
                        "after the predecessor finishes, SS after it starts, FF finishes it no earlier; lag is "
                        "whole working days (negative: a lead). Refused if the tasks are already linked or the "
                        "link would make a loop. Writes; not destructive; no confirmation needed."),
         QStringLiteral("tasks:write"), false,
         schema({{QStringLiteral("predecessor"), idProp(QStringLiteral("The task that comes first"))},
                 {QStringLiteral("successor"), idProp(QStringLiteral("The task that depends on it"))},
                 {QStringLiteral("type"),
                  QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                              {QStringLiteral("enum"),
                               QJsonArray{QStringLiteral("FS"), QStringLiteral("SS"), QStringLiteral("FF")}}}},
                 {QStringLiteral("lag"), prop(QStringLiteral("integer"),
                                              QStringLiteral("Working days between them; negative is a lead"))}},
                {QStringLiteral("predecessor"), QStringLiteral("successor")})},
        {QStringLiteral("delete_task"), QStringLiteral("Delete a task"),
         QStringLiteral("DESTRUCTIVE, needs confirmation. Without confirm_id: a dry run that lists the task and "
                        "its subtree and returns a confirmId (valid 5 minutes, single use). Call again with "
                        "confirm_id to delete. Deletes are tombstones and can be restored."),
         QStringLiteral("tasks:write"), true,
         schema({{QStringLiteral("id"), idProp(QStringLiteral("Task id"))},
                 {QStringLiteral("confirm_id"),
                  prop(QStringLiteral("string"), QStringLiteral("confirmId from the dry run"))}},
                {QStringLiteral("id")})},
    };
  }();
  return t;
}

Response Mcp::handle(const http::Request& req, const store::TokenRecord& token) {
  if (!req.header("content-type").toLower().startsWith("application/json")) {
    return Response::error(415, QStringLiteral("unsupported_media_type"),
                           QStringLiteral("send Content-Type: application/json"));
  }
  QJsonParseError perr{};
  const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
  if (perr.error != QJsonParseError::NoError) {
    return Response::json(400, rpcError(QJsonValue(), -32700, QStringLiteral("parse error")));
  }
  if (!doc.isObject()) {
    return Response::json(400, rpcError(QJsonValue(), -32600, QStringLiteral("batches are not supported")));
  }
  const QJsonObject msg = doc.object();
  const QJsonValue id = msg.value(QStringLiteral("id"));
  const QString method = msg.value(QStringLiteral("method")).toString();
  if (msg.value(QStringLiteral("jsonrpc")).toString() != QLatin1String("2.0") || method.isEmpty()) {
    // A response or something else from the client: accepted, nothing to say.
    if (msg.contains(QStringLiteral("result")) || msg.contains(QStringLiteral("error"))) return Response::empty(202);
    return Response::json(400, rpcError(id, -32600, QStringLiteral("invalid request")));
  }
  if (id.isUndefined()) return Response::empty(202);  // notification
  const QJsonObject params = msg.value(QStringLiteral("params")).toObject();

  if (method == QLatin1String("initialize")) {
    const QString asked = params.value(QStringLiteral("protocolVersion")).toString();
    const QString version =
        supportedProtocolVersions().contains(asked) ? asked : supportedProtocolVersions().constFirst();
    return Response::json(
        200, rpcResult(id, QJsonObject{
                               {QStringLiteral("protocolVersion"), version},
                               {QStringLiteral("capabilities"),
                                QJsonObject{{QStringLiteral("tools"),
                                             QJsonObject{{QStringLiteral("listChanged"), false}}},
                                            {QStringLiteral("resources"),
                                             QJsonObject{{QStringLiteral("listChanged"), false}}}}},
                               {QStringLiteral("serverInfo"),
                                QJsonObject{{QStringLiteral("name"), QStringLiteral("zprojectd")},
                                            {QStringLiteral("version"), appVersion()}}},
                               {QStringLiteral("instructions"),
                                QStringLiteral("zproject plans: projects and their task trees. Tools are filtered "
                                               "by this token's scopes. delete_task is a two-step call: dry run, "
                                               "then confirm_id.")}}));
  }
  if (method == QLatin1String("ping")) return Response::json(200, rpcResult(id, {}));
  if (method == QLatin1String("tools/list")) {
    QJsonArray list;
    for (const Tool& t : tools()) {
      if (!hasScope(token, t.scope)) continue;
      list.append(QJsonObject{
          {QStringLiteral("name"), t.name},
          {QStringLiteral("title"), t.title},
          {QStringLiteral("description"), t.description},
          {QStringLiteral("inputSchema"), t.inputSchema},
          {QStringLiteral("annotations"),
           QJsonObject{{QStringLiteral("readOnlyHint"), t.scope.endsWith(QLatin1String(":read"))},
                       {QStringLiteral("destructiveHint"), t.destructive},
                       {QStringLiteral("openWorldHint"), false}}}});
    }
    return Response::json(200, rpcResult(id, QJsonObject{{QStringLiteral("tools"), list}}));
  }
  if (method == QLatin1String("tools/call")) {
    QJsonObject err;
    const QJsonObject result = callTool(req, token, params.value(QStringLiteral("name")).toString(),
                                        params.value(QStringLiteral("arguments")).toObject(), &err);
    if (!err.isEmpty()) {
      return Response::json(200, rpcError(id, err.value(QStringLiteral("code")).toInt(),
                                          err.value(QStringLiteral("message")).toString()));
    }
    return Response::json(200, rpcResult(id, result));
  }
  if (method == QLatin1String("resources/list")) {
    return Response::json(200, rpcResult(id, listResources(req, token)));
  }
  if (method == QLatin1String("resources/templates/list")) {
    return Response::json(
        200, rpcResult(id, QJsonObject{{QStringLiteral("resourceTemplates"),
                                        QJsonArray{QJsonObject{
                                            {QStringLiteral("uriTemplate"), QStringLiteral("zproject://projects/{id}/summary")},
                                            {QStringLiteral("name"), QStringLiteral("Project summary")},
                                            {QStringLiteral("mimeType"), QStringLiteral("application/json")}}}}}));
  }
  if (method == QLatin1String("resources/read")) {
    QJsonObject err;
    const QJsonObject result = readResource(req, token, params.value(QStringLiteral("uri")).toString(), &err);
    if (!err.isEmpty()) {
      return Response::json(200, rpcError(id, err.value(QStringLiteral("code")).toInt(),
                                          err.value(QStringLiteral("message")).toString()));
    }
    return Response::json(200, rpcResult(id, result));
  }
  return Response::json(200, rpcError(id, -32601, QStringLiteral("method not found: %1").arg(method)));
}

QJsonObject Mcp::callTool(const http::Request& req, const store::TokenRecord& token, const QString& name,
                          const QJsonObject& args, QJsonObject* err) {
  const Tool* tool = nullptr;
  for (const Tool& t : tools()) {
    if (t.name == name && hasScope(token, t.scope)) tool = &t;
  }
  if (!tool) {
    *err = QJsonObject{{QStringLiteral("code"), -32602}, {QStringLiteral("message"), QStringLiteral("unknown tool: %1").arg(name)}};
    return {};
  }
  auto bad = [&](const QString& message) {
    *err = QJsonObject{{QStringLiteral("code"), -32602}, {QStringLiteral("message"), message}};
    return QJsonObject{};
  };
  // Arguments must match the tool's schema keys.
  const QJsonObject props = tool->inputSchema.value(QStringLiteral("properties")).toObject();
  for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
    if (!props.contains(it.key())) return bad(QStringLiteral("unknown argument '%1'").arg(it.key()));
  }
  for (const auto& r : tool->inputSchema.value(QStringLiteral("required")).toArray()) {
    if (!args.contains(r.toString())) return bad(QStringLiteral("missing argument '%1'").arg(r.toString()));
  }
  auto idArg = [&](const char* key) { return args.value(QLatin1String(key)).toString(); };
  for (const char* key : {"id", "project", "parent", "predecessor", "successor"}) {
    if (args.contains(QLatin1String(key)) && !isValidId(idArg(key))) {
      return bad(QStringLiteral("'%1' must be a 20-character hex id").arg(QLatin1String(key)));
    }
  }

  Api::InternalCall call;
  call.mcpTool = name;
  if (name == QLatin1String("list_projects")) {
    call.method = "GET";
    call.path = QStringLiteral("/v1/projects");
    call.query = {{QStringLiteral("limit"), QStringLiteral("500")}};
  } else if (name == QLatin1String("get_project")) {
    call.method = "GET";
    call.path = QStringLiteral("/v1/projects/") + idArg("id");
  } else if (name == QLatin1String("get_task")) {
    call.method = "GET";
    call.path = QStringLiteral("/v1/tasks/") + idArg("id");
  } else if (name == QLatin1String("search_tasks")) {
    const int limit = args.contains(QStringLiteral("limit")) ? args.value(QStringLiteral("limit")).toInt() : 50;
    if (limit < 1 || limit > 200) return bad(QStringLiteral("limit must be 1..200"));
    QStringList projectIds;
    if (args.contains(QStringLiteral("project"))) {
      projectIds.append(idArg("project"));
    } else {
      Api::InternalCall list{"GET", QStringLiteral("/v1/projects"), {{QStringLiteral("limit"), QStringLiteral("500")}},
                             {}, false, {}, {}, name};
      const Response r = api_.callAs(token, req, list);
      if (r.status >= 400) return toolResult(r);
      for (const auto& p : bodyOf(r).value(QStringLiteral("items")).toArray()) {
        projectIds.append(p.toObject().value(QStringLiteral("id")).toString());
      }
    }
    QJsonArray found;
    for (const QString& pid : projectIds) {
      Api::InternalCall one{"GET", QStringLiteral("/v1/projects/%1/tasks").arg(pid),
                            {{QStringLiteral("limit"), QString::number(limit)}}, {}, false, {}, {}, name};
      if (args.contains(QStringLiteral("query"))) one.query.append({QStringLiteral("q"), idArg("query")});
      if (args.contains(QStringLiteral("status"))) one.query.append({QStringLiteral("status"), idArg("status")});
      const Response r = api_.callAs(token, req, one);
      if (r.status >= 400) return toolResult(r);
      for (const auto& t : bodyOf(r).value(QStringLiteral("items")).toArray()) {
        if (found.size() < limit) found.append(t);
      }
      if (found.size() >= limit) break;
    }
    Response combined = Response::json(200, QJsonObject{{QStringLiteral("items"), found}});
    return toolResult(combined);
  } else if (name == QLatin1String("create_task")) {
    call.method = "POST";
    call.path = QStringLiteral("/v1/projects/%1/tasks").arg(idArg("project"));
    call.body = args;
    call.body.remove(QStringLiteral("project"));
    call.hasBody = true;
  } else if (name == QLatin1String("update_task")) {
    call.method = "PATCH";
    call.path = QStringLiteral("/v1/tasks/") + idArg("id");
    call.body = args;
    call.body.remove(QStringLiteral("id"));
    call.body.remove(QStringLiteral("rev"));
    call.hasBody = true;
    if (args.contains(QStringLiteral("rev"))) {
      call.ifMatch = QByteArray::number(args.value(QStringLiteral("rev")).toInteger());
    }
  } else if (name == QLatin1String("get_schedule")) {
    call.method = "GET";
    call.path = QStringLiteral("/v1/projects/%1/schedule").arg(idArg("project"));
  } else if (name == QLatin1String("get_critical_path")) {
    call.method = "GET";
    call.path = QStringLiteral("/v1/projects/%1/critical-path").arg(idArg("project"));
  } else if (name == QLatin1String("add_dependency")) {
    call.method = "POST";
    call.path = QStringLiteral("/v1/dependencies");
    call.body = args;
    call.hasBody = true;
  } else if (name == QLatin1String("delete_task")) {
    call.method = "DELETE";
    call.path = QStringLiteral("/v1/tasks/") + idArg("id");
    if (args.contains(QStringLiteral("confirm_id"))) {
      call.confirm = idArg("confirm_id").toLatin1();
    } else {
      call.query = {{QStringLiteral("dryRun"), QStringLiteral("true")}};
    }
  }
  return toolResult(api_.callAs(token, req, call));
}

QJsonObject Mcp::listResources(const http::Request& req, const store::TokenRecord& token) {
  QJsonArray list{QJsonObject{{QStringLiteral("uri"), QStringLiteral("zproject://openapi")},
                              {QStringLiteral("name"), QStringLiteral("zproject OpenAPI 3.1 description")},
                              {QStringLiteral("mimeType"), QStringLiteral("application/json")}}};
  if (hasScope(token, QStringLiteral("projects:read"))) {
    Api::InternalCall call{"GET", QStringLiteral("/v1/projects"), {{QStringLiteral("limit"), QStringLiteral("500")}},
                           {}, false, {}, {}, QStringLiteral("resources/list")};
    const Response r = api_.callAs(token, req, call);
    for (const auto& p : bodyOf(r).value(QStringLiteral("items")).toArray()) {
      const QJsonObject o = p.toObject();
      list.append(QJsonObject{
          {QStringLiteral("uri"), QStringLiteral("zproject://projects/%1/summary").arg(o.value(QStringLiteral("id")).toString())},
          {QStringLiteral("name"), o.value(QStringLiteral("name")).toString()},
          {QStringLiteral("mimeType"), QStringLiteral("application/json")}});
    }
  }
  return QJsonObject{{QStringLiteral("resources"), list}};
}

QJsonObject Mcp::readResource(const http::Request& req, const store::TokenRecord& token, const QString& uri,
                              QJsonObject* err) {
  auto contents = [&](const QByteArray& text) {
    return QJsonObject{{QStringLiteral("contents"),
                        QJsonArray{QJsonObject{{QStringLiteral("uri"), uri},
                                               {QStringLiteral("mimeType"), QStringLiteral("application/json")},
                                               {QStringLiteral("text"), QString::fromUtf8(text)}}}}};
  };
  if (uri == QLatin1String("zproject://openapi")) return contents(openApiJson());
  static const QRegularExpression re(QStringLiteral("^zproject://projects/([0-9a-f]{20})/summary$"));
  const auto m = re.match(uri);
  if (!m.hasMatch()) {
    *err = QJsonObject{{QStringLiteral("code"), -32002}, {QStringLiteral("message"), QStringLiteral("resource not found")}};
    return {};
  }
  const QString pid = m.captured(1);
  const QString tool = QStringLiteral("resources/read");
  const Response p = api_.callAs(token, req, {"GET", QStringLiteral("/v1/projects/") + pid, {}, {}, false, {}, {}, tool});
  if (p.status >= 400) {
    *err = QJsonObject{{QStringLiteral("code"), -32002},
                       {QStringLiteral("message"), bodyOf(p).value(QStringLiteral("error")).toString()}};
    return {};
  }
  const Response t = api_.callAs(token, req, {"GET", QStringLiteral("/v1/projects/%1/tasks").arg(pid),
                                              {{QStringLiteral("limit"), QStringLiteral("500")}}, {}, false, {}, {}, tool});
  QJsonObject counts{{QStringLiteral("total"), 0}, {QStringLiteral("todo"), 0}, {QStringLiteral("in_progress"), 0},
                     {QStringLiteral("done"), 0}, {QStringLiteral("milestones"), 0}};
  if (t.status < 400) {
    const QJsonArray items = bodyOf(t).value(QStringLiteral("items")).toArray();
    counts[QStringLiteral("total")] = static_cast<int>(items.size());
    for (const auto& v : items) {
      const QJsonObject o = v.toObject();
      const QString st = o.value(QStringLiteral("status")).toString();
      counts[st] = counts.value(st).toInt() + 1;
      if (o.value(QStringLiteral("type")).toString() == QLatin1String("milestone")) {
        counts[QStringLiteral("milestones")] = counts.value(QStringLiteral("milestones")).toInt() + 1;
      }
    }
  }
  const QJsonObject summary{{QStringLiteral("project"), bodyOf(p)}, {QStringLiteral("tasks"), counts}};
  return contents(QJsonDocument(summary).toJson(QJsonDocument::Compact));
}

}  // namespace zproject::server
