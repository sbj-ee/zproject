#include "client/ApiError.hpp"

#include <QJsonArray>
#include <QVersionNumber>

namespace zproject::client {

namespace {

bool isDerivedSummaryField(const QString& f) {
  return f == QLatin1String("percentComplete") || f == QLatin1String("status") ||
         f == QLatin1String("actualStart") || f == QLatin1String("actualFinish");
}

}  // namespace

bool ApiError::goesOffline() const {
  return kind == ErrorKind::Server || kind == ErrorKind::Network || kind == ErrorKind::Timeout;
}

bool ApiError::stopsSync() const { return kind == ErrorKind::Unauthenticated || kind == ErrorKind::Tls; }

QString errorKindName(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::None: return QStringLiteral("none");
    case ErrorKind::Validation: return QStringLiteral("validation");
    case ErrorKind::SummaryRollup: return QStringLiteral("summary_rollup");
    case ErrorKind::Unauthenticated: return QStringLiteral("unauthenticated");
    case ErrorKind::ForbiddenScope: return QStringLiteral("forbidden_scope");
    case ErrorKind::ForbiddenProject: return QStringLiteral("forbidden_project");
    case ErrorKind::NotFound: return QStringLiteral("not_found");
    case ErrorKind::Conflict: return QStringLiteral("conflict");
    case ErrorKind::ConfirmRequired: return QStringLiteral("confirm_required");
    case ErrorKind::RevMismatch: return QStringLiteral("rev_mismatch");
    case ErrorKind::TooLarge: return QStringLiteral("too_large");
    case ErrorKind::Unprocessable: return QStringLiteral("unprocessable");
    case ErrorKind::RateLimited: return QStringLiteral("rate_limited");
    case ErrorKind::Server: return QStringLiteral("server");
    case ErrorKind::Network: return QStringLiteral("network");
    case ErrorKind::Timeout: return QStringLiteral("timeout");
    case ErrorKind::Tls: return QStringLiteral("tls");
    case ErrorKind::BadResponse: return QStringLiteral("bad_response");
  }
  return QStringLiteral("unknown");
}

