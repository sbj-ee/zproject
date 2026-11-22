// The change-feed syncer (SPEC.md §16.5). End to end against a real zprojectd
// /v1 on a loopback port (initial load, updates with summary roll-ups,
// paging, a restored server), and against the fault-injecting mock server
// (offline backoff and reconnect, 401, API-version refusal, 429, a
// project-limited token's list polling, a schema change, cadence). Tokens
// are generated per run.
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>

#include "MockServer.hpp"
#include "client/AsyncApiClient.hpp"
#include "client/Cache.hpp"
#include "client/ChangeFeedSyncer.hpp"
#include "server/Api.hpp"
#include "server/http/HttpServer.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::client;
using zproject::test::MockReply;
using zproject::test::MockRequest;
using zproject::test::MockServer;
using State = ChangeFeedSyncer::State;

Q_DECLARE_METATYPE(zproject::client::ChangeFeedSyncer::State)

namespace {

const store::WriteContext kCtx{QStringLiteral("test"), QString()};
const QByteArray kFakeToken = "zpt_0123456789abcdef0123." + QByteArray(43, 'A');

ChangeFeedSyncer::Timing fast() {
  ChangeFeedSyncer::Timing t;
  t.activeMs = 60'000;  // tests drive cycles with refreshNow() unless they test cadence
  t.inactiveMs = 120'000;
  t.listPollMs = 90'000;
  t.backoffMinMs = 50;
  t.backoffMaxMs = 200;
  return t;
}

// A scripted server: a fixed feed plus counters.
struct FakeServer {
  QJsonArray feed;
  QString version = QStringLiteral("0.6.0");
  int apiVersion = 1;
  int schemaVersion = 2;
  int failHealth = 0;       // answer 503 this many times
  int status = 0;           // when set, every request gets this error
  QString errorCode;
  bool limitedToken = false;  // /v1/changes → 403 forbidden_project
  QHash<QString, int> hits;

  static QJsonObject rec(qint64 seq, const QString& kind, const QString& id, const QJsonObject& before,
                         const QJsonObject& after) {
    return {{QStringLiteral("seq"), seq},       {QStringLiteral("ts"), QStringLiteral("t%1").arg(seq)},
            {QStringLiteral("actor"), QStringLiteral("a")}, {QStringLiteral("entityKind"), kind},
            {QStringLiteral("entityId"), id},   {QStringLiteral("before"), before},
            {QStringLiteral("after"), after},   {QStringLiteral("requestId"), QJsonValue()}};
  }
  void addProject(const QString& id) {
    feed.append(rec(feed.size() + 1, QStringLiteral("project"), id, {},
                    {{QStringLiteral("id"), id}, {QStringLiteral("name"), id}, {QStringLiteral("rev"), 1},
                     {QStringLiteral("deleted"), false}, {QStringLiteral("order"), 1}}));
  }
  void addTask(const QString& id, const QString& project) {
    feed.append(rec(feed.size() + 1, QStringLiteral("task"), id, {},
                    {{QStringLiteral("id"), id}, {QStringLiteral("project"), project}, {QStringLiteral("title"), id},
                     {QStringLiteral("rev"), 1}, {QStringLiteral("deleted"), false}, {QStringLiteral("order"), 1}}));
  }

