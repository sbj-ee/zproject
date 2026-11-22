#pragma once

#include <QJsonObject>
#include <QList>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

#include "client/ApiError.hpp"
#include "client/AsyncApiClient.hpp"
#include "client/Cache.hpp"

namespace zproject::client {

// Keeps the cache in step with the server (SPEC.md §16.5):
//   - GET /v1/changes?since=<lastSeq>&limit=500, looping while `more`;
//   - at start, on refreshNow() (F5) and after being offline: GET /v1/health
//     (API-version gate; a new schemaVersion means a full reload) and a
//     position check: the record at lastSeq must still exist with the same
//     ts, otherwise the server was restored and the cache is rebuilt;
//   - every 15 s while the window is active, 60 s otherwise;
//   - offline (network, timeout, 5xx): backoff 5 s → 60 s, cache read-only;
//   - 429: waits Retry-After; 401 and TLS errors stop polling until start();
//   - a project-limited token can't read the feed (403 forbidden_project):
//     it falls back to polling the project and task lists every 30 s.
// WBS codes are refetched for each project that changed.
class ChangeFeedSyncer : public QObject {
  Q_OBJECT

 public:
  enum class State { Stopped, Idle, Syncing, Offline, Halted };
  enum class Mode { Feed, ListPoll };
  struct Timing {
    int activeMs = 15'000;
    int inactiveMs = 60'000;
    int listPollMs = 30'000;
    int backoffMinMs = 5'000;
    int backoffMaxMs = 60'000;
    int pageLimit = 500;
  };

  ChangeFeedSyncer(AsyncApiClient* api, Cache* cache, QObject* parent = nullptr);

  void setTiming(const Timing& t) { timing_ = t; }
  void start();
  void stop();
  void refreshNow();
  void setActive(bool active);

  State state() const { return state_; }
  Mode mode() const { return mode_; }
  bool active() const { return active_; }
  // Milliseconds until the next poll; -1 when none is scheduled.
  int nextPollInMs() const;
  const ApiError& lastError() const { return lastError_; }
  const std::optional<ServerHealth>& server() const { return server_; }
  const Compatibility& compatibility() const { return compat_; }

  static QString stateName(State s);

 signals:
  void stateChanged(zproject::client::ChangeFeedSyncer::State state);
  // After every successful cycle; `projects` changed (may be empty).
  void synced(const QSet<QString>& projects, bool fullReload);
  void failed(const zproject::client::ApiError& error);
  void serverChecked(const zproject::client::ServerHealth& health, const zproject::client::Compatibility& compat);

 private:
  using Next = std::function<void(const ApiResponse&)>;
  void request(const QString& path, const QUrlQuery& query, Next next);
  void runCycle(bool check);
  void checkHealth();
  void verifyPosition();
  void fetchChanges();
  void beginFullReload();
  void listPage(const QString& path, const QString& cursor, QList<QJsonObject>* into, std::function<void()> then);
  void listSnapshot();
  void nextSnapshotProject();
  void fetchWbs();
  void nextWbs();
  void finishCycle();
  void fail(const ApiError& e);
  void setState(State s);
  void schedule(int ms);
  int interval() const;

  AsyncApiClient* api_;
  Cache* cache_;
  Timing timing_;
  QTimer timer_;
  QElapsedTimer scheduledAt_;
  int scheduledMs_ = 0;
  State state_ = State::Stopped;
  Mode mode_ = Mode::Feed;
  bool active_ = true;
  bool needCheck_ = true;
  bool pendingCheck_ = false;
  bool busy_ = false;
  bool cycleFull_ = false;
  int generation_ = 0;
  int backoffMs_ = 0;
  QSet<QString> cycleProjects_;
  QList<QJsonObject> snapProjects_;
  QList<QJsonObject> snapTasks_;
  QList<QString> snapQueue_;
  QList<QString> wbsQueue_;
  ApiError lastError_;
  std::optional<ServerHealth> server_;
  Compatibility compat_;
};

}  // namespace zproject::client
