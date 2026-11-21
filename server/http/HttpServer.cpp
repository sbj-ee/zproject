#include "HttpServer.hpp"

#include <QTcpSocket>
#include <QTimer>

#include <memory>

namespace zproject::server::http {

Server::Server(Handler handler, QObject* parent) : Server(std::move(handler), Options{}, parent) {}

Server::Server(Handler handler, Options options, QObject* parent)
    : QObject(parent), handler_(std::move(handler)), options_(options) {
  connect(&tcp_, &QTcpServer::newConnection, this, &Server::onNewConnection);
}

bool Server::listen(const QHostAddress& address, quint16 port) {
  if (!address.isLoopback()) return false;
  return tcp_.listen(address, port);
}

void Server::onNewConnection() {
  while (QTcpSocket* socket = tcp_.nextPendingConnection()) {
    if (connections_ >= options_.maxConnections) {
      socket->write(serialize(Response::error(503, QStringLiteral("busy"),
                                              QStringLiteral("too many connections")), false));
      socket->disconnectFromHost();
      connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      continue;
    }
    ++connections_;
    auto parser = std::make_shared<Parser>(options_.limits);
    auto served = std::make_shared<int>(0);
    auto* idle = new QTimer(socket);
    idle->setSingleShot(true);
    idle->setInterval(options_.idleTimeoutMs);
    connect(idle, &QTimer::timeout, socket, [socket] { socket->abort(); });
    idle->start();

    // Context `this`: dropped automatically if the server goes first.
    connect(socket, &QTcpSocket::disconnected, this, [this] { --connections_; });
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    connect(socket, &QTcpSocket::readyRead, socket, [this, socket, parser, served, idle] {
      if (socket->state() != QAbstractSocket::ConnectedState) return;
      idle->start();
      auto state = parser->feed(socket->readAll());
      while (state == Parser::State::Ready) {
        Request req = parser->take();
        req.peer = socket->peerAddress();
        ++*served;
        const bool keepAlive = req.keepAlive && *served < options_.maxRequestsPerConnection;
        Response resp;
        try {
          resp = handler_(req);
        } catch (...) {
          resp = Response::error(500, QStringLiteral("internal"), QStringLiteral("internal error"));
        }
        socket->write(serialize(resp, keepAlive));
        if (!keepAlive) {
          socket->disconnectFromHost();
          return;
        }
        state = parser->state();
      }
      if (state == Parser::State::Error) {
        const int status = parser->errorStatus();
        const QString code = status == 413 ? QStringLiteral("too_large") : QStringLiteral("bad_request");
        socket->write(serialize(Response::error(status, code, parser->errorMessage()), false));
        socket->disconnectFromHost();
      }
    });
  }
}

}  // namespace zproject::server::http
