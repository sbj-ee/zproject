#include "RateLimiter.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <vector>

namespace zproject::server {
namespace {

struct Limits {
  int read = 0, write = 0, destructive = 0, total = 0;  // 0 = no such bucket
};

Limits defaultsFor(const QString& kind) {
  if (kind == QLatin1String("agent")) return {120, 30, 5, 0};
  if (kind == QLatin1String("app")) return {600, 120, 20, 0};
  return {0, 0, 0, 1200};  // device, owner
}

}  // namespace

RateLimiter::RateLimiter(Clock clock) : clock_(std::move(clock)) {
  if (!clock_) {
    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    clock_ = [timer] { return timer->elapsed(); };
  }
}

int RateLimiter::check(Bucket& b, int perMinute, qint64 now) {
  const double cap = perMinute;
  if (b.tokens < 0) {
    b.tokens = cap;
    b.lastMs = now;
  }
  const double refill = static_cast<double>(now - b.lastMs) * cap / 60000.0;
  b.tokens = std::min(cap, b.tokens + refill);
  b.lastMs = now;
  if (b.tokens >= 1.0) return 0;
  const double secs = (1.0 - b.tokens) * 60.0 / cap;
  return std::max(1, static_cast<int>(std::ceil(secs)));
}

int RateLimiter::take(const QString& tokenId, const QString& tokenKind, const QJsonObject& overrides, Kind kind) {
  Limits l = defaultsFor(tokenKind);
  auto over = [&](const char* key, int& field) {
    const QJsonValue v = overrides.value(QLatin1String(key));
    if (v.isDouble() && v.toInt() > 0) field = v.toInt();
  };
  over("read", l.read);
  over("write", l.write);
  over("destructive", l.destructive);
  over("total", l.total);

  const qint64 now = clock_();
  std::vector<std::pair<Bucket*, int>> needed;
  auto need = [&](const char* name, int perMinute) {
    if (perMinute > 0) needed.emplace_back(&buckets_[tokenId + QLatin1Char('/') + QLatin1String(name)], perMinute);
  };
  need("total", l.total);
  if (kind == Kind::Read) need("read", l.read);
  if (kind == Kind::Write || kind == Kind::Destructive) need("write", l.write);
  if (kind == Kind::Destructive) need("destructive", l.destructive);

  int wait = 0;
  for (auto& [bucket, perMinute] : needed) wait = std::max(wait, check(*bucket, perMinute, now));
  if (wait > 0) return wait;
  for (auto& [bucket, perMinute] : needed) consume(*bucket);
  return 0;
}

int RateLimiter::takeFailedAuth(const QString& source) {
  Bucket& b = buckets_[QStringLiteral("auth-fail/") + source];
  const int wait = check(b, 10, clock_());
  if (wait == 0) consume(b);
  return wait;
}

}  // namespace zproject::server
