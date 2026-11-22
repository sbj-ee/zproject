#include "app/AppSession.hpp"

#include <QLocale>

namespace zproject::app {

using client::ChangeFeedSyncer;
using client::ErrorKind;

AppSession::AppSession(client::Connection connection, QString cachePath, QObject* parent)
    : QObject(parent),
      connection_(std::move(connection)),
      cachePath_(std::move(cachePath)),
      api_(std::make_unique<client::AsyncApiClient>(connection_.url, connection_.token.toLatin1())) {}

AppSession::~AppSession() {
  syncer_.reset();  // before the cache and client it uses
}

bool AppSession::open(QString* error) {
  auto c = client::Cache::open(cachePath_);
  if (!c) {
    if (error) *error = c.error().message;
    cachePath_ = QStringLiteral(":memory:");
    c = client::Cache::open(cachePath_);
    if (!c) return false;
  }
  cache_ = std::move(c).value();
  makeSyncer();
  return true;
}

void AppSession::makeSyncer() {
  syncer_ = std::make_unique<ChangeFeedSyncer>(api_.get(), cache_.get());
  syncer_->setTiming(timing_);
  syncer_->setActive(active_);
  connect(syncer_.get(), &ChangeFeedSyncer::stateChanged, this, [this] { updateHealth(); });
  connect(syncer_.get(), &ChangeFeedSyncer::failed, this, [this] { updateHealth(); });
  connect(syncer_.get(), &ChangeFeedSyncer::serverChecked, this, [this] { emit statusChanged(); });
  connect(syncer_.get(), &ChangeFeedSyncer::synced, this, [this](const QSet<QString>& projects, bool full) {
    updateHealth();
    emit dataChanged(projects, full);
    emit statusChanged();
  });
}

void AppSession::setTiming(const ChangeFeedSyncer::Timing& t) {
  timing_ = t;
  if (syncer_) syncer_->setTiming(t);
}

void AppSession::start() {
  if (syncer_) syncer_->start();
}

void AppSession::refresh() {
  if (syncer_) syncer_->refreshNow();
}

void AppSession::setActive(bool active) {
  active_ = active;
  if (syncer_) syncer_->setActive(active);
}

bool AppSession::clearCache(QString* error) {
  if (syncer_) syncer_->stop();
  syncer_.reset();
  api_->cancelAll();
  cache_.reset();
  if (cachePath_ != QLatin1String(":memory:")) {
    if (auto s = client::Cache::removeFiles(cachePath_); !s) {
      if (error) *error = s.error().message;
    }
  }
  if (!open(error)) return false;
  emit dataChanged({}, true);
  start();
  return true;
}

void AppSession::updateHealth() {
  Health h = health_;
  switch (syncer_ ? syncer_->state() : ChangeFeedSyncer::State::Stopped) {
    case ChangeFeedSyncer::State::Idle:
      h = Health::Online;
      break;
    case ChangeFeedSyncer::State::Offline:
      h = Health::Offline;
      break;
    case ChangeFeedSyncer::State::Halted: {
      const auto& e = syncer_->lastError();
      if (e.kind == ErrorKind::Unauthenticated) {
        h = Health::SignInNeeded;
      } else if (e.kind == ErrorKind::Tls) {
        h = Health::TlsError;
      } else {
        h = Health::Unsupported;
      }
      break;
    }
    case ChangeFeedSyncer::State::Syncing:
    case ChangeFeedSyncer::State::Stopped:
      break;  // keep what we last knew
  }
  if (h != health_) {
    health_ = h;
    emit healthChanged(h);
  }
  emit statusChanged();
}

QDateTime AppSession::lastRefresh() const {
  if (!cache_) return {};
  const QDateTime t = cache_->refreshedAt();
  return t.isValid() ? t.toLocalTime() : t;
}

QString AppSession::serverVersion() const {
  if (syncer_) {
    if (const auto& server = syncer_->server(); server.has_value()) return server.value().version;
  }
  return cache_ ? cache_->serverVersion() : QString();
}

QString AppSession::compatibilityNote() const { return syncer_ ? syncer_->compatibility().message : QString(); }

QString AppSession::lastErrorText() const { return syncer_ ? syncer_->lastError().message : QString(); }

QString AppSession::healthName(Health h) {
  switch (h) {
    case Health::Connecting: return tr("Connecting…");
    case Health::Online: return tr("Connected");
    case Health::Offline: return tr("Offline");
    case Health::SignInNeeded: return tr("Sign-in needed");
    case Health::Unsupported: return tr("Server not supported");
    case Health::TlsError: return tr("Certificate problem");
  }
  return {};
}

QString AppSession::statusText() const {
  QString s = healthName(health_);
  const QString v = serverVersion();
  if (health_ == Health::Online && !v.isEmpty()) s += tr(" · zprojectd %1").arg(v);
  return s;
}

}  // namespace zproject::app