  MockReply handle(const MockRequest& r) {
    ++hits[r.path];
    if (status != 0) return MockReply::error(status, errorCode, QStringLiteral("scripted"));
    if (r.path == QLatin1String("/v1/health")) {
      if (failHealth > 0) {
        --failHealth;
        return MockReply::error(503, QStringLiteral("internal"), QStringLiteral("down"));
      }
      return MockReply::json(200, {{QStringLiteral("ok"), true},
                                   {QStringLiteral("version"), version},
                                   {QStringLiteral("apiVersion"), apiVersion},
                                   {QStringLiteral("schemaVersion"), schemaVersion}});
    }
    if (r.path == QLatin1String("/v1/changes")) {
      if (limitedToken) {
        return MockReply::error(403, QStringLiteral("forbidden_project"), QStringLiteral("limited"));
      }
      const qint64 since = r.query.queryItemValue(QStringLiteral("since")).toLongLong();
      const int limit = r.query.queryItemValue(QStringLiteral("limit")).toInt();
      QJsonArray items;
      qint64 last = since;
      bool more = false;
      for (const QJsonValue& v : std::as_const(feed)) {
        const qint64 seq = v.toObject().value(QStringLiteral("seq")).toInteger();
        if (seq <= since) continue;
        if (items.size() == limit) {
          more = true;
          break;
        }
        items.append(v);
        last = seq;
      }
      return MockReply::json(200, {{QStringLiteral("items"), items},
                                   {QStringLiteral("nextSince"), last},
                                   {QStringLiteral("more"), more}});
    }
    if (r.path == QLatin1String("/v1/projects")) {
      return MockReply::json(200, {{QStringLiteral("items"),
                                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("P")},
                                                           {QStringLiteral("name"), QStringLiteral("Limited")},
                                                           {QStringLiteral("rev"), 4}}}},
                                   {QStringLiteral("nextCursor"), QJsonValue()}});
    }
    if (r.path == QLatin1String("/v1/projects/P/tasks")) {
      // Two pages.
      if (r.query.queryItemValue(QStringLiteral("cursor")).isEmpty()) {
        return MockReply::json(200, {{QStringLiteral("items"),
                                      QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("a")},
                                                             {QStringLiteral("project"), QStringLiteral("P")}}}},
                                     {QStringLiteral("nextCursor"), QStringLiteral("page2")}});
      }
      return MockReply::json(200, {{QStringLiteral("items"),
                                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("b")},
                                                           {QStringLiteral("project"), QStringLiteral("P")}}}},
                                   {QStringLiteral("nextCursor"), QJsonValue()}});
    }
    if (r.path.endsWith(QLatin1String("/wbs"))) {
      return MockReply::json(200, {{QStringLiteral("items"), QJsonArray{}}});
    }
    return MockReply::error(404, QStringLiteral("not_found"), QStringLiteral("no route"));
  }
};

bool waitSynced(QSignalSpy& spy, int count = 1, int ms = 5000) {
  return QTest::qWaitFor([&] { return spy.count() >= count; }, ms);
}

}  // namespace

class TestSyncer : public QObject {
  Q_OBJECT

  // Real server.
  std::unique_ptr<store::SqliteStore> store_;
  std::unique_ptr<server::Api> api_;
  std::unique_ptr<server::http::Server> http_;
  QString owner_;
  QUrl url_;

