#include <QTest>

#include "server/ServerConfig.hpp"

using zproject::server::ListenAddress;
using zproject::server::parseListenAddress;

class TstServerConfig : public QObject {
  Q_OBJECT

 private slots:
  void defaults() {
    const ListenAddress a;
    QCOMPARE(a.host, QHostAddress(QHostAddress::LocalHost));
    QCOMPARE(a.port, quint16{8788});
  }

  void accepts_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("host");
    QTest::addColumn<int>("port");
    QTest::newRow("default") << QStringLiteral("127.0.0.1:8788") << QStringLiteral("127.0.0.1") << 8788;
    QTest::newRow("port only") << QStringLiteral("9000") << QStringLiteral("127.0.0.1") << 9000;
    QTest::newRow(":port") << QStringLiteral(":8443") << QStringLiteral("127.0.0.1") << 8443;
    QTest::newRow("localhost") << QStringLiteral("localhost:8788") << QStringLiteral("127.0.0.1") << 8788;
    QTest::newRow("ipv6 loopback") << QStringLiteral("[::1]:8788") << QStringLiteral("::1") << 8788;
  }
  void accepts() {
    QFETCH(QString, text);
    QFETCH(QString, host);
    QFETCH(int, port);
    ListenAddress a;
    QString error;
    QVERIFY2(parseListenAddress(text, &a, &error), qPrintable(error));
    QCOMPARE(a.host, QHostAddress(host));
    QCOMPARE(int{a.port}, port);
  }

  void rejects_data() {
    QTest::addColumn<QString>("text");
    QTest::newRow("all interfaces") << QStringLiteral("0.0.0.0:8788");
    QTest::newRow("lan") << QStringLiteral("192.168.1.10:8788");
    // A tailnet (CGNAT range) example address, not a real host.
    QTest::newRow("tailnet") << QStringLiteral("100.64.0.1:8788");
    QTest::newRow("ipv6 any") << QStringLiteral("[::]:8788");
    QTest::newRow("port 0") << QStringLiteral("127.0.0.1:0");
    QTest::newRow("port too big") << QStringLiteral("127.0.0.1:65536");
    QTest::newRow("no port") << QStringLiteral("127.0.0.1:");
    QTest::newRow("garbage") << QStringLiteral("not-an-address:80");
    QTest::newRow("empty") << QString();
  }
  void rejects() {
    QFETCH(QString, text);
    ListenAddress a;
    QString error;
    QVERIFY(!parseListenAddress(text, &a, &error));
    QVERIFY(!error.isEmpty());
  }
};

QTEST_GUILESS_MAIN(TstServerConfig)
#include "tst_serverconfig.moc"
