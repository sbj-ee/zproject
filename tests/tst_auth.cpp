// Token format, hashing, scopes and rate limits (SPEC.md §9.2, §9.3).
#include <QTest>

#include "server/Auth.hpp"
#include "server/RateLimiter.hpp"

using namespace zproject::server;

class TestAuth : public QObject {
  Q_OBJECT
 private slots:
  void tokenRoundTrip() {
    const NewToken t = generateToken();
    QCOMPARE(t.secret.size(), 43);  // 256 bits, unpadded base64url
    QCOMPARE(t.secretHash.size(), 64);
    QVERIFY(t.text().startsWith(QStringLiteral("zpt_") + t.id + QLatin1Char('.')));
    const auto parsed = parseToken(t.text().toLatin1());
    QVERIFY(parsed);
    QCOMPARE(parsed->id, t.id);
    QCOMPARE(hashSecret(parsed->secret), t.secretHash);
    QVERIFY(generateToken().secret != t.secret);
    QVERIFY(parseBearer("Bearer " + t.text().toLatin1()));
    QVERIFY(parseBearer("bearer  " + t.text().toLatin1()));
  }

  void malformedTokens_data() {
    QTest::addColumn<QByteArray>("text");
    const QByteArray good = generateToken().text().toLatin1();
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("prefix") << ("zpx_" + good.mid(4));
    QTest::newRow("short secret") << good.left(good.size() - 1);
    QTest::newRow("long secret") << (good + "A");
    QTest::newRow("bad char") << (good.left(good.size() - 1) + "=");
    QTest::newRow("upper id") << ("zpt_" + good.mid(4, 20).toUpper() + good.mid(24));
    QTest::newRow("no dot") << QByteArray(good).replace('.', 'x');
  }
  void malformedTokens() {
    QFETCH(QByteArray, text);
    QVERIFY(!parseToken(text));
  }

  void constantTimeCompare() {
    QVERIFY(constantTimeEquals("abc", "abc"));
    QVERIFY(!constantTimeEquals("abc", "abd"));
    QVERIFY(!constantTimeEquals("abc", "ab"));
  }

  void scopes() {
    zproject::store::TokenRecord t;
    t.scopes = {QStringLiteral("read")};
    QVERIFY(hasScope(t, QStringLiteral("tasks:read")));
    QVERIFY(hasScope(t, QStringLiteral("projects:read")));
    QVERIFY(!hasScope(t, QStringLiteral("tasks:write")));
    QVERIFY(!hasScope(t, QStringLiteral("audit:read")));
    QVERIFY(!hasScope(t, QStringLiteral("admin")));
    t.scopes = {QStringLiteral("write")};
    QVERIFY(hasScope(t, QStringLiteral("tasks:write")));
    QVERIFY(hasScope(t, QStringLiteral("tasks:read")));
    QVERIFY(!hasScope(t, QStringLiteral("admin")));
    t.scopes = {QStringLiteral("admin")};
    QVERIFY(hasScope(t, QStringLiteral("audit:read")));
    QVERIFY(hasScope(t, QStringLiteral("tasks:write")));
    t.projects = {QStringLiteral("aaaaaaaaaaaaaaaaaaaa")};
    QVERIFY(projectAllowed(t, QStringLiteral("aaaaaaaaaaaaaaaaaaaa")));
    QVERIFY(!projectAllowed(t, QStringLiteral("bbbbbbbbbbbbbbbbbbbb")));
    t.projects = {QStringLiteral("*")};
    QVERIFY(projectAllowed(t, QStringLiteral("bbbbbbbbbbbbbbbbbbbb")));
  }

  void agentRateLimits() {
    qint64 now = 0;
    RateLimiter rl([&] { return now; });
    // 5 destructive per minute, and each also uses a write.
    for (int i = 0; i < 5; ++i) QCOMPARE(rl.take(QStringLiteral("a"), QStringLiteral("agent"), {}, RateLimiter::Kind::Destructive), 0);
    const int wait = rl.take(QStringLiteral("a"), QStringLiteral("agent"), {}, RateLimiter::Kind::Destructive);
    QVERIFY(wait >= 1 && wait <= 12);
    // Plain writes still have 25 left; reads are separate.
    for (int i = 0; i < 25; ++i) QCOMPARE(rl.take(QStringLiteral("a"), QStringLiteral("agent"), {}, RateLimiter::Kind::Write), 0);
    QVERIFY(rl.take(QStringLiteral("a"), QStringLiteral("agent"), {}, RateLimiter::Kind::Write) > 0);
    QCOMPARE(rl.take(QStringLiteral("a"), QStringLiteral("agent"), {}, RateLimiter::Kind::Read), 0);
    // Refill: after 12 s one destructive token is back.
    now += 12'000;
    QCOMPARE(rl.take(QStringLiteral("a"), QStringLiteral("agent"), {}, RateLimiter::Kind::Destructive), 0);
    // Another token is unaffected.
    QCOMPARE(rl.take(QStringLiteral("b"), QStringLiteral("agent"), {}, RateLimiter::Kind::Destructive), 0);
  }

  void overridesAndOwner() {
    qint64 now = 0;
    RateLimiter rl([&] { return now; });
    const QJsonObject over{{QStringLiteral("read"), 2}};
    QCOMPARE(rl.take(QStringLiteral("a"), QStringLiteral("agent"), over, RateLimiter::Kind::Read), 0);
    QCOMPARE(rl.take(QStringLiteral("a"), QStringLiteral("agent"), over, RateLimiter::Kind::Read), 0);
    QVERIFY(rl.take(QStringLiteral("a"), QStringLiteral("agent"), over, RateLimiter::Kind::Read) > 0);
    for (int i = 0; i < 1200; ++i) QCOMPARE(rl.take(QStringLiteral("o"), QStringLiteral("owner"), {}, RateLimiter::Kind::Read), 0);
    QVERIFY(rl.take(QStringLiteral("o"), QStringLiteral("owner"), {}, RateLimiter::Kind::Write) > 0);
  }

  void failedAuthPerSource() {
    qint64 now = 0;
    RateLimiter rl([&] { return now; });
    for (int i = 0; i < 10; ++i) QCOMPARE(rl.takeFailedAuth(QStringLiteral("192.0.2.9")), 0);
    QVERIFY(rl.takeFailedAuth(QStringLiteral("192.0.2.9")) > 0);
    QCOMPARE(rl.takeFailedAuth(QStringLiteral("192.0.2.10")), 0);
  }
};

QTEST_GUILESS_MAIN(TestAuth)
#include "tst_auth.moc"
