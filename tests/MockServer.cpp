#include "MockServer.hpp"

#include <QJsonDocument>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>

namespace zproject::test {

MockReply MockReply::json(int status, const QJsonObject& body) {
  MockReply r;
  r.status = status;
  r.headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
  r.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
  return r;
}

MockReply MockReply::error(int status, const QString& code, const QString& message, const QJsonObject& details) {
  return json(status, QJsonObject{{QStringLiteral("error"), message},
                                  {QStringLiteral("code"), code},
                                  {QStringLiteral("details"), details}});
}

MockServer::MockServer(QObject* parent) : QObject(parent) {
  connect(&server_, &QTcpServer::newConnection, this, &MockServer::onConnection);
  handler_ = [](const MockRequest&) { return MockReply::error(404, QStringLiteral("not_found"), QStringLiteral("no route")); };
}

MockServer::~MockServer() {
  // The sockets belong to server_, which outlives buffers_: unhook them first.
  for (QTcpSocket* s : server_.findChildren<QTcpSocket*>()) {
    s->disconnect(this);
    s->abort();
  }
}

bool MockServer::listen() { return server_.listen(QHostAddress::LocalHost, 0); }

QUrl MockServer::url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port())); }

void MockServer::onConnection() {
  while (QTcpSocket* s = server_.nextPendingConnection()) {
    connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadable(s); });
    connect(s, &QTcpSocket::disconnected, this, [this, s] {
      buffers_.remove(s);
      s->deleteLater();
    });
  }
}

void MockServer::onReadable(QTcpSocket* socket) {
  QByteArray& buf = buffers_[socket];
  buf += socket->readAll();
  if (buf.startsWith('\x16')) {  // a TLS ClientHello: answer as a plain HTTP server would
    socket->write("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    socket->disconnectFromHost();
    buf.clear();
    return;
  }
  for (;;) {
    const qsizetype end = buf.indexOf("\r\n\r\n");
    if (end < 0) return;
    const QList<QByteArray> lines = buf.left(end).split('\n');
    MockRequest req;
    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
    req.method = first.value(0);
    const QUrl target(QString::fromLatin1(first.value(1)));
    req.path = target.path();
    req.query = QUrlQuery(target);
    for (qsizetype i = 1; i < lines.size(); ++i) {
      const qsizetype colon = lines.at(i).indexOf(':');
      if (colon > 0) req.headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
    }
    const qsizetype length = req.header("Content-Length").toLongLong();
    if (buf.size() < end + 4 + length) return;  // wait for the body
    req.body = buf.mid(end + 4, length);
    buf.remove(0, end + 4 + length);
    requests_.append(req);
    answer(socket, req);
  }
}

void MockServer::answer(QTcpSocket* socket, const MockRequest& req) {
  const MockReply reply = handler_(req);
  ++open_;
  maxConcurrent_ = std::max(maxConcurrent_, open_);
  if (reply.hang) return;  // stays open until the client gives up
  const QPointer<QTcpSocket> s(socket);
  auto send = [this, s, reply] {
    --open_;
    QTcpSocket* sock = s.data();
    if (sock == nullptr) return;
    if (reply.drop) {
      sock->abort();
      return;
    }
    QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.status) + " X\r\n";
    for (const auto& h : reply.headers) out += h.first + ": " + h.second + "\r\n";
    out += "Content-Length: " + QByteArray::number(reply.body.size()) + "\r\n";
    out += reply.cutBody ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
    out += reply.cutBody ? reply.body.left(reply.body.size() / 2) : reply.body;
    sock->write(out);
    sock->flush();
    if (reply.cutBody) sock->disconnectFromHost();
  };
  if (reply.delayMs > 0) {
    QTimer::singleShot(reply.delayMs, this, send);
  } else {
    send();
  }
}

}  // namespace zproject::test