ApiError classifyResponse(const ApiResponse& r) {
  ApiError e;
  e.status = r.status;
  e.code = r.errorCode();
  e.details = r.json.value(QStringLiteral("details")).toObject();
  e.requestId = QString::fromLatin1(r.header("X-Request-Id"));
  const QString serverText = r.json.value(QStringLiteral("error")).toString();

  if (r.status == 0) {
    if (r.tlsFailure) {
      e.kind = ErrorKind::Tls;
      e.message = QStringLiteral("The server's TLS certificate could not be verified (%1). zproject never connects "
                                 "without a valid certificate.")
                      .arg(r.networkError);
    } else if (r.timedOut) {
      e.kind = ErrorKind::Timeout;
      e.message = r.networkError;
    } else {
      e.kind = ErrorKind::Network;
      e.message = r.networkError;
    }
    return e;
  }
  if (r.ok()) {
    // Every /v1 answer with a body is a JSON object; anything else (cut off,
    // HTML from a proxy) is not trusted.
    if (!r.body.isEmpty() && r.json.isEmpty() && r.body.trimmed() != "{}") {
      e.kind = ErrorKind::BadResponse;
      e.message = QStringLiteral("The server sent an answer that is not valid JSON (HTTP %1).").arg(r.status);
    }
    return e;
  }

  const QJsonArray fields = e.details.value(QStringLiteral("fields")).toArray();
  if (!fields.isEmpty()) e.field = fields.first().toObject().value(QStringLiteral("field")).toString();
  if (e.field.isEmpty()) e.field = e.details.value(QStringLiteral("field")).toString();
  e.message = serverText.isEmpty() ? QStringLiteral("HTTP %1").arg(r.status) : serverText;

  switch (r.status) {
    case 400: {
      e.kind = ErrorKind::Validation;
      bool rollup = false;
      for (const QJsonValue& f : fields) {
        const QJsonObject o = f.toObject();
        if (isDerivedSummaryField(o.value(QStringLiteral("field")).toString()) &&
            o.value(QStringLiteral("message")).toString().contains(QLatin1String("summary task"))) {
          rollup = true;
        }
      }
      if (rollup) {
        e.kind = ErrorKind::SummaryRollup;
        e.message = QStringLiteral("This task now has subtasks; its progress is rolled up.");
      }
      break;
    }
    case 401:
      e.kind = ErrorKind::Unauthenticated;
      e.message = QStringLiteral("The server refused this machine's zproject token (invalid, expired or revoked).");
      break;
    case 403:
      e.kind = e.code == QLatin1String("forbidden_project") ? ErrorKind::ForbiddenProject : ErrorKind::ForbiddenScope;
      if (e.kind == ErrorKind::ForbiddenScope && e.details.contains(QStringLiteral("required"))) {
        e.message = QStringLiteral("This token lacks the '%1' scope.")
                        .arg(e.details.value(QStringLiteral("required")).toString());
      }
      break;
    case 404:
      e.kind = ErrorKind::NotFound;
      e.message = QStringLiteral("It was deleted elsewhere.");
      break;
    case 409:
      e.kind = e.code == QLatin1String("confirm_required") ? ErrorKind::ConfirmRequired : ErrorKind::Conflict;
      break;
    case 412:
      e.kind = ErrorKind::RevMismatch;
      e.message = QStringLiteral("Someone else changed this since you started editing.");
      break;
    case 413:
      e.kind = ErrorKind::TooLarge;
      break;
    case 429: {
      e.kind = ErrorKind::RateLimited;
      bool ok = false;
      const int s = QString::fromLatin1(r.header("Retry-After")).trimmed().toInt(&ok);
      e.retryAfterSeconds = ok && s >= 0 ? s : 60;
      e.message = QStringLiteral("Too many requests; retrying in %1 s.").arg(e.retryAfterSeconds);
      break;
    }
    default:
      e.kind = r.status >= 500 ? ErrorKind::Server : ErrorKind::Unprocessable;
      break;
  }
  return e;
}

Result<ServerHealth> parseHealth(const QJsonObject& json) {
  ServerHealth h;
  h.version = json.value(QStringLiteral("version")).toString();
  h.apiVersion = json.value(QStringLiteral("apiVersion")).toInt();
  h.schemaVersion = json.value(QStringLiteral("schemaVersion")).toInt();
  if (!json.value(QStringLiteral("ok")).toBool() || h.version.isEmpty() || h.apiVersion < 1 || h.schemaVersion < 1) {
    return Error::validation(QStringLiteral("the server's /v1/health answer is not a zprojectd health document"));
  }
  return h;
}

Compatibility checkCompatibility(const ServerHealth& server, const QString& clientVersion, int clientApiVersion) {
  Compatibility c;
  if (server.apiVersion != clientApiVersion) {
    c.level = Compatibility::Unsupported;
    c.message = QStringLiteral("The server speaks API v%1 (zprojectd %2); this zproject %3 speaks v%4. Install "
                               "matching versions.")
                    .arg(server.apiVersion)
                    .arg(server.version, clientVersion)
                    .arg(clientApiVersion);
    return c;
  }
  const QVersionNumber s = QVersionNumber::fromString(server.version);
  const QVersionNumber a = QVersionNumber::fromString(clientVersion);
  const int cmp = QVersionNumber::compare(s.normalized(), a.normalized());
  if (cmp < 0) {
    c.level = Compatibility::ServerOlder;
    c.message = QStringLiteral("The server runs zprojectd %1, older than this app (%2). Features it lacks are "
                               "unavailable.")
                    .arg(server.version, clientVersion);
  } else if (cmp > 0) {
    c.level = Compatibility::ServerNewer;
    c.message = QStringLiteral("The server runs zprojectd %1, newer than this app (%2). Update zproject when you can.")
                    .arg(server.version, clientVersion);
  }
  return c;
}

}  // namespace zproject::client
