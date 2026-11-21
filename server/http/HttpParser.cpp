#include "HttpParser.hpp"

#include <QJsonDocument>
#include <QUrl>

namespace zproject::server::http {
namespace {

bool isTokenChar(char c) {
  // RFC 9110 tchar
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
    case '-': case '.': case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

bool isToken(const QByteArray& s) {
  if (s.isEmpty()) return false;
  for (char c : s) {
    if (!isTokenChar(c)) return false;
  }
  return true;
}

bool hasControlChars(const QByteArray& s) {
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if ((u < 0x20 && u != '\t') || u == 0x7f) return true;
  }
  return false;
}

}  // namespace

Parser::State Parser::fail(int status, const QString& message) {
  state_ = State::Error;
  errorStatus_ = status;
  errorMessage_ = message;
  buffer_.clear();
  return state_;
}

Parser::State Parser::feed(const QByteArray& data) {
  if (state_ == State::Error) return state_;
  buffer_.append(data);
  if (state_ == State::Ready) return state_;
  return parse();
}

Parser::State Parser::parse() {
  if (!headDone_) {
    // Tolerate empty lines before a request line (RFC 9112 §2.2).
    while (buffer_.startsWith("\r\n")) buffer_.remove(0, 2);
    const qsizetype end = buffer_.indexOf("\r\n\r\n");
    if (end < 0) {
      const qsizetype firstLine = buffer_.indexOf("\r\n");
      if ((firstLine < 0 && buffer_.size() > limits_.maxRequestLine) || firstLine > limits_.maxRequestLine) {
        return fail(414, QStringLiteral("request line too long"));
      }
      if (buffer_.size() > limits_.maxRequestLine + limits_.maxHeaderBytes) {
        return fail(431, QStringLiteral("request headers too large"));
      }
      return State::NeedMore;
    }
    if (end > limits_.maxRequestLine + limits_.maxHeaderBytes) {
      return fail(431, QStringLiteral("request headers too large"));
    }
    const QByteArray head = buffer_.left(end);
    buffer_.remove(0, end + 4);
    if (!parseHead(head)) return state_;
    headDone_ = true;
  }
  if (buffer_.size() < bodyLength_) return State::NeedMore;
  current_.body = buffer_.left(bodyLength_);
  buffer_.remove(0, bodyLength_);
  state_ = State::Ready;
  return state_;
}

bool Parser::parseHead(const QByteArray& head) {
  const QList<QByteArray> lines = head.split('\n');
  // Every line must end in CRLF; a bare LF is refused (request smuggling).
  for (qsizetype i = 0; i + 1 < lines.size(); ++i) {
    if (!lines[i].endsWith('\r')) {
      fail(400, QStringLiteral("bare LF in request head"));
      return false;
    }
  }
  auto stripCr = [](QByteArray l) {
    if (l.endsWith('\r')) l.chop(1);
    return l;
  };
  const QByteArray requestLine = stripCr(lines.value(0));
  if (requestLine.size() > limits_.maxRequestLine) {
    fail(414, QStringLiteral("request line too long"));
    return false;
  }
  const QList<QByteArray> parts = requestLine.split(' ');
  if (parts.size() != 3 || !isToken(parts[0]) || parts[1].isEmpty() || hasControlChars(parts[1])) {
    fail(400, QStringLiteral("malformed request line"));
    return false;
  }
  if (parts[2] != "HTTP/1.1" && parts[2] != "HTTP/1.0") {
    fail(505, QStringLiteral("only HTTP/1.1 is supported"));
    return false;
  }
  if (!parts[1].startsWith('/')) {
    fail(400, QStringLiteral("only origin-form request targets are supported"));
    return false;
  }
  current_ = Request{};
  current_.method = parts[0];
  current_.target = parts[1];
  const qsizetype q = parts[1].indexOf('?');
  const QByteArray rawPath = q < 0 ? parts[1] : parts[1].left(q);
  if (rawPath.contains('#')) {
    fail(400, QStringLiteral("fragment in request target"));
    return false;
  }
  current_.path = QUrl::fromPercentEncoding(rawPath);
  if (current_.path.contains(QChar(0)) || current_.path.contains(QStringLiteral("/../")) ||
      current_.path.endsWith(QStringLiteral("/.."))) {
    fail(400, QStringLiteral("invalid path"));
    return false;
  }
  if (q >= 0) current_.query = QUrlQuery(QString::fromUtf8(parts[1].mid(q + 1)));
  bool http10 = parts[2] == "HTTP/1.0";

  qsizetype headerBytes = 0;
  bool haveLength = false;
  qint64 length = 0;
  for (qsizetype i = 1; i < lines.size(); ++i) {
    const QByteArray line = stripCr(lines[i]);
    headerBytes += line.size();
    if (line.isEmpty()) continue;
    if (line.startsWith(' ') || line.startsWith('\t')) {
      fail(400, QStringLiteral("obsolete header folding"));
      return false;
    }
    const qsizetype colon = line.indexOf(':');
    if (colon <= 0) {
      fail(400, QStringLiteral("malformed header line"));
      return false;
    }
    const QByteArray name = line.left(colon).toLower();
    const QByteArray value = line.mid(colon + 1).trimmed();
    if (!isToken(name) || hasControlChars(value)) {
      fail(400, QStringLiteral("malformed header"));
      return false;
    }
    if (current_.headers.size() >= limits_.maxHeaders) {
      fail(431, QStringLiteral("too many headers"));
      return false;
    }
    if (name == "transfer-encoding") {
      fail(501, QStringLiteral("Transfer-Encoding is not supported; send Content-Length"));
      return false;
    }
    if (name == "content-length") {
      bool ok = false;
      const qint64 n = value.toLongLong(&ok);
      if (!ok || n < 0 || value.startsWith('+') || (haveLength && n != length)) {
        fail(400, QStringLiteral("invalid Content-Length"));
        return false;
      }
      haveLength = true;
      length = n;
    }
    if (name == "host" && current_.hasHeader("host")) {
      fail(400, QStringLiteral("duplicate Host header"));
      return false;
    }
    current_.headers.append({name, value});
  }
  if (headerBytes > limits_.maxHeaderBytes) {
    fail(431, QStringLiteral("request headers too large"));
    return false;
  }
  if (!http10 && !current_.hasHeader("host")) {
    fail(400, QStringLiteral("missing Host header"));
    return false;
  }
  if (length > limits_.maxBody) {
    fail(413, QStringLiteral("request body larger than %1 bytes").arg(limits_.maxBody));
    return false;
  }
  bodyLength_ = static_cast<qsizetype>(length);
  const QByteArray connection = current_.header("connection").toLower();
  current_.keepAlive = http10 ? connection.contains("keep-alive") : !connection.contains("close");
  return true;
}

Request Parser::take() {
  Request r = std::move(current_);
  current_ = Request{};
  headDone_ = false;
  bodyLength_ = 0;
  state_ = State::NeedMore;
  if (!buffer_.isEmpty()) parse();
  return r;
}

Response Response::json(int status, const QJsonObject& body) {
  Response r;
  r.status = status;
  r.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
  return r;
}

Response Response::empty(int status) {
  Response r;
  r.status = status;
  r.contentType.clear();
  return r;
}

Response Response::error(int status, const QString& code, const QString& message, const QJsonObject& details) {
  QJsonObject o{{QStringLiteral("error"), message}, {QStringLiteral("code"), code}};
  if (!details.isEmpty()) o.insert(QStringLiteral("details"), details);
  return json(status, o);
}

const char* reasonPhrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 409: return "Conflict";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Status";
  }
}

QByteArray serialize(const Response& r, bool keepAlive) {
  QByteArray out;
  out.reserve(256 + r.body.size());
  out += "HTTP/1.1 " + QByteArray::number(r.status) + ' ' + reasonPhrase(r.status) + "\r\n";
  if (!r.contentType.isEmpty() && r.status != 204) out += "Content-Type: " + r.contentType + "\r\n";
  out += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
  out += "Cache-Control: no-store\r\n";
  out += "X-Content-Type-Options: nosniff\r\n";
  for (const auto& h : r.headers) out += h.first + ": " + h.second + "\r\n";
  out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  out += "\r\n";
  out += r.body;
  return out;
}

}  // namespace zproject::server::http
