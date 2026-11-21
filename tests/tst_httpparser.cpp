// The HTTP/1.1 request parser (server/http/HttpParser): framing, limits and
// the request-smuggling refusals (SPEC.md §4.3, §9.1).
#include <QTest>

#include "server/http/HttpParser.hpp"

using namespace zproject::server::http;

class TestHttpParser : public QObject {
  Q_OBJECT
 private slots:
  void simpleGet() {
    Parser p;
    QCOMPARE(p.feed("GET /v1/projects?limit=5&q=a%20b HTTP/1.1\r\nHost: 127.0.0.1:8788\r\nX-Thing: v\r\n\r\n"),
             Parser::State::Ready);
    const Request r = p.take();
    QCOMPARE(r.method, QByteArray("GET"));
    QCOMPARE(r.path, QStringLiteral("/v1/projects"));
    QCOMPARE(r.query.queryItemValue(QStringLiteral("limit")), QStringLiteral("5"));
    QCOMPARE(r.query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded), QStringLiteral("a b"));
    QCOMPARE(r.header("host"), QByteArray("127.0.0.1:8788"));
    QCOMPARE(r.header("x-thing"), QByteArray("v"));
    QVERIFY(r.keepAlive);
    QCOMPARE(p.state(), Parser::State::NeedMore);
  }

  void bodyInPiecesAndPipelining() {
    Parser p;
    const QByteArray two =
        "POST /v1/projects HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\n{\"name\":1}\n"
        "GET /v1/health HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";
    // Byte by byte: never Ready early, never an error.
    Parser::State s = Parser::State::NeedMore;
    qsizetype i = 0;
    for (; i < two.size() && s != Parser::State::Ready; ++i) s = p.feed(two.mid(i, 1));
    QCOMPARE(s, Parser::State::Ready);
    const Request first = p.take();
    QCOMPARE(first.body, QByteArray("{\"name\":1}\n"));
    QCOMPARE(p.feed(two.mid(i)), Parser::State::Ready);
    const Request second = p.take();
    QCOMPARE(second.path, QStringLiteral("/v1/health"));
    QVERIFY(!second.keepAlive);
  }

  void refusals_data() {
    QTest::addColumn<QByteArray>("raw");
    QTest::addColumn<int>("status");
    QTest::newRow("transfer-encoding") << QByteArray("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n") << 501;
    QTest::newRow("bare LF") << QByteArray("GET / HTTP/1.1\nHost: h\r\n\r\n") << 400;
    QTest::newRow("no host") << QByteArray("GET / HTTP/1.1\r\n\r\n") << 400;
    QTest::newRow("two hosts") << QByteArray("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n") << 400;
    QTest::newRow("conflicting lengths") << QByteArray("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n") << 400;
    QTest::newRow("negative length") << QByteArray("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n") << 400;
    QTest::newRow("plus length") << QByteArray("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: +1\r\n\r\n") << 400;
    QTest::newRow("too large") << QByteArray("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1048577\r\n\r\n") << 413;
    QTest::newRow("http/2") << QByteArray("GET / HTTP/2.0\r\nHost: h\r\n\r\n") << 505;
    QTest::newRow("absolute form") << QByteArray("GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n") << 400;
    QTest::newRow("folding") << QByteArray("GET / HTTP/1.1\r\nHost: h\r\nX: a\r\n b\r\n\r\n") << 400;
    QTest::newRow("space in name") << QByteArray("GET / HTTP/1.1\r\nHost: h\r\nBad Name: x\r\n\r\n") << 400;
    QTest::newRow("dot-dot") << QByteArray("GET /v1/../etc HTTP/1.1\r\nHost: h\r\n\r\n") << 400;
    QTest::newRow("encoded dot-dot") << QByteArray("GET /v1/%2e%2e/x HTTP/1.1\r\nHost: h\r\n\r\n") << 400;
    QTest::newRow("bad method") << QByteArray("G(T / HTTP/1.1\r\nHost: h\r\n\r\n") << 400;
  }
  void refusals() {
    QFETCH(QByteArray, raw);
    QFETCH(int, status);
    Parser p;
    QCOMPARE(p.feed(raw), Parser::State::Error);
    QCOMPARE(p.errorStatus(), status);
    // Errors are final.
    QCOMPARE(p.feed("GET / HTTP/1.1\r\nHost: h\r\n\r\n"), Parser::State::Error);
  }

  void headerLimits() {
    Parser p;
    QByteArray raw = "GET / HTTP/1.1\r\nHost: h\r\n";
    for (int i = 0; i < 65; ++i) raw += "X-" + QByteArray::number(i) + ": v\r\n";
    raw += "\r\n";
    QCOMPARE(p.feed(raw), Parser::State::Error);
    QCOMPARE(p.errorStatus(), 431);

    Parser q;
    QCOMPARE(q.feed("GET /" + QByteArray(9000, 'a')), Parser::State::Error);
    QCOMPARE(q.errorStatus(), 414);

    Parser r;
    QCOMPARE(r.feed("GET / HTTP/1.1\r\nHost: h\r\nX: " + QByteArray(50000, 'a')), Parser::State::Error);
    QCOMPARE(r.errorStatus(), 431);
  }

  void http10DefaultsToClose() {
    Parser p;
    QCOMPARE(p.feed("GET /v1/health HTTP/1.0\r\n\r\n"), Parser::State::Ready);
    QVERIFY(!p.take().keepAlive);
  }

  void serializeResponse() {
    Response r = Response::error(404, QStringLiteral("not_found"), QStringLiteral("nope"));
    const QByteArray bytes = serialize(r, true);
    QVERIFY(bytes.startsWith("HTTP/1.1 404 Not Found\r\n"));
    QVERIFY(bytes.contains("Content-Type: application/json\r\n"));
    QVERIFY(bytes.contains("Content-Length: " + QByteArray::number(r.body.size()) + "\r\n"));
    QVERIFY(bytes.contains("Connection: keep-alive\r\n"));
    QVERIFY(bytes.endsWith("{\"code\":\"not_found\",\"error\":\"nope\"}"));
  }
};

QTEST_GUILESS_MAIN(TestHttpParser)
#include "tst_httpparser.moc"
