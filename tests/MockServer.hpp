#pragma once

// A fault-injecting HTTP/1.1 server for the desktop client tests (SPEC.md
// §19): scripted answers on a loopback port, with delays, hangs, connection
// drops and bodies cut short. Records every request. Never used outside
// tests; it speaks just enough HTTP for QNetworkAccessManager.
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTcpServer>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

class QTcpSocket;

namespace zproject::test {

struct MockRequest {
  QByteArray method;
  QString path;
  QUrlQuery query;
  QHash<QByteArray, QByteArray> headers;  // lower-case names
  QByteArray body;
  QByteArray header(const QByteArray& name) const { return headers.value(name.toLower()); }
};

struct MockReply {
  int status = 200;
  QList<QPair<QByteArray, QByteArray>> headers;
  QByteArray body;
  int delayMs = 0;           // wait before answering
  bool hang = false;         // never answer (the client must time out)
  bool drop = false;         // close the connection without an answer
  bool cutBody = false;      // announce the full length, send half, close

  static MockReply json(int status, const QJsonObject& body);
  static MockReply error(int status, const QString& code, const QString& message, const QJsonObject& details = {});
};

class MockServer : public QObject {
  Q_OBJECT

 public:
  using Handler = std::function<MockReply(const MockRequest&)>;

  explicit MockServer(QObject* parent = nullptr);
  ~MockServer() override;
  MockServer(const MockServer&) = delete;
  MockServer& operator=(const MockServer&) = delete;
  bool listen();
  quint16 port() const { return server_.serverPort(); }
  QUrl url() const;

  void setHandler(Handler h) { handler_ = std::move(h); }
  const QList<MockRequest>& requests() const { return requests_; }
  void clearRequests() { requests_.clear(); }
  int maxConcurrent() const { return maxConcurrent_; }  // requests open at once

 private:
  void onConnection();
  void onReadable(QTcpSocket* socket);
  void answer(QTcpSocket* socket, const MockRequest& req);

  QTcpServer server_;
  Handler handler_;
  QList<MockRequest> requests_;
  QHash<QTcpSocket*, QByteArray> buffers_;
  int open_ = 0;
  int maxConcurrent_ = 0;
};

}  // namespace zproject::test
