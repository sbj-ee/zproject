// The desktop's non-blocking /v1 client (SPEC.md §4.1, §9.3) against the
// fault-injecting mock server: headers (token, If-Match, dry run, confirm),
// error answers, timeouts, slow and cut-off answers, dropped connections,
// 429 pauses, no redirects, the read cap and per-entity write order, and
// strict TLS. The token is made up.
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <QtNetwork/qtnetworkglobal.h>
#if QT_CONFIG(ssl)
#include <QSslCertificate>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#endif

#include "MockServer.hpp"
#include "client/ApiError.hpp"
#include "client/AsyncApiClient.hpp"
#include "core/Version.hpp"

using namespace zproject;
using namespace zproject::client;
using zproject::test::MockReply;
using zproject::test::MockRequest;
using zproject::test::MockServer;

namespace {

const QByteArray kToken = "zpt_0123456789abcdef0123." + QByteArray(43, 'A');

// Runs one request and waits for its callback.
ApiResponse call(AsyncApiClient& api, ApiRequest req, int waitMs = 5000) {
  ApiResponse out;
  bool done = false;
  api.send(std::move(req), [&](const ApiResponse& r) {
    out = r;
    done = true;
  });
  if (!QTest::qWaitFor([&] { return done; }, waitMs)) qWarning("request did not finish");
  return out;
}

ApiRequest get(const QString& path) {
  ApiRequest r;
  r.path = path;
  return r;
}

}  // namespace

class TestAsyncApi : public QObject {
  Q_OBJECT

  std::unique_ptr<MockServer> mock_;
  std::unique_ptr<AsyncApiClient> api_;

 private slots:
  void init() {
    mock_ = std::make_unique<MockServer>();
    QVERIFY(mock_->listen());
    api_ = std::make_unique<AsyncApiClient>(mock_->url(), kToken);
  }
  void cleanup() {
    api_.reset();
    mock_.reset();
  }

  void sendsTokenAndJson() {
    mock_->setHandler([](const MockRequest&) { return MockReply::json(200, {{QStringLiteral("ok"), true}}); });
    bool sync = true;
    bool called = false;
    api_->get(QStringLiteral("/v1/health"), {}, [&](const ApiResponse&) {
      called = true;
      QVERIFY(!sync);  // never inside get()
    });
    sync = false;
    QTRY_VERIFY(called);
    const MockRequest& r = mock_->requests().first();
    QCOMPARE(r.method, QByteArray("GET"));
    QCOMPARE(r.path, QStringLiteral("/v1/health"));
    QCOMPARE(r.header("authorization"), "Bearer " + kToken);
    QCOMPARE(r.header("accept"), QByteArray("application/json"));
    QCOMPARE(r.header("user-agent"), QStringLiteral("zproject/%1").arg(appVersion()).toLatin1());
    QVERIFY(!r.header("x-zproject-client-request").isEmpty());
    QVERIFY(r.header("if-match").isEmpty());
  }

  void patchSendsIfMatchDryRunAndConfirm() {
    mock_->setHandler([](const MockRequest&) { return MockReply::json(200, {{QStringLiteral("dryRun"), true}}); });
    bool done = false;
    api_->patch(QStringLiteral("/v1/tasks/abc"), {{QStringLiteral("title"), QStringLiteral("New")}}, 7,
                [&](const ApiResponse& r) {
                  QVERIFY(r.ok());
                  done = true;
                },
                true, "confirm-1");
    QTRY_VERIFY(done);
    const MockRequest& r = mock_->requests().first();
    QCOMPARE(r.method, QByteArray("PATCH"));
    QCOMPARE(r.header("if-match"), QByteArray("7"));
    QCOMPARE(r.query.queryItemValue(QStringLiteral("dryRun")), QStringLiteral("true"));
    QCOMPARE(r.header("x-zproject-confirm"), QByteArray("confirm-1"));
    QCOMPARE(r.header("content-type"), QByteArray("application/json"));
    QCOMPARE(QJsonDocument::fromJson(r.body).object().value(QStringLiteral("title")).toString(), QStringLiteral("New"));
  }

