#pragma once

#include <QHostAddress>
#include <QObject>
#include <QTcpServer>

#include <functional>

#include "server/http/HttpParser.hpp"
#include "server/http/HttpTypes.hpp"

namespace zproject::server::http {

// zproject's own small HTTP/1.1 server on QTcpServer (SPEC.md §4.3): one
// thread, keep-alive, no TLS (tailscale serve terminates TLS in front of it),
// loopback only. Each complete request is handed to the handler; its
// response is written back in order.
class Server : public QObject {
  Q_OBJECT
 public:
  using Handler = std::function<Response(const Request&)>;

  struct Options {
    Limits limits;
    int idleTimeoutMs = 30'000;
    int maxConnections = 64;
    int maxRequestsPerConnection = 1000;
  };

  explicit Server(Handler handler, QObject* parent = nullptr);
  Server(Handler handler, Options options, QObject* parent = nullptr);

  // Listens on `address` (must be loopback; the caller validates config).
  bool listen(const QHostAddress& address, quint16 port);
  quint16 port() const { return tcp_.serverPort(); }
  QString errorString() const { return tcp_.errorString(); }
  void close() { tcp_.close(); }
  int connectionCount() const { return connections_; }

 private:
  void onNewConnection();

  QTcpServer tcp_;
  Handler handler_;
  Options options_;
  int connections_ = 0;
};

}  // namespace zproject::server::http
