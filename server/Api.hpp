#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

#include "server/RateLimiter.hpp"
#include "server/http/HttpTypes.hpp"
#include "store/Audit.hpp"
#include "store/SqliteStore.hpp"
#include "store/Tokens.hpp"

namespace zproject::server {

class Mcp;

// One /v1 operation (SPEC.md §9.4). The table is the router and, through
// the contract test, must agree with api/openapi.yaml: operationId,
// x-zproject-scope, x-zproject-destructive and x-zproject-dry-run.
struct RouteInfo {
  QByteArray method;
  QString pattern;      // /v1/projects/{id}
  QString operationId;
  QString scope;        // "" = any valid token; "none" = no token
  bool destructive = false;
  bool dryRun = false;  // accepts ?dryRun=true
};

struct ApiOptions {
  QStringList allowedHosts;  // Host header values (ServerConfig effectiveAllowedHosts)
  QString backupDir;
  int agentMaxExpiryDays = 90;
  int confirmTtlSeconds = 300;
  std::function<QDateTime()> now;   // UTC; tests move time forward
  RateLimiter::Clock rateClock;     // milliseconds; tests control refill
};

// The /v1 REST API and the /mcp endpoint on top of the store: Host and
// Origin checks, bearer-token authentication, scopes and project
// allowlists, rate limits, dry-run and confirm, If-Match, and the
// hash-chained audit log. Single-threaded, like the store.
class Api {
 public:
  Api(store::SqliteStore& store, ApiOptions options);
  ~Api();
  Api(const Api&) = delete;
  Api& operator=(const Api&) = delete;

  http::Response handle(const http::Request& req);

  static const QList<RouteInfo>& routes();

  // For /mcp tools: runs one /v1 operation as `token`, through the same
  // scope, allowlist, rate-limit, confirm and audit checks as HTTP.
  struct InternalCall {
    QByteArray method;
    QString path;
    QList<QPair<QString, QString>> query;
    QJsonObject body;
    bool hasBody = false;
    QByteArray ifMatch;
    QByteArray confirm;
    QString mcpTool;
  };
  http::Response callAs(const store::TokenRecord& token, const http::Request& outer, const InternalCall& call);

  // Creates the owner token (zprojectd --create-owner-token); returns the
  // token text, shown once.
  Result<QString> createOwnerToken(const QString& name);

  QDateTime now() const;
  static QString isoTimestamp(const QDateTime& t);
  store::SqliteStore& store() { return store_; }

  struct Call;  // per-request state (Api.cpp)

 private:
  using Handler = http::Response (Api::*)(Call&);
  struct Route {
    RouteInfo info;
    Handler handler = nullptr;
    bool transaction = true;  // run handler + audit in one transaction
  };
  static const QList<Route>& routeTable();
  const Route* match(const QByteArray& method, const QString& path, QStringList* params, bool* pathKnown) const;

  http::Response dispatch(Call& c);
  std::optional<http::Response> authenticate(const http::Request& req, store::TokenRecord* token, Call& c);
  void audit(const Call& c, int status);
  bool shouldAudit(const Call& c, int status) const;
  void maybeTouch(const store::TokenRecord& t);

  // Helpers
  http::Response storeError(const Error& e);
  http::Response checkProjectAccess(Call& c, const QString& projectId);
  std::optional<qint64> ifMatch(const Call& c, http::Response* error) const;
  QString requestHash(const Call& c) const;
  http::Response dryRunResponse(Call& c, const QJsonArray& changes);
  std::optional<http::Response> requireConfirm(Call& c);

  // Handlers
  http::Response getHealth(Call& c);
  http::Response getOpenApi(Call& c);
  http::Response listProjects(Call& c);
  http::Response createProject(Call& c);
  http::Response getProject(Call& c);
  http::Response updateProject(Call& c);
  http::Response deleteProject(Call& c);
  http::Response getWbs(Call& c);
  http::Response listTasks(Call& c);
  http::Response createTask(Call& c);
  http::Response getTask(Call& c);
  http::Response updateTask(Call& c);
  http::Response deleteTask(Call& c);
  http::Response getTaskHistory(Call& c);
  http::Response listChanges(Call& c);
  http::Response listTokens(Call& c);
  http::Response createToken(Call& c);
  http::Response revokeToken(Call& c);
  http::Response listAudit(Call& c);
  http::Response runBackup(Call& c);
  http::Response mcp(Call& c);

  store::SqliteStore& store_;
  ApiOptions options_;
  store::TokenStore tokens_;
  store::AuditLog audit_;
  RateLimiter limiter_;
  std::unique_ptr<Mcp> mcp_;
  QHash<QString, qint64> lastTouchMs_;
};

// The embedded api/openapi.json (generated from api/openapi.yaml).
const QByteArray& openApiJson();

}  // namespace zproject::server