  void errorAnswersAreMapped() {
    mock_->setHandler([](const MockRequest& r) {
      if (r.path == QLatin1String("/412")) {
        return MockReply::error(412, QStringLiteral("rev_mismatch"), QStringLiteral("revision mismatch"),
                                {{QStringLiteral("expected"), 3}, {QStringLiteral("current"), 4}});
      }
      if (r.path == QLatin1String("/500")) return MockReply::error(500, QStringLiteral("internal"), QStringLiteral("boom"));
      MockReply m = MockReply::error(429, QStringLiteral("rate_limited"), QStringLiteral("slow down"));
      m.headers.append({QByteArray("Retry-After"), QByteArray("1")});
      return m;
    });
    ApiError e = classifyResponse(call(*api_, get(QStringLiteral("/412"))));
    QCOMPARE(e.kind, ErrorKind::RevMismatch);
    QCOMPARE(e.details.value(QStringLiteral("current")).toInt(), 4);
    QCOMPARE(classifyResponse(call(*api_, get(QStringLiteral("/500")))).kind, ErrorKind::Server);
    e = classifyResponse(call(*api_, get(QStringLiteral("/429"))));
    QCOMPARE(e.kind, ErrorKind::RateLimited);
    QCOMPARE(e.retryAfterSeconds, 1);
  }

  // After a 429 nothing new starts until Retry-After has passed; nothing is
  // retried or lost.
  void rateLimitPausesTheQueue() {
    int n = 0;
    mock_->setHandler([&](const MockRequest&) {
      if (n++ == 0) {
        MockReply m = MockReply::error(429, QStringLiteral("rate_limited"), QStringLiteral("slow down"));
        m.headers.append({QByteArray("Retry-After"), QByteArray("1")});
        return m;
      }
      return MockReply::json(200, {{QStringLiteral("ok"), true}});
    });
    QCOMPARE(call(*api_, get(QStringLiteral("/a"))).status, 429);
    QVERIFY(api_->rateLimitRemainingMs() > 0);
    QElapsedTimer t;
    t.start();
    const ApiResponse r = call(*api_, get(QStringLiteral("/b")));
    QCOMPARE(r.status, 200);
    QVERIFY2(t.elapsed() >= 800, qPrintable(QString::number(t.elapsed())));
    QCOMPARE(mock_->requests().size(), 2);
  }

  void timeoutsAndSlowAnswers() {
    mock_->setHandler([](const MockRequest& r) {
      MockReply m = MockReply::json(200, {{QStringLiteral("ok"), true}});
      if (r.path == QLatin1String("/hang")) m.hang = true;
      if (r.path == QLatin1String("/slow")) m.delayMs = 150;
      return m;
    });
    api_->setTimeoutMs(1000);
    ApiResponse r = call(*api_, get(QStringLiteral("/slow")));
    QCOMPARE(r.status, 200);  // slow but in time
    api_->setTimeoutMs(300);
    r = call(*api_, get(QStringLiteral("/hang")));
    QCOMPARE(r.status, 0);
    QVERIFY(r.timedOut);
    const ApiError e = classifyResponse(r);
    QCOMPARE(e.kind, ErrorKind::Timeout);
    QVERIFY(e.goesOffline());
  }

  void brokenAnswers() {
    mock_->setHandler([](const MockRequest& r) {
      MockReply m = MockReply::json(200, {{QStringLiteral("items"), QJsonArray{1, 2, 3}}});
      if (r.path == QLatin1String("/drop")) m.drop = true;
      if (r.path == QLatin1String("/cut")) m.cutBody = true;
      if (r.path == QLatin1String("/truncated")) m.body = "{\"items\": [{\"seq\": 1,";
      return m;
    });
    QCOMPARE(classifyResponse(call(*api_, get(QStringLiteral("/truncated")))).kind, ErrorKind::BadResponse);
    const ApiError drop = classifyResponse(call(*api_, get(QStringLiteral("/drop"))));
    QVERIFY2(drop.kind == ErrorKind::Network, qPrintable(errorKindName(drop.kind)));
    const ApiError cut = classifyResponse(call(*api_, get(QStringLiteral("/cut"))));
    QVERIFY2(cut.kind == ErrorKind::Network || cut.kind == ErrorKind::BadResponse, qPrintable(errorKindName(cut.kind)));
    QVERIFY(!cut.ok());
  }

