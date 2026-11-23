// Desktop client core without a network (SPEC.md §9.3, §16.5): error
// mapping, the health/API-version gate, change-feed decoding, the desktop's
// paths and settings, and reuse of zproject-cli's token store. Tokens are
// made up per test.
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

#include "client/ApiError.hpp"
#include "client/ChangeRecord.hpp"
#include "client/DesktopSettings.hpp"
#include "client/TokenStore.hpp"
#include "core/Version.hpp"

using namespace zproject;
using namespace zproject::client;

Q_DECLARE_METATYPE(zproject::client::ErrorKind)

namespace {

const QString kToken = QStringLiteral("zpt_0123456789abcdef0123.") + QString(43, QLatin1Char('A'));

ApiResponse response(int status, const QJsonObject& json, const QList<QPair<QByteArray, QByteArray>>& headers = {}) {
  ApiResponse r;
  r.status = status;
  r.json = json;
  r.body = QJsonDocument(json).toJson(QJsonDocument::Compact);
  r.headers = headers;
  return r;
}

QJsonObject err(const QString& code, const QJsonObject& details = {}) {
  return {{QStringLiteral("error"), QStringLiteral("server text")},
          {QStringLiteral("code"), code},
          {QStringLiteral("details"), details}};
}

int fileMode(const QString& path) {
  struct stat st {};
  if (::stat(QFile::encodeName(path).constData(), &st) != 0) return -1;
  return static_cast<int>(st.st_mode & 0777);
}

QJsonObject change(qint64 seq, const QString& id = QStringLiteral("t1")) {
  return {{QStringLiteral("seq"), seq},
          {QStringLiteral("ts"), QStringLiteral("2026-11-22T19:00:00.000Z")},
          {QStringLiteral("actor"), QStringLiteral("abc")},
          {QStringLiteral("entityKind"), QStringLiteral("task")},
          {QStringLiteral("entityId"), id},
          {QStringLiteral("before"), QJsonObject{}},
          {QStringLiteral("after"), QJsonObject{{QStringLiteral("title"), QStringLiteral("x")}}},
          {QStringLiteral("requestId"), QJsonValue()}};
}

}  // namespace

class TestClientCore : public QObject {
  Q_OBJECT

