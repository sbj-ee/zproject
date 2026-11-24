#pragma once

#include <QJsonObject>
#include <QString>

#include "client/ApiClient.hpp"
#include "core/Error.hpp"

namespace zproject::client {

// What a desktop client does with a /v1 answer (SPEC.md §9.3 error codes,
// client behavior in the "Desktop client" note there).
enum class ErrorKind {
  None,               // 2xx with a usable body
  Validation,         // 400 validation: show next to `field`
  SummaryRollup,      // 400 on a summary's derived field (SPEC.md §5.3)
  Unauthenticated,    // 401: token invalid, expired or revoked
  ForbiddenScope,     // 403 forbidden_scope: the action is disabled
  ForbiddenProject,   // 403 forbidden_project
  NotFound,           // 404: deleted elsewhere, refresh
  Conflict,           // 409 conflict or cycle
  ConfirmRequired,    // 409 confirm_required: dry-run again and confirm
  RevMismatch,        // 412 rev_mismatch: the conflict dialog
  TooLarge,           // 413
  Unprocessable,      // 422 (schedule_violation) and other 4xx
  RateLimited,        // 429: wait Retry-After
  Server,             // 5xx
  Network,            // no connection, refused, reset, closed early
  Timeout,            // no answer in time
  Tls,                // certificate or handshake failure: hard stop
  BadResponse,        // 2xx but the body is not the JSON object expected
};

struct ApiError {
  ErrorKind kind = ErrorKind::None;
  int status = 0;
  QString code;           // the server's machine code, if any
  QString message;        // text for the user (never a secret)
  QString field;          // validation: the first field named in details
  QJsonObject details;
  int retryAfterSeconds = -1;  // 429 only
  QString requestId;      // the server's X-Request-Id, for correlation

  bool ok() const { return kind == ErrorKind::None; }
  // The server can't be reached: show cached data read-only and retry with
  // backoff.
  bool goesOffline() const;
  // Polling stops until the user acts: a new token, or a TLS problem (never
  // bypassed).
  bool stopsSync() const;
};

QString errorKindName(ErrorKind kind);

// Classifies one answer. Never retries anything itself.
ApiError classifyResponse(const ApiResponse& r);

// GET /v1/health: {ok, version, apiVersion, schemaVersion}.
struct ServerHealth {
  QString version;
  int apiVersion = 0;
  int schemaVersion = 0;
};
Result<ServerHealth> parseHealth(const QJsonObject& json);

// Whether this client may talk to that server (SPEC.md §20-§21): another API
// major is refused; an older or newer server is allowed with an info note.
struct Compatibility {
  enum Level { Same, ServerOlder, ServerNewer, Unsupported };
  Level level = Same;
  QString message;  // empty for Same
  bool usable() const { return level != Unsupported; }
};
Compatibility checkCompatibility(const ServerHealth& server, const QString& clientVersion,
                                 int clientApiVersion);

}  // namespace zproject::client
