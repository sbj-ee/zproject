#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <deque>
#include <functional>
#include <optional>

#include "client/ApiClient.hpp"

class QNetworkAccessManager;

namespace zproject::client {

// One /v1 request for AsyncApiClient.
struct ApiRequest {
  QByteArray method = "GET";
  QString path;
  QUrlQuery query;
  std::optional<QJsonObject> body;
  // The entity revision the user started from; sent as If-Match (SPEC.md
  // §9.3). Every desktop PATCH sets it.
  std::optional<qint64> ifMatch;
  bool dryRun = false;        // adds ?dryRun=true
  QByteArray confirmId;       // X-Zproject-Confirm, from a dry run
  // Writes with the same key run one after another, in order; empty means
  // the request path.
  QString serializeKey;

  bool isWrite() const { return method != "GET" && method != "HEAD"; }
};

// The desktop's non-blocking /v1 client (SPEC.md §4.1, §9.3) on
// QNetworkAccessManager. It shares request building with the CLI's
// synchronous ApiClient: bearer token, no redirects, TLS peer verification
// required (certificate errors are never ignored; there is no setting for
// it), a timeout per request.
//
// Reads run in parallel up to a cap (4). Writes are serialized per entity.
// After a 429 no new request starts until Retry-After has passed. Nothing is
// retried here: callers decide (the syncer polls again; an edit is the
// user's to repeat). Callbacks always run later from the event loop, never
// inside the call that queued them.
class AsyncApiClient : public QObject {
  Q_OBJECT

 public:
  using Callback = std::function<void(const ApiResponse&)>;

  AsyncApiClient(QUrl base, QByteArray token, QObject* parent = nullptr);
  ~AsyncApiClient() override;
  AsyncApiClient(const AsyncApiClient&) = delete;
  AsyncApiClient& operator=(const AsyncApiClient&) = delete;

  void setTimeoutMs(int ms) { timeoutMs_ = ms; }
  void setMaxParallelReads(int n) { maxReads_ = n < 1 ? 1 : n; }
  const QUrl& base() const { return base_; }

  // Queues a request; returns its id (also sent as X-Zproject-Client-Request).
  quint64 send(ApiRequest request, Callback done);
  quint64 get(const QString& path, const QUrlQuery& query, Callback done);
  // PATCH with If-Match: <rev>.
  quint64 patch(const QString& path, const QJsonObject& body, qint64 rev, Callback done, bool dryRun = false,
                const QByteArray& confirmId = {});

  // Drops queued requests and aborts running ones; their callbacks get a
  // network error ("canceled").
  void cancelAll();

  int running() const { return runningReads_ + static_cast<int>(busyKeys_.size()); }
  int queued() const { return static_cast<int>(queue_.size()); }
  // Milliseconds until requests may start again after a 429 (0: now).
  qint64 rateLimitRemainingMs() const;

 signals:
  // Every finished request, after its callback; for status bars and logs.
  void finished(quint64 id, int status);

 private:
  struct Pending {
    quint64 id = 0;
    ApiRequest request;
    Callback done;
  };
  void pump();
  bool canStart(const Pending& p) const;
  void start(Pending p);

  QUrl base_;
  QByteArray token_;
  int timeoutMs_ = 20'000;
  int maxReads_ = 4;
  quint64 nextId_ = 0;
  int runningReads_ = 0;
  QSet<QString> busyKeys_;
  std::deque<Pending> queue_;
  QNetworkAccessManager* nam_;
  QTimer resumeTimer_;
  QElapsedTimer clock_;
  qint64 pausedUntilMs_ = 0;
};

}  // namespace zproject::client
