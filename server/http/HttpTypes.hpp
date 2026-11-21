#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrlQuery>

namespace zproject::server::http {

// One parsed HTTP/1.1 request. Header names are lower-cased.
struct Request {
  QByteArray method;  // GET, POST, ...
  QByteArray target;  // as sent: /v1/projects?limit=10
  QString path;       // percent-decoded path, no query
  QUrlQuery query;
  QList<QPair<QByteArray, QByteArray>> headers;
  QByteArray body;
  QHostAddress peer;
  bool keepAlive = true;

  QByteArray header(const QByteArray& lowerName) const {
    for (const auto& h : headers) {
      if (h.first == lowerName) return h.second;
    }
    return {};
  }
  bool hasHeader(const QByteArray& lowerName) const {
    for (const auto& h : headers) {
      if (h.first == lowerName) return true;
    }
    return false;
  }
};

struct Response {
  int status = 200;
  QList<QPair<QByteArray, QByteArray>> headers;  // Content-Length/Connection are added by the server
  QByteArray body;
  QByteArray contentType = "application/json";

  static Response json(int status, const QJsonObject& body);
  static Response empty(int status);
  // {"error": message, "code": code, "details": {...}} (SPEC.md §9.3).
  static Response error(int status, const QString& code, const QString& message,
                        const QJsonObject& details = {});
};

const char* reasonPhrase(int status);
// The full response bytes: status line, headers, body.
QByteArray serialize(const Response& r, bool keepAlive);

}  // namespace zproject::server::http
