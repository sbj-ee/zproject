// The HTTP layer over real loopback sockets (SPEC.md §4.3): keep-alive,
// pipelining, limits, malformed input, the connection cap and the idle
// timeout.
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QTest>

#include "server/http/HttpServer.hpp"

using namespace zproject::server::http;

namespace {

Response echo(const Request& r) {
  Response out;
  out.status = 200;
  out.contentType = "text/plain";
  out.body = r.method + ' ' + r.target + ' ' + QByteArray::number(r.body.size());
  return out;
}

// Reads until `n` complete responses (by Content-Length) or the peer closes.
QList<QByteArray> readResponses(QTcpSocket& s, int n, int timeoutMs = 5000) {
  QByteArray buf;
  QList<QByteArray> out;
  QElapsedTimer t;
  t.start();
  while (out.size() < n && t.elapsed() < timeoutMs) {
    for (;;) {
      const qsizetype end = buf.indexOf("\r\n\r\n");
      if (end < 0) break;
      qsizetype len = 0;
      for (const QByteArray& line : buf.left(end).split('\n')) {
        if (line.toLower().startsWith("content-length:")) len = line.mid(15).trimmed().toLongLong();
      }
      if (buf.size() < end + 4 + len) break;
      out.append(buf.left(end + 4 + len));
      buf.remove(0, end + 4 + len);
    }
    if (out.size() >= n) break;
    if (s.state() != QAbstractSocket::ConnectedState && s.bytesAvailable() == 0) break;
    QTest::qWait(20);
    buf += s.readAll();
  }
  return out;
}

bool waitClosed(QTcpSocket& s, int timeoutMs = 5000) {
  QElapsedTimer t;
  t.start();
  while (s.state() != QAbstractSocket::UnconnectedState && t.elapsed() < timeoutMs) {
    QTest::qWait(20);
    s.readAll();
  }
  return s.state() == QAbstractSocket::UnconnectedState;
}

}  // namespace

class TestHttpServer : public QObject {
  Q_OBJECT
 private slots:
  void keepAliveAndPipelining() {
    Server server(echo);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, server.port());
    QVERIFY(s.waitForConnected(3000));
    s.write("GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
            "POST /b HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc");
    const auto rs = readResponses(s, 2);
    QCOMPARE(rs.size(), 2);
    QVERIFY(rs[0].startsWith("HTTP/1.1 200 OK\r\n"));
    QVERIFY(rs[0].endsWith("GET /a 0"));
    QVERIFY(rs[1].endsWith("POST /b 3"));
    QVERIFY(rs[0].contains("Cache-Control: no-store"));
    QVERIFY(rs[0].contains("X-Content-Type-Options: nosniff"));
    // Still open for a third.
    s.write("GET /c HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    const auto r3 = readResponses(s, 1);
    QCOMPARE(r3.size(), 1);
    QVERIFY(r3[0].contains("Connection: close"));
    QVERIFY(waitClosed(s));
  }

  void oversizedBodyIs413AndCloses() {
    Server server(echo);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, server.port());
    QVERIFY(s.waitForConnected(3000));
    s.write("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999\r\n\r\n");
    const auto rs = readResponses(s, 1);
    QCOMPARE(rs.size(), 1);
    QVERIFY2(rs[0].startsWith("HTTP/1.1 413 "), rs[0].constData());
    QVERIFY(waitClosed(s));
  }

  void malformedIs400AndCloses() {
    Server server(echo);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, server.port());
    QVERIFY(s.waitForConnected(3000));
    s.write("GET /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    auto rs = readResponses(s, 1);
    QCOMPARE(rs.size(), 1);
    QVERIFY2(rs[0].startsWith("HTTP/1.1 501 "), rs[0].constData());
    QVERIFY(waitClosed(s));

    QTcpSocket s2;
    s2.connectToHost(QHostAddress::LocalHost, server.port());
    QVERIFY(s2.waitForConnected(3000));
    s2.write("garbage\r\n\r\n");
    rs = readResponses(s2, 1);
    QCOMPARE(rs.size(), 1);
    QVERIFY(rs[0].startsWith("HTTP/1.1 400 "));
    QVERIFY(waitClosed(s2));
  }

  void handlerExceptionIs500() {
    Server server([](const Request&) -> Response { throw std::runtime_error("boom"); });
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, server.port());
    QVERIFY(s.waitForConnected(3000));
    s.write("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto rs = readResponses(s, 1);
    QCOMPARE(rs.size(), 1);
    QVERIFY(rs[0].startsWith("HTTP/1.1 500 "));
    QVERIFY(!rs[0].contains("boom"));
  }

  void connectionCapAndIdleTimeout() {
    Server::Options o;
    o.maxConnections = 1;
    o.idleTimeoutMs = 300;
    Server server(echo, o);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket a;
    a.connectToHost(QHostAddress::LocalHost, server.port());
    QVERIFY(a.waitForConnected(3000));
    QTRY_COMPARE(server.connectionCount(), 1);
    QTcpSocket b;
    b.connectToHost(QHostAddress::LocalHost, server.port());
    QVERIFY(b.waitForConnected(3000));
    const auto rs = readResponses(b, 1);
    QCOMPARE(rs.size(), 1);
    QVERIFY(rs[0].startsWith("HTTP/1.1 503 "));
    // The idle one is dropped after the timeout.
    QVERIFY(waitClosed(a, 3000));
    QTRY_COMPARE(server.connectionCount(), 0);
  }
};

QTEST_GUILESS_MAIN(TestHttpServer)
#include "tst_httpserver.moc"