 private slots:
  void errorMapping_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("code");
    QTest::addColumn<ErrorKind>("kind");
    QTest::addColumn<bool>("offline");
    QTest::addColumn<bool>("stops");
    QTest::newRow("200") << 200 << QString() << ErrorKind::None << false << false;
    QTest::newRow("400") << 400 << "validation" << ErrorKind::Validation << false << false;
    QTest::newRow("401") << 401 << "unauthenticated" << ErrorKind::Unauthenticated << false << true;
    QTest::newRow("403 scope") << 403 << "forbidden_scope" << ErrorKind::ForbiddenScope << false << false;
    QTest::newRow("403 project") << 403 << "forbidden_project" << ErrorKind::ForbiddenProject << false << false;
    QTest::newRow("404") << 404 << "not_found" << ErrorKind::NotFound << false << false;
    QTest::newRow("409 conflict") << 409 << "conflict" << ErrorKind::Conflict << false << false;
    QTest::newRow("409 cycle") << 409 << "cycle" << ErrorKind::Conflict << false << false;
    QTest::newRow("409 confirm") << 409 << "confirm_required" << ErrorKind::ConfirmRequired << false << false;
    QTest::newRow("412") << 412 << "rev_mismatch" << ErrorKind::RevMismatch << false << false;
    QTest::newRow("413") << 413 << "too_large" << ErrorKind::TooLarge << false << false;
    QTest::newRow("422") << 422 << "schedule_violation" << ErrorKind::Unprocessable << false << false;
    QTest::newRow("429") << 429 << "rate_limited" << ErrorKind::RateLimited << false << false;
    QTest::newRow("500") << 500 << "internal" << ErrorKind::Server << true << false;
    QTest::newRow("503") << 503 << QString() << ErrorKind::Server << true << false;
  }
  void errorMapping() {
    QFETCH(int, status);
    QFETCH(QString, code);
    QFETCH(ErrorKind, kind);
    QFETCH(bool, offline);
    QFETCH(bool, stops);
    const ApiError e = classifyResponse(response(status, status < 300 ? QJsonObject{{QStringLiteral("ok"), true}}
                                                                       : err(code)));
    QCOMPARE(errorKindName(e.kind), errorKindName(kind));
    QCOMPARE(e.goesOffline(), offline);
    QCOMPARE(e.stopsSync(), stops);
    QCOMPARE(e.ok(), kind == ErrorKind::None);
    QCOMPARE(e.status, status);
    if (!e.ok()) QVERIFY(!e.message.isEmpty());
  }

  void errorDetails() {
    // Validation names the field.
    ApiError e = classifyResponse(response(
        400, err(QStringLiteral("validation"),
                 QJsonObject{{QStringLiteral("fields"),
                              QJsonArray{QJsonObject{{QStringLiteral("field"), QStringLiteral("duration")},
                                                     {QStringLiteral("message"), QStringLiteral("must be >= 0")}}}}})));
    QCOMPARE(e.kind, ErrorKind::Validation);
    QCOMPARE(e.field, QStringLiteral("duration"));
    QCOMPARE(e.message, QStringLiteral("server text"));
    // The summary roll-up refusal of 0.5.0 gets its own message.
    e = classifyResponse(response(
        400, err(QStringLiteral("validation"),
                 QJsonObject{{QStringLiteral("fields"),
                              QJsonArray{QJsonObject{
                                  {QStringLiteral("field"), QStringLiteral("percentComplete")},
                                  {QStringLiteral("message"),
                                   QStringLiteral("is derived from the child tasks of a summary task and cannot be set")}}}}})));
    QCOMPARE(e.kind, ErrorKind::SummaryRollup);
    QCOMPARE(e.field, QStringLiteral("percentComplete"));
    QVERIFY(e.message.contains(QLatin1String("rolled up")));
    // 403 names the missing scope; 429 carries Retry-After (default 60).
    e = classifyResponse(response(403, err(QStringLiteral("forbidden_scope"),
                                           QJsonObject{{QStringLiteral("required"), QStringLiteral("tasks:write")}})));
    QVERIFY(e.message.contains(QLatin1String("tasks:write")));
    e = classifyResponse(response(429, err(QStringLiteral("rate_limited")), {{"Retry-After", "7"}, {"X-Request-Id", "r1"}}));
    QCOMPARE(e.retryAfterSeconds, 7);
    QCOMPARE(e.requestId, QStringLiteral("r1"));
    QCOMPARE(classifyResponse(response(429, err(QStringLiteral("rate_limited")))).retryAfterSeconds, 60);
    // Transport failures.
    ApiResponse r;
    r.networkError = QStringLiteral("cannot reach");
    QCOMPARE(classifyResponse(r).kind, ErrorKind::Network);
    r.timedOut = true;
    QCOMPARE(classifyResponse(r).kind, ErrorKind::Timeout);
    r.timedOut = false;
    r.tlsFailure = true;
    e = classifyResponse(r);
    QCOMPARE(e.kind, ErrorKind::Tls);
    QVERIFY(e.stopsSync());
    QVERIFY(!e.goesOffline());
    // 2xx with something that isn't a JSON object.
    ApiResponse bad;
    bad.status = 200;
    bad.body = "{\"items\": [";
    QCOMPARE(classifyResponse(bad).kind, ErrorKind::BadResponse);
    bad.body = "<html>proxy</html>";
    QCOMPARE(classifyResponse(bad).kind, ErrorKind::BadResponse);
    bad.body.clear();  // 204-style empty body is fine
    QCOMPARE(classifyResponse(bad).kind, ErrorKind::None);
  }

  void healthAndVersionGate() {
    auto h = parseHealth(QJsonObject{{QStringLiteral("ok"), true},
                                     {QStringLiteral("version"), QStringLiteral("0.5.0")},
                                     {QStringLiteral("apiVersion"), 1},
                                     {QStringLiteral("schemaVersion"), 2}});
    QVERIFY(h.ok());
    QCOMPARE(h->schemaVersion, 2);
    QVERIFY(!parseHealth(QJsonObject{{QStringLiteral("ok"), true}}).ok());
    QVERIFY(!parseHealth(QJsonObject{}).ok());

    ServerHealth s{QStringLiteral("0.6.0"), 1, 2};
    QCOMPARE(checkCompatibility(s, QStringLiteral("0.6.0"), 1).level, Compatibility::Same);
    QVERIFY(checkCompatibility(s, QStringLiteral("0.6.0"), 1).message.isEmpty());
    s.version = QStringLiteral("0.5.0");
    Compatibility c = checkCompatibility(s, QStringLiteral("0.6.0"), 1);
    QCOMPARE(c.level, Compatibility::ServerOlder);
    QVERIFY(c.usable());
    QVERIFY(c.message.contains(QLatin1String("older")));
    s.version = QStringLiteral("0.10.0");  // numeric, not string, comparison
    QCOMPARE(checkCompatibility(s, QStringLiteral("0.6.0"), 1).level, Compatibility::ServerNewer);
    s.apiVersion = 2;
    c = checkCompatibility(s, QStringLiteral("0.6.0"), 1);
    QCOMPARE(c.level, Compatibility::Unsupported);
    QVERIFY(!c.usable());
    QVERIFY(c.message.contains(QLatin1String("API v2")));
  }

  void changeDecoding() {
    auto c = decodeChangeRecord(change(5));
    QVERIFY(c.ok());
    QCOMPARE(c->seq, qint64(5));
    QCOMPARE(c->after.value(QStringLiteral("title")).toString(), QStringLiteral("x"));
    QVERIFY(c->requestId.isEmpty());

    QJsonObject bad = change(0);
    QVERIFY(!decodeChangeRecord(bad).ok());
    bad = change(1);
    bad.insert(QStringLiteral("seq"), 1.5);
    QVERIFY(!decodeChangeRecord(bad).ok());
    bad = change(1);
    bad.insert(QStringLiteral("seq"), QStringLiteral("1"));
    QVERIFY(!decodeChangeRecord(bad).ok());
    bad = change(1);
    bad.remove(QStringLiteral("entityId"));
    QVERIFY(!decodeChangeRecord(bad).ok());
    bad = change(1);
    bad.insert(QStringLiteral("after"), QJsonArray{});
    QVERIFY(!decodeChangeRecord(bad).ok());
    bad = change(1);
    bad.insert(QStringLiteral("after"), QJsonObject{{QStringLiteral("id"), QStringLiteral("other")}});
    QVERIFY(!decodeChangeRecord(bad).ok());

    auto page = decodeChangePage(QJsonObject{{QStringLiteral("items"), QJsonArray{change(3), change(4)}},
                                             {QStringLiteral("nextSince"), 4},
                                             {QStringLiteral("more"), true}});
    QVERIFY(page.ok());
    QCOMPARE(page->items.size(), 2);
    QVERIFY(page->more);
    // Empty page: nextSince echoes since.
    QVERIFY(decodeChangePage(QJsonObject{{QStringLiteral("items"), QJsonArray{}},
                                         {QStringLiteral("nextSince"), 9},
                                         {QStringLiteral("more"), false}})
                .ok());
    // Not increasing, wrong nextSince, missing fields: the page is refused.
    QVERIFY(!decodeChangePage(QJsonObject{{QStringLiteral("items"), QJsonArray{change(4), change(3)}},
                                          {QStringLiteral("nextSince"), 3},
                                          {QStringLiteral("more"), false}})
                 .ok());
    QVERIFY(!decodeChangePage(QJsonObject{{QStringLiteral("items"), QJsonArray{change(3)}},
                                          {QStringLiteral("nextSince"), 7},
                                          {QStringLiteral("more"), false}})
                 .ok());
    QVERIFY(!decodeChangePage(QJsonObject{{QStringLiteral("items"), QJsonArray{change(3)}}}).ok());
    QVERIFY(!decodeChangePage(QJsonObject{}).ok());
  }

  void desktopPaths_() {
    QHash<QString, QString> env{{QStringLiteral("HOME"), QStringLiteral("/home/u")}};
    DesktopPaths p = desktopPaths(env, false);
    QCOMPARE(p.settingsFile, QStringLiteral("/home/u/.config/zproject/zproject.ini"));
    QCOMPARE(p.cacheFile(), QStringLiteral("/home/u/.cache/zproject/cache.db"));
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), QStringLiteral("/x/conf"));
    env.insert(QStringLiteral("XDG_CACHE_HOME"), QStringLiteral("/x/cache"));
    p = desktopPaths(env, false);
    QCOMPARE(p.settingsFile, QStringLiteral("/x/conf/zproject/zproject.ini"));
    QCOMPARE(p.cacheDir, QStringLiteral("/x/cache/zproject"));
    p = desktopPaths(env, true);
    QCOMPARE(p.settingsFile, QStringLiteral("/home/u/Library/Preferences/zproject/zproject.ini"));
    QCOMPARE(p.cacheDir, QStringLiteral("/home/u/Library/Caches/zproject"));
    env.insert(QStringLiteral("ZPROJECT_CACHE_DIR"), QStringLiteral("/t/c"));
    QCOMPARE(desktopPaths(env, true).cacheDir, QStringLiteral("/t/c"));
  }

  void settingsRoundTrip() {
    QTemporaryDir dir;
    const QHash<QString, QString> env{{QStringLiteral("HOME"), dir.path()},
                                      {QStringLiteral("ZPROJECT_CONFIG_DIR"), dir.path() + QStringLiteral("/cfg")}};
    const DesktopPaths p = desktopPaths(env, false);
    DesktopSettings s = loadDesktopSettings(p);
    QVERIFY(s.serverUrl.isEmpty());
    QVERIFY(s.cacheEnabled);
    s.serverUrl = QStringLiteral("https://server-host.example.ts.net");
    s.cacheEnabled = false;
    QVERIFY(saveDesktopSettings(p, s).ok());
    QCOMPARE(fileMode(p.settingsFile), 0600);
    QCOMPARE(fileMode(dir.path() + QStringLiteral("/cfg")), 0700);
    const DesktopSettings back = loadDesktopSettings(p);
    QCOMPARE(back.serverUrl, s.serverUrl);
    QVERIFY(!back.cacheEnabled);
    QVERIFY(back.showToolbar);
    s.showToolbar = false;
    QVERIFY(saveDesktopSettings(p, s).ok());
    QVERIFY(!loadDesktopSettings(p).showToolbar);
    QFile f(p.settingsFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(!f.readAll().contains("zpt_"));
  }

  // The app finds what `zproject-cli login` stored: cli.ini's URL and store,
  // and the same token file (or Keychain entry).
  void reusesTheCliToken() {
    QTemporaryDir dir;
    const QString cfg = dir.path() + QStringLiteral("/cfg");
    QHash<QString, QString> env{{QStringLiteral("HOME"), dir.path()}, {QStringLiteral("ZPROJECT_CONFIG_DIR"), cfg}};
    ClientConfig cli = loadClientConfig(env, QString());
    cli.url = QStringLiteral("https://server-host.example.ts.net");
    cli.store = QStringLiteral("file");
    QVERIFY(saveClientConfig(cli).ok());
    const CommandRunner noTools = [](const QString&, const QStringList&, const QByteArray&) { return CommandResult{}; };
    TokenStore store(cli.tokenFile, noTools, false);

    cli = loadClientConfig(env, QString());
    const DesktopPaths paths = desktopPaths(env, false);
    auto c = resolveConnection(env, loadDesktopSettings(paths), cli, store);
    QVERIFY(!c.ok());  // no token yet
    QCOMPARE(c.error().code, QStringLiteral("auth"));
    QVERIFY(c.error().message.contains(QLatin1String("zproject-cli login")));

    QVERIFY(store.writeFile(kToken).ok());
    c = resolveConnection(env, loadDesktopSettings(paths), cli, store);
    QVERIFY2(c.ok(), qPrintable(c.ok() ? QString() : c.error().message));
    QCOMPARE(c->url.toString(), QStringLiteral("https://server-host.example.ts.net"));
    QCOMPARE(c->urlSource, QStringLiteral("cli.ini"));
    QCOMPARE(c->token, kToken);
    QCOMPARE(c->tokenLabel, QStringLiteral("zpt_0123456789abcdef0123"));
    QCOMPARE(c->tokenSource, QStringLiteral("file ") + cli.tokenFile);
    QVERIFY(!c->tokenLabel.contains(QLatin1Char('.')));

    // The app's own URL wins over cli.ini; ZPROJECT_URL over both.
    DesktopSettings s;
    s.serverUrl = QStringLiteral("https://other-host.example.ts.net");
    QCOMPARE(resolveConnection(env, s, cli, store)->urlSource, QStringLiteral("zproject.ini"));
    env.insert(QStringLiteral("ZPROJECT_URL"), QStringLiteral("http://127.0.0.1:9"));
    QCOMPARE(resolveConnection(env, s, cli, store)->urlSource, QStringLiteral("ZPROJECT_URL"));
    env.remove(QStringLiteral("ZPROJECT_URL"));
    // Clear text off-host is refused, as for the CLI.
    s.serverUrl = QStringLiteral("http://server-host.example.ts.net");
    QCOMPARE(resolveConnection(env, s, cli, store).error().code, QStringLiteral("validation"));

    // A token file others can read is refused, not used.
    QVERIFY(::chmod(QFile::encodeName(cli.tokenFile).constData(), 0644) == 0);
    c = resolveConnection(env, DesktopSettings{}, cli, store);
    QVERIFY(!c.ok());
    QVERIFY(c.error().message.contains(QLatin1String("chmod 600")));
  }

  void reusesTheKeychainToken() {
    QString secret = kToken;
    QList<QStringList> argvs;
    const CommandRunner fake = [&](const QString&, const QStringList& args, const QByteArray&) {
      argvs.append(args);
      CommandResult r;
      r.exitCode = 0;
      r.out = secret.toLatin1() + '\n';
      return r;
    };
    TokenStore store(QStringLiteral("/nonexistent/token"), fake, true);
    auto t = resolveDeviceToken({}, QStringLiteral("keychain"), store);
    QVERIFY(t.ok());
    QCOMPARE(t->token, kToken);
    QCOMPARE(t->source, QStringLiteral("keychain"));
    for (const QStringList& a : argvs) QVERIFY(!a.join(QLatin1Char(' ')).contains(kToken));  // never in argv
    // macOS with no store chosen: the file first, then the Keychain.
    t = resolveDeviceToken({}, QString(), store);
    QVERIFY(t.ok());
    QCOMPARE(t->source, QStringLiteral("keychain"));
    // ZPROJECT_TOKEN wins, and must look like a token.
    QCOMPARE(resolveDeviceToken({{QStringLiteral("ZPROJECT_TOKEN"), kToken}}, QString(), store)->source,
             QStringLiteral("ZPROJECT_TOKEN"));
    QVERIFY(!resolveDeviceToken({{QStringLiteral("ZPROJECT_TOKEN"), QStringLiteral("nope")}}, QString(), store).ok());
  }
};

QTEST_GUILESS_MAIN(TestClientCore)
#include "tst_clientcore.moc"
