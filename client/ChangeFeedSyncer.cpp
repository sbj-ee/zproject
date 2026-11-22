#include "client/ChangeFeedSyncer.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QPointer>

#include <algorithm>
#include <utility>

#include "core/Version.hpp"

namespace zproject::client {

ChangeFeedSyncer::ChangeFeedSyncer(AsyncApiClient* api, Cache* cache, QObject* parent)
    : QObject(parent), api_(api), cache_(cache) {
  timer_.setSingleShot(true);
  connect(&timer_, &QTimer::timeout, this, [this] { runCycle(needCheck_); });
}

QString ChangeFeedSyncer::stateName(State s) {
  switch (s) {
    case State::Stopped: return QStringLiteral("stopped");
    case State::Idle: return QStringLiteral("idle");
    case State::Syncing: return QStringLiteral("syncing");
    case State::Offline: return QStringLiteral("offline");
    case State::Halted: return QStringLiteral("halted");
  }
  return QStringLiteral("unknown");
}

void ChangeFeedSyncer::start() {
  stop();
  needCheck_ = true;
  backoffMs_ = 0;
  runCycle(true);
}

void ChangeFeedSyncer::stop() {
  ++generation_;  // drops answers to requests already sent
  timer_.stop();
  busy_ = false;
  pendingCheck_ = false;
  setState(State::Stopped);
}

void ChangeFeedSyncer::refreshNow() {
  if (state_ == State::Stopped || state_ == State::Halted) {
    start();
    return;
  }
  runCycle(true);
}

void ChangeFeedSyncer::setActive(bool active) {
  if (active == active_) return;
  active_ = active;
  if (state_ == State::Idle && timer_.isActive() && !busy_) schedule(interval());
}

int ChangeFeedSyncer::interval() const {
  if (mode_ == Mode::ListPoll) return active_ ? timing_.listPollMs : std::max(timing_.listPollMs, timing_.inactiveMs);
  return active_ ? timing_.activeMs : timing_.inactiveMs;
}

void ChangeFeedSyncer::setState(State s) {
  if (s == state_) return;
  state_ = s;
  emit stateChanged(s);
}

void ChangeFeedSyncer::schedule(int ms) {
  scheduledMs_ = std::max(ms, 0);
  scheduledAt_.start();
  timer_.start(scheduledMs_);
}

int ChangeFeedSyncer::nextPollInMs() const {
  if (!timer_.isActive()) return -1;
  return static_cast<int>(std::max<qint64>(0, scheduledMs_ - scheduledAt_.elapsed()));
}

void ChangeFeedSyncer::request(const QString& path, const QUrlQuery& query, Next next) {
  const int gen = generation_;
  const QPointer<ChangeFeedSyncer> self(this);
  api_->get(path, query, [self, gen, cont = std::move(next)](const ApiResponse& r) {
    ChangeFeedSyncer* me = self.data();
    if (me == nullptr || gen != me->generation_) return;
    const ApiError e = classifyResponse(r);
    if (!e.ok()) {
      me->fail(e);
      return;
    }
    cont(r);
  });
}

void ChangeFeedSyncer::runCycle(bool check) {
  if (busy_) {
    pendingCheck_ = pendingCheck_ || check;
    return;
  }
  timer_.stop();
  busy_ = true;
  cycleFull_ = false;
  cycleProjects_.clear();
  setState(State::Syncing);
  if (check || needCheck_) {
    checkHealth();
  } else if (mode_ == Mode::ListPoll) {
    listSnapshot();
  } else {
    fetchChanges();
  }
}

void ChangeFeedSyncer::checkHealth() {
  request(QStringLiteral("/v1/health"), {}, [this](const ApiResponse& r) {
    auto h = parseHealth(r.json);
    if (!h) {
      ApiError e;
      e.kind = ErrorKind::BadResponse;
      e.status = r.status;
      e.message = h.error().message;
      fail(e);
      return;
    }
    server_ = h.value();
    compat_ = checkCompatibility(h.value(), appVersion(), kApiVersion);
    emit serverChecked(h.value(), compat_);
    if (!compat_.usable()) {
      ApiError e;
      e.kind = ErrorKind::Unprocessable;
      e.code = QStringLiteral("api_version");
      e.message = compat_.message;
      lastError_ = e;
      busy_ = false;
      timer_.stop();
      setState(State::Halted);
      emit failed(e);
      return;
    }
    const int cachedSchema = cache_->schemaVersion();
    if (cachedSchema != 0 && cachedSchema != h->schemaVersion) {
      if (auto s = cache_->reset(); !s) qWarning("zproject: cannot reset the cache: %s", qPrintable(s.error().message));
      cycleFull_ = true;
    }
    if (auto s = cache_->setServer(h.value()); !s) {
      qWarning("zproject: cannot write the cache: %s", qPrintable(s.error().message));
    }
    needCheck_ = false;
    if (mode_ == Mode::ListPoll) {
      listSnapshot();
    } else if (cache_->lastSeq() > 0) {
      verifyPosition();
    } else {
      fetchChanges();
    }
  });
}

void ChangeFeedSyncer::verifyPosition() {
  const qint64 seq = cache_->lastSeq();
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("since"), QString::number(seq - 1));
  q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
  request(QStringLiteral("/v1/changes"), q, [this, seq](const ApiResponse& r) {
    auto page = decodeChangePage(r.json);
    const bool same = page && !page->items.isEmpty() && page->items.first().seq == seq &&
                      page->items.first().ts == cache_->lastSeqTs();
    if (!same) beginFullReload();  // the server was restored (or replaced)
    fetchChanges();
  });
}