  void connectionRefused() {
    quint16 port = mock_->port();
    mock_.reset();  // nothing listens there now
    AsyncApiClient api(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port)), kToken);
    const ApiError e = classifyResponse(call(api, get(QStringLiteral("/v1/health"))));
    QCOMPARE(e.kind, ErrorKind::Network);
    QVERIFY(e.goesOffline());
  }

  // A redirect is returned as is; the token never goes to the other host.
  void redirectsAreNotFollowed() {
    MockServer other;
    QVERIFY(other.listen());
    other.setHandler([](const MockRequest&) { return MockReply::json(200, {}); });
    const QByteArray target = other.url().toString().toLatin1() + "/v1/health";
    mock_->setHandler([&](const MockRequest&) {
      MockReply m;
      m.status = 302;
      m.headers.append({QByteArray("Location"), target});
      return m;
    });
    const ApiResponse r = call(*api_, get(QStringLiteral("/v1/health")));
    QCOMPARE(r.status, 302);
    QTest::qWait(100);
    QVERIFY(other.requests().isEmpty());
    QVERIFY(!classifyResponse(r).ok());
  }

  void readsAreCappedAndWritesSerialized() {
    mock_->setHandler([](const MockRequest&) {
      MockReply m = MockReply::json(200, {});
      m.delayMs = 100;
      return m;
    });
    int done = 0;
    for (int i = 0; i < 10; ++i) api_->get(QStringLiteral("/r%1").arg(i), {}, [&](const ApiResponse&) { ++done; });
    QTRY_COMPARE_WITH_TIMEOUT(done, 10, 5000);
    QVERIFY2(mock_->maxConcurrent() <= 4, qPrintable(QString::number(mock_->maxConcurrent())));
    QVERIFY(mock_->maxConcurrent() >= 2);

    // Writes to one entity go one at a time and in order.
    mock_ = std::make_unique<MockServer>();
    QVERIFY(mock_->listen());
    mock_->setHandler([](const MockRequest& r) {
      MockReply m = MockReply::json(200, {});
      m.delayMs = r.body.contains("\"n\":0") ? 150 : 10;  // the first is slow
      return m;
    });
    api_ = std::make_unique<AsyncApiClient>(mock_->url(), kToken);
    QList<int> order;
    for (int i = 0; i < 3; ++i) {
      api_->patch(QStringLiteral("/v1/tasks/same"), {{QStringLiteral("n"), i}}, i + 1,
                  [&order, i](const ApiResponse&) { order.append(i); });
    }
    QTRY_COMPARE_WITH_TIMEOUT(order.size(), 3, 5000);
    QCOMPARE(order, (QList<int>{0, 1, 2}));
    QCOMPARE(mock_->maxConcurrent(), 1);
  }

  void cancelAll() {
    mock_->setHandler([](const MockRequest&) {
      MockReply m;
      m.hang = true;
      return m;
    });
    int cancelled = 0;
    for (int i = 0; i < 6; ++i) {
      api_->get(QStringLiteral("/x"), {}, [&](const ApiResponse& r) {
        if (r.status == 0) ++cancelled;
      });
    }
    QTRY_VERIFY(api_->running() > 0);
    api_->cancelAll();
    QTRY_COMPARE(cancelled, 6);
    QCOMPARE(api_->queued(), 0);
  }

  // https to something that doesn't speak TLS fails the handshake: a TLS
  // error, a hard stop, never offline-and-retry.
  void tlsHandshakeFailureIsHardStop() {
#if QT_CONFIG(ssl)
    if (!QSslSocket::supportsSsl()) QSKIP("no TLS backend");
    mock_->setHandler([](const MockRequest&) { return MockReply::json(200, {}); });
    AsyncApiClient api(QUrl(QStringLiteral("https://127.0.0.1:%1").arg(mock_->port())), kToken);
    api.setTimeoutMs(3000);
    const ApiResponse r = call(api, get(QStringLiteral("/v1/health")));
    const ApiError e = classifyResponse(r);
    QVERIFY2(e.kind == ErrorKind::Tls, qPrintable(errorKindName(e.kind) + QLatin1Char(' ') + r.networkError));
    QVERIFY(e.stopsSync());
    QVERIFY(mock_->requests().isEmpty());  // no HTTP request was sent in clear
#else
    QSKIP("Qt without TLS");
#endif
  }

  // A self-signed certificate is refused: there is no way to accept it.
  void selfSignedCertificateIsRefused() {
#if QT_CONFIG(ssl)
    if (!QSslSocket::supportsSsl()) QSKIP("no TLS backend");
    // A server-side test certificate needs the OpenSSL backend (Linux CI);
    // macOS's Secure Transport can't load a PEM key.
    if (QSslSocket::activeBackend() != QLatin1String("openssl")) QSKIP("not the OpenSSL TLS backend");
    const QString openssl = QStandardPaths::findExecutable(QStringLiteral("openssl"));
    if (openssl.isEmpty()) QSKIP("openssl not installed");
    QTemporaryDir dir;
    const QString key = dir.path() + QStringLiteral("/key.pem");
    const QString cert = dir.path() + QStringLiteral("/cert.pem");
    QProcess p;
    p.start(openssl, {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"), QStringLiteral("rsa:2048"),
                      QStringLiteral("-nodes"), QStringLiteral("-keyout"), key, QStringLiteral("-out"), cert,
                      QStringLiteral("-days"), QStringLiteral("1"), QStringLiteral("-subj"), QStringLiteral("/CN=localhost"),
                      QStringLiteral("-addext"), QStringLiteral("subjectAltName=DNS:localhost,IP:127.0.0.1")});
    if (!p.waitForFinished(30000) || p.exitCode() != 0) QSKIP("openssl could not make a test certificate");
    QFile kf(key);
    QFile cf(cert);
    QVERIFY(kf.open(QIODevice::ReadOnly) && cf.open(QIODevice::ReadOnly));
    const QSslKey sslKey(&kf, QSsl::Rsa);
    const QSslCertificate sslCert(&cf);
    if (sslKey.isNull() || sslCert.isNull()) QSKIP("this TLS backend cannot load PEM test keys");

    QSslServer server;
    QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
    conf.setLocalCertificate(sslCert);
    conf.setPrivateKey(sslKey);
    conf.setPeerVerifyMode(QSslSocket::VerifyNone);
    server.setSslConfiguration(conf);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    int encrypted = 0;
    connect(&server, &QSslServer::pendingConnectionAvailable, this, [&] {
      while (QTcpSocket* s = server.nextPendingConnection()) {
        connect(s, &QTcpSocket::readyRead, s, [s, &encrypted] {
          ++encrypted;  // only reached if the client accepted the certificate
          s->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}");
        });
      }
    });
    AsyncApiClient api(QUrl(QStringLiteral("https://localhost:%1").arg(server.serverPort())), kToken);
    api.setTimeoutMs(5000);
    const ApiResponse r = call(api, get(QStringLiteral("/v1/health")), 8000);
    const ApiError e = classifyResponse(r);
    QVERIFY2(e.kind == ErrorKind::Tls, qPrintable(errorKindName(e.kind) + QLatin1Char(' ') + r.networkError));
    QCOMPARE(encrypted, 0);
#else
    QSKIP("Qt without TLS");
#endif
  }
};

QTEST_GUILESS_MAIN(TestAsyncApi)
#include "tst_asyncapi.moc"
