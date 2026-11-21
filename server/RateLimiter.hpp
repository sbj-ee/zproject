#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <functional>

namespace zproject::server {

// Per-token token buckets (SPEC.md §9.3, decision Q28). Capacities are per
// minute and refill continuously. Defaults by token kind:
//   agent  120 reads, 30 writes, 5 destructive
//   app    600 / 120 / 20
//   device, owner  1,200 requests in total
// A token's `rateLimit` JSON ({"read", "write", "destructive", "total"})
// overrides any of them. Failed authentication: 10 per minute per source.
class RateLimiter {
 public:
  enum class Kind { Read, Write, Destructive };
  using Clock = std::function<qint64()>;  // milliseconds, monotonic

  explicit RateLimiter(Clock clock = {});

  // Takes one request of `kind` for the token; returns 0 when allowed, or the
  // seconds to wait (Retry-After) when not. A destructive request also uses
  // one write.
  int take(const QString& tokenId, const QString& tokenKind, const QJsonObject& overrides, Kind kind);
  int takeFailedAuth(const QString& source);

 private:
  struct Bucket {
    double tokens = -1;  // -1: not initialised (full)
    qint64 lastMs = 0;
  };
  // Refills and reports whether one token is available; Retry-After seconds otherwise.
  int check(Bucket& b, int perMinute, qint64 now);
  static void consume(Bucket& b) { b.tokens -= 1; }

  Clock clock_;
  QHash<QString, Bucket> buckets_;
};

}  // namespace zproject::server