void ChangeFeedSyncer::beginFullReload() {
  if (auto s = cache_->reset(); !s) qWarning("zproject: cannot reset the cache: %s", qPrintable(s.error().message));
  if (server_) (void)cache_->setServer(*server_);
  cycleFull_ = true;
}

void ChangeFeedSyncer::fetchChanges() {
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("since"), QString::number(cache_->lastSeq()));
  q.addQueryItem(QStringLiteral("limit"), QString::number(timing_.pageLimit));
  request(QStringLiteral("/v1/changes"), q, [this](const ApiResponse& r) {
    auto page = decodeChangePage(r.json);
    if (!page) {
      ApiError e;
      e.kind = ErrorKind::BadResponse;
      e.status = r.status;
      e.message = page.error().message;
      fail(e);
      return;
    }
    auto applied = cache_->apply(page->items);
    if (!applied) {
      ApiError e;
      e.kind = ErrorKind::BadResponse;
      e.message = QStringLiteral("cannot write the cache: %1").arg(applied.error().message);
      fail(e);
      return;
    }
    cycleProjects_.unite(applied->projects);
    if (applied->incomplete && !cycleFull_) {
      beginFullReload();
      fetchChanges();
      return;
    }
    if (page->more && !page->items.isEmpty()) {
      fetchChanges();
      return;
    }
    fetchWbs();
  });
}

void ChangeFeedSyncer::listPage(const QString& path, const QString& cursor, QList<QJsonObject>* into,
                                std::function<void()> then) {
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("limit"), QString::number(timing_.pageLimit));
  if (!cursor.isEmpty()) q.addQueryItem(QStringLiteral("cursor"), cursor);
  request(path, q, [this, path, into, then = std::move(then)](const ApiResponse& r) mutable {
    for (const QJsonValue& v : r.json.value(QStringLiteral("items")).toArray()) into->append(v.toObject());
    const QString next = r.json.value(QStringLiteral("nextCursor")).toString();
    if (!next.isEmpty()) {
      listPage(path, next, into, std::move(then));
      return;
    }
    then();
  });
}

void ChangeFeedSyncer::listSnapshot() {
  snapProjects_.clear();
  snapTasks_.clear();
  listPage(QStringLiteral("/v1/projects"), {}, &snapProjects_, [this] {
    snapQueue_.clear();
    for (const QJsonObject& p : std::as_const(snapProjects_)) snapQueue_.append(p.value(QStringLiteral("id")).toString());
    nextSnapshotProject();
  });
}

void ChangeFeedSyncer::nextSnapshotProject() {
  if (!snapQueue_.isEmpty()) {
    const QString id = snapQueue_.takeFirst();
    listPage(QStringLiteral("/v1/projects/%1/tasks").arg(id), {}, &snapTasks_, [this] { nextSnapshotProject(); });
    return;
  }
  if (auto s = cache_->replaceSnapshot(snapProjects_, snapTasks_); !s) {
    ApiError e;
    e.kind = ErrorKind::BadResponse;
    e.message = QStringLiteral("cannot write the cache: %1").arg(s.error().message);
    fail(e);
    return;
  }
  for (const QJsonObject& p : std::as_const(snapProjects_)) cycleProjects_.insert(p.value(QStringLiteral("id")).toString());
  fetchWbs();
}

void ChangeFeedSyncer::fetchWbs() {
  wbsQueue_.clear();
  for (const QString& id : std::as_const(cycleProjects_)) {
    if (cache_->project(id)) wbsQueue_.append(id);
  }
  std::sort(wbsQueue_.begin(), wbsQueue_.end());
  nextWbs();
}

void ChangeFeedSyncer::nextWbs() {
  if (wbsQueue_.isEmpty()) {
    finishCycle();
    return;
  }
  const QString id = wbsQueue_.takeFirst();
  request(QStringLiteral("/v1/projects/%1/wbs").arg(id), {}, [this, id](const ApiResponse& r) {
    if (auto s = cache_->setWbs(id, r.json.value(QStringLiteral("items")).toArray()); !s) {
      qWarning("zproject: cannot write the cache: %s", qPrintable(s.error().message));
    }
    nextWbs();
  });
}

void ChangeFeedSyncer::finishCycle() {
  (void)cache_->markRefreshed(QDateTime::currentDateTimeUtc());
  busy_ = false;
  backoffMs_ = 0;
  lastError_ = ApiError{};
  setState(State::Idle);
  const QSet<QString> projects = cycleProjects_;
  const bool full = cycleFull_;
  const int gen = generation_;
  emit synced(projects, full);
  if (gen != generation_) return;  // stopped or restarted from a slot
  if (pendingCheck_) {
    pendingCheck_ = false;
    runCycle(true);
    return;
  }
  schedule(interval());
}

void ChangeFeedSyncer::fail(const ApiError& e) {
  busy_ = false;
  if (e.kind == ErrorKind::ForbiddenProject && mode_ == Mode::Feed) {
    // A project-limited token: poll the lists instead (SPEC.md §16.5).
    mode_ = Mode::ListPoll;
    runCycle(false);
    return;
  }
  lastError_ = e;
  const int gen = generation_;
  if (e.stopsSync()) {
    timer_.stop();
    setState(State::Halted);
  } else if (e.goesOffline()) {
    needCheck_ = true;  // check health and the feed position on reconnect
    backoffMs_ = backoffMs_ == 0 ? timing_.backoffMinMs : std::min(backoffMs_ * 2, timing_.backoffMaxMs);
    setState(State::Offline);
    schedule(backoffMs_);
  } else if (e.kind == ErrorKind::RateLimited) {
    setState(State::Idle);
    schedule(std::max(e.retryAfterSeconds, 1) * 1000);
  } else {
    setState(State::Idle);
    schedule(interval());
  }
  if (gen == generation_) emit failed(e);
}

}  // namespace zproject::client