  void startRealServer() {
    store_ = std::move(store::SqliteStore::open(QStringLiteral(":memory:"))).value();
    if (!http_) {
      http_ = std::make_unique<server::http::Server>([this](const server::http::Request& r) { return api_->handle(r); });
      QVERIFY(http_->listen(QHostAddress::LocalHost, 0));
    }
    server::ApiOptions o;
    o.allowedHosts = {QStringLiteral("127.0.0.1:%1").arg(http_->port())};
    api_ = std::make_unique<server::Api>(*store_, o);
    owner_ = api_->createOwnerToken(QStringLiteral("owner")).value();
    url_ = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(http_->port()));
  }
  Project makeProject(const QString& name) {
    Project p;
    p.name = name;
    return std::move(store_->createProject(p, kCtx)).value();
  }
  Task makeTask(const QString& project, const QString& title, const QString& parent = {}) {
    Task t;
    t.project = project;
    t.title = title;
    t.parent = parent;
    auto r = store_->createTask(t, kCtx);
    if (!r) qFatal("createTask: %s", qPrintable(r.error().message));
    return std::move(r).value();
  }

 private slots:
  void init() { qRegisterMetaType<State>(); }
  void cleanup() {
    http_.reset();
    api_.reset();
    store_.reset();
  }

  void endToEndWithRealServer() {
    startRealServer();
    const Project p = makeProject(QStringLiteral("Plan"));
    const Task summary = makeTask(p.id, QStringLiteral("Phase 1"));
    const Task leaf = makeTask(p.id, QStringLiteral("Design"), summary.id);
    makeTask(p.id, QStringLiteral("Build"), summary.id);

    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    AsyncApiClient client(url_, owner_.toLatin1());
    ChangeFeedSyncer sync(&client, cache.get());
    sync.setTiming(fast());
    QSignalSpy synced(&sync, &ChangeFeedSyncer::synced);
    QSignalSpy checked(&sync, &ChangeFeedSyncer::serverChecked);
    sync.start();
    QVERIFY(waitSynced(synced));
    QCOMPARE(sync.state(), State::Idle);
    QCOMPARE(checked.count(), 1);
    QVERIFY(sync.server().has_value());
    QCOMPARE(sync.compatibility().level, Compatibility::Same);
    QCOMPARE(synced.first().at(0).value<QSet<QString>>(), QSet<QString>{p.id});
    QCOMPARE(cache->projects().size(), 1);
    QCOMPARE(cache->tasks(p.id).size(), 3);
    QCOMPARE(cache->wbs(p.id).size(), 3);
    QCOMPARE(cache->lastSeq(), store_->changesSince(0, 500)->last().seq);
    QVERIFY(cache->refreshedAt().isValid());

    // A child goes to 50 %: the server rolls the summary up, and the cache
    // gets both records.
    Task t = *store_->task(leaf.id);
    t.status = QStringLiteral("in_progress");
    t.percentComplete = 50;
    t.actualStart = QStringLiteral("2026-11-23");
    QVERIFY(store_->updateTask(t, t.rev, kCtx).ok());
    sync.refreshNow();
    QVERIFY(waitSynced(synced, 2));
    QCOMPARE(cache->task(leaf.id)->value(QStringLiteral("percentComplete")).toInt(), 50);
    QCOMPARE(cache->task(summary.id)->value(QStringLiteral("percentComplete")).toInt(), 25);
    QCOMPARE(cache->task(summary.id)->value(QStringLiteral("status")).toString(), QStringLiteral("in_progress"));
    QCOMPARE(cache->activity(p.id, 2).size(), 2);
    QVERIFY(!synced.last().at(1).toBool());  // not a full reload

    // Nothing in the cache is the token.
    for (const QJsonObject& o : cache->tasks(p.id)) QVERIFY(!o.contains(QStringLiteral("token")));
  }

  void pagesThroughTheFeed() {
    startRealServer();
    const Project p = makeProject(QStringLiteral("Big"));
    for (int i = 0; i < 25; ++i) makeTask(p.id, QStringLiteral("T%1").arg(i));
    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    AsyncApiClient client(url_, owner_.toLatin1());
    ChangeFeedSyncer sync(&client, cache.get());
    ChangeFeedSyncer::Timing t = fast();
    t.pageLimit = 4;
    sync.setTiming(t);
    QSignalSpy synced(&sync, &ChangeFeedSyncer::synced);
    sync.start();
    QVERIFY(waitSynced(synced));
    QCOMPARE(cache->tasks(p.id).size(), 25);
    QCOMPARE(cache->lastSeq(), qint64(26));
  }

  // The server is restored from an older backup (or replaced): the record at
  // the cached position is gone or different, so the cache is rebuilt.
  void detectsARestoredServer() {
    startRealServer();
    const Project before = makeProject(QStringLiteral("Before restore"));
    makeTask(before.id, QStringLiteral("Lost"));
    makeTask(before.id, QStringLiteral("Lost too"));
    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    {
      AsyncApiClient client(url_, owner_.toLatin1());
      ChangeFeedSyncer sync(&client, cache.get());
      sync.setTiming(fast());
      QSignalSpy synced(&sync, &ChangeFeedSyncer::synced);
      sync.start();
      QVERIFY(waitSynced(synced));
    }
    QCOMPARE(cache->projects().size(), 1);

    startRealServer();  // a different database behind the same URL
    const Project after = makeProject(QStringLiteral("After restore"));
    AsyncApiClient client(url_, owner_.toLatin1());
    ChangeFeedSyncer sync(&client, cache.get());
    sync.setTiming(fast());
    QSignalSpy synced(&sync, &ChangeFeedSyncer::synced);
    sync.start();
    QVERIFY(waitSynced(synced));
    QVERIFY(synced.first().at(1).toBool());  // full reload
    QCOMPARE(cache->projects().size(), 1);
    QCOMPARE(cache->projects().first().value(QStringLiteral("id")).toString(), after.id);
    QVERIFY(!cache->project(before.id));
  }

  void offlineBackoffAndReconnect() {
    MockServer mock;
    QVERIFY(mock.listen());
    FakeServer fake;
    fake.addProject(QStringLiteral("P"));
    fake.failHealth = 3;
    mock.setHandler([&](const MockRequest& r) { return fake.handle(r); });
    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    AsyncApiClient client(mock.url(), kFakeToken);
    ChangeFeedSyncer sync(&client, cache.get());
    sync.setTiming(fast());
    QSignalSpy states(&sync, &ChangeFeedSyncer::stateChanged);
    QSignalSpy failed(&sync, &ChangeFeedSyncer::failed);
    QSignalSpy synced(&sync, &ChangeFeedSyncer::synced);
    sync.start();
    QVERIFY(QTest::qWaitFor([&] { return sync.state() == State::Offline; }, 2000));
    QVERIFY(sync.nextPollInMs() <= 200);
    QVERIFY(sync.lastError().goesOffline());
    QVERIFY(waitSynced(synced, 1, 5000));  // 50 + 100 + 200 ms later
    QCOMPARE(failed.count(), 3);
    QCOMPARE(fake.hits.value(QStringLiteral("/v1/health")), 4);
    QCOMPARE(sync.state(), State::Idle);
    QVERIFY(sync.lastError().ok());
    QCOMPARE(cache->projects().size(), 1);
    QVERIFY(states.count() >= 4);
    // Normal cadence again.
    QVERIFY(sync.nextPollInMs() > 50'000);

    // The server goes away mid-run: offline, then back with a health check.
    fake.status = 502;
    sync.refreshNow();
    QVERIFY(QTest::qWaitFor([&] { return sync.state() == State::Offline; }, 2000));
    fake.status = 0;
    const int health = fake.hits.value(QStringLiteral("/v1/health"));
    QVERIFY(waitSynced(synced, 2, 5000));
    QCOMPARE(fake.hits.value(QStringLiteral("/v1/health")), health + 1);
  }

  void unauthorizedHalts() {
    MockServer mock;
    QVERIFY(mock.listen());
    FakeServer fake;
    fake.status = 401;
    fake.errorCode = QStringLiteral("unauthenticated");
    mock.setHandler([&](const MockRequest& r) { return fake.handle(r); });
    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    AsyncApiClient client(mock.url(), kFakeToken);
    ChangeFeedSyncer sync(&client, cache.get());
    sync.setTiming(fast());
    QSignalSpy failed(&sync, &ChangeFeedSyncer::failed);
    sync.start();
    QVERIFY(QTest::qWaitFor([&] { return sync.state() == State::Halted; }, 2000));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(sync.lastError().kind, ErrorKind::Unauthenticated);
    QCOMPARE(sync.nextPollInMs(), -1);
    QTest::qWait(300);
    QCOMPARE(mock.requests().size(), 1);  // no polling with a refused token
  }

  void refusesAnotherApiVersion() {
    MockServer mock;
    QVERIFY(mock.listen());
    FakeServer fake;
    fake.apiVersion = 2;
    fake.version = QStringLiteral("2.0.0");
    mock.setHandler([&](const MockRequest& r) { return fake.handle(r); });
    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    AsyncApiClient client(mock.url(), kFakeToken);
    ChangeFeedSyncer sync(&client, cache.get());
    sync.setTiming(fast());
    QSignalSpy failed(&sync, &ChangeFeedSyncer::failed);
    sync.start();
    QVERIFY(QTest::qWaitFor([&] { return sync.state() == State::Halted; }, 2000));
    QCOMPARE(sync.compatibility().level, Compatibility::Unsupported);
    QCOMPARE(sync.lastError().code, QStringLiteral("api_version"));
    QVERIFY(sync.lastError().message.contains(QLatin1String("API v2")));
    QCOMPARE(fake.hits.value(QStringLiteral("/v1/changes")), 0);
    QVERIFY(cache->isEmpty());
  }

  void rateLimitedWaitsRetryAfter() {
    MockServer mock;
    QVERIFY(mock.listen());
    FakeServer fake;
    mock.setHandler([&](const MockRequest& r) {
      if (r.path == QLatin1String("/v1/changes")) {
        MockReply m = MockReply::error(429, QStringLiteral("rate_limited"), QStringLiteral("slow down"));
        m.headers.append({QByteArray("Retry-After"), QByteArray("3")});
        return m;
      }
      return fake.handle(r);
    });
    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    AsyncApiClient client(mock.url(), kFakeToken);
    ChangeFeedSyncer sync(&client, cache.get());
    sync.setTiming(fast());
    QSignalSpy failed(&sync, &ChangeFeedSyncer::failed);
    sync.start();
    QVERIFY(QTest::qWaitFor([&] { return failed.count() == 1; }, 2000));
    QCOMPARE(sync.lastError().kind, ErrorKind::RateLimited);
    QCOMPARE(sync.state(), State::Idle);  // not offline
    QVERIFY(sync.nextPollInMs() > 2000 && sync.nextPollInMs() <= 3000);
  }

  // A token limited to named projects can't read the feed: list polling.
  void projectLimitedTokenPollsLists() {
    MockServer mock;
    QVERIFY(mock.listen());
    FakeServer fake;
    fake.limitedToken = true;
    mock.setHandler([&](const MockRequest& r) { return fake.handle(r); });
    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    AsyncApiClient client(mock.url(), kFakeToken);
    ChangeFeedSyncer sync(&client, cache.get());
    sync.setTiming(fast());
    QSignalSpy synced(&sync, &ChangeFeedSyncer::synced);
    sync.start();
    QVERIFY(waitSynced(synced));
    QCOMPARE(sync.mode(), ChangeFeedSyncer::Mode::ListPoll);
    QCOMPARE(cache->projects().size(), 1);
    QCOMPARE(cache->tasks(QStringLiteral("P")).size(), 2);  // both pages
    QCOMPARE(fake.hits.value(QStringLiteral("/v1/projects/P/wbs")), 1);
    QVERIFY(sync.nextPollInMs() > 60'000 && sync.nextPollInMs() <= 90'000);
    sync.refreshNow();  // list mode stays
    QVERIFY(waitSynced(synced, 2));
    QCOMPARE(fake.hits.value(QStringLiteral("/v1/changes")), 1);
  }

  void schemaChangeReloads() {
    MockServer mock;
    QVERIFY(mock.listen());
    FakeServer fake;
    fake.addProject(QStringLiteral("P"));
    fake.addTask(QStringLiteral("a"), QStringLiteral("P"));
    mock.setHandler([&](const MockRequest& r) { return fake.handle(r); });
    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    AsyncApiClient client(mock.url(), kFakeToken);
    ChangeFeedSyncer sync(&client, cache.get());
    sync.setTiming(fast());
    QSignalSpy synced(&sync, &ChangeFeedSyncer::synced);
    sync.start();
    QVERIFY(waitSynced(synced));
    QVERIFY(!synced.last().at(1).toBool());
    QCOMPARE(cache->schemaVersion(), 2);
    fake.schemaVersion = 3;
    sync.refreshNow();
    QVERIFY(waitSynced(synced, 2));
    QVERIFY(synced.last().at(1).toBool());
    QCOMPARE(cache->schemaVersion(), 3);
    QCOMPARE(cache->tasks(QStringLiteral("P")).size(), 1);
  }

  // An update to an entity the cache never saw (a cache from before a
  // server-side purge, say) triggers one full reload.
  void incompleteCacheReloads() {
    MockServer mock;
    QVERIFY(mock.listen());
    FakeServer fake;
    fake.addProject(QStringLiteral("P"));
    mock.setHandler([&](const MockRequest& r) { return fake.handle(r); });
    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    // The cache believes it's at seq 1 but has nothing.
    ChangeRecord c;
    c.seq = 1;
    c.ts = QStringLiteral("t1");
    c.entityKind = QStringLiteral("token");
    c.entityId = QStringLiteral("x");
    QVERIFY(cache->apply({c}).ok());
    fake.feed.append(FakeServer::rec(2, QStringLiteral("project"), QStringLiteral("P"),
                                     {{QStringLiteral("name"), QStringLiteral("P")}},
                                     {{QStringLiteral("name"), QStringLiteral("Renamed")}, {QStringLiteral("rev"), 2}}));
    AsyncApiClient client(mock.url(), kFakeToken);
    ChangeFeedSyncer sync(&client, cache.get());
    sync.setTiming(fast());
    QSignalSpy synced(&sync, &ChangeFeedSyncer::synced);
    sync.start();
    QVERIFY(waitSynced(synced));
    QVERIFY(synced.last().at(1).toBool());
    QCOMPARE(cache->project(QStringLiteral("P"))->value(QStringLiteral("name")).toString(), QStringLiteral("Renamed"));
  }

  void cadenceFollowsFocus() {
    MockServer mock;
    QVERIFY(mock.listen());
    FakeServer fake;
    mock.setHandler([&](const MockRequest& r) { return fake.handle(r); });
    auto cache = std::move(Cache::open(QStringLiteral(":memory:"))).value();
    AsyncApiClient client(mock.url(), kFakeToken);
    ChangeFeedSyncer sync(&client, cache.get());
    ChangeFeedSyncer::Timing t;  // the real defaults: 15 s / 60 s
    sync.setTiming(t);
    QSignalSpy synced(&sync, &ChangeFeedSyncer::synced);
    sync.start();
    QVERIFY(waitSynced(synced));
    QVERIFY(sync.nextPollInMs() > 14'000 && sync.nextPollInMs() <= 15'000);
    sync.setActive(false);
    QVERIFY2(sync.nextPollInMs() > 59'000, qPrintable(QString::number(sync.nextPollInMs())));
    QVERIFY(sync.nextPollInMs() > 59'000 && sync.nextPollInMs() <= 60'000);
    sync.setActive(true);
    QVERIFY(sync.nextPollInMs() <= 15'000);

    // Short intervals: polls happen on their own, without a health check.
    t.activeMs = 100;
    sync.setTiming(t);
    const int health = fake.hits.value(QStringLiteral("/v1/health"));
    sync.refreshNow();  // F5 checks the server once
    QVERIFY(waitSynced(synced, 4, 3000));
    QCOMPARE(fake.hits.value(QStringLiteral("/v1/health")), health + 1);
    QVERIFY(fake.hits.value(QStringLiteral("/v1/changes")) >= 3);
    sync.stop();
    QCOMPARE(sync.state(), State::Stopped);
    QCOMPARE(sync.nextPollInMs(), -1);
  }
};

QTEST_GUILESS_MAIN(TestSyncer)
#include "tst_syncer.moc"
