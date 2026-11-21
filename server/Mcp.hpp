#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

#include "server/http/HttpTypes.hpp"
#include "store/Tokens.hpp"

namespace zproject::server {

class Api;

// The MCP endpoint (SPEC.md §11, decision Q25): JSON-RPC 2.0 over the
// Streamable HTTP transport at POST /mcp, answering with application/json
// (no SSE stream, no sessions). Behind the same token auth as /v1. Each tool
// runs as the matching /v1 operation through Api::callAs, so scopes, project
// allowlists, rate limits, dry-run/confirm and the audit log apply
// unchanged; tools/list shows only the tools the token's scopes allow.
class Mcp {
 public:
  struct Tool {
    QString name;
    QString title;
    QString description;
    QString scope;
    bool destructive = false;
    QJsonObject inputSchema;
  };

  explicit Mcp(Api& api) : api_(api) {}

  http::Response handle(const http::Request& req, const store::TokenRecord& token);

  static const QList<Tool>& tools();
  static const QStringList& supportedProtocolVersions();

 private:
  QJsonObject callTool(const http::Request& req, const store::TokenRecord& token, const QString& name,
                       const QJsonObject& args, QJsonObject* rpcError);
  QJsonObject listResources(const http::Request& req, const store::TokenRecord& token);
  QJsonObject readResource(const http::Request& req, const store::TokenRecord& token, const QString& uri,
                           QJsonObject* rpcError);

  Api& api_;
};

}  // namespace zproject::server
