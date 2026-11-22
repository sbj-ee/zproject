#pragma once

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QString>

#include <memory>

#include "client/AsyncApiClient.hpp"
#include "client/Cache.hpp"
#include "client/ChangeFeedSyncer.hpp"
#include "client/DesktopSettings.hpp"

namespace zproject::app {

// One connection of the desktop app to zprojectd (SPEC.md §16.5): the
// non-blocking client, the read-only cache and the change-feed syncer from
// client/, and what the window shows about them. Editing is allowed only
// while the server answers; offline the window shows cached data read-only.
class AppSession : public QObject {
  Q_OBJECT

 public:
  enum class Health {
    Connecting,    // before the first answer
    Online,        // the last cycle reached the server
    Offline,       // network, timeout or 5xx: cached data, retrying
    SignInNeeded,  // 401: the token was refused
    Unsupported,   // another API major
    TlsError,      // the certificate could not be verified
  };

  // cachePath ":memory:" keeps nothing on disk (Preferences → cache off).
  AppSession(client::Connection connection, QString cachePath, QObject* parent = nullptr);
  ~AppSession() override;
  AppSession(const AppSession&) = delete;
  AppSession& operator=(const AppSession&) = delete;

  // Opens the cache; false with *error if it can't (the app then runs
  // with a memory cache).
  bool open(QString* error);
  void start();
  void refresh();               // F5
  void setActive(bool active);  // window focus: 15 s vs 60 s
  void setTiming(const client::ChangeFeedSyncer::Timing& t);
  // Preferences → Clear cache: deletes the cache files and loads everything
  // again from the server.
  bool clearCache(QString* error);

  Health health() const { return health_; }
  bool editingAllowed() const { return health_ == Health::Online; }
  QDateTime lastRefresh() const;  // local time; invalid if never
  QString serverVersion() const;
  QString compatibilityNote() const;  // older/newer server, or empty
  QString statusText() const;          // for the status bar
  QString lastErrorText() const;
  const client::Connection& connection() const { return connection_; }
  QString cachePath() const { return cachePath_; }

  client::Cache* cache() const { return cache_.get(); }
  client::ChangeFeedSyncer* syncer() const { return syncer_.get(); }
  client::AsyncApiClient* api() const { return api_.get(); }

  static QString healthName(Health h);

 signals:
  void healthChanged(zproject::app::AppSession::Health health);
  void dataChanged(const QSet<QString>& projects, bool fullReload);
  void statusChanged();

 private:
  void makeSyncer();
  void updateHealth();

  client::Connection connection_;
  QString cachePath_;
  std::unique_ptr<client::AsyncApiClient> api_;
  std::unique_ptr<client::Cache> cache_;
  std::unique_ptr<client::ChangeFeedSyncer> syncer_;
  client::ChangeFeedSyncer::Timing timing_;
  bool active_ = true;
  Health health_ = Health::Connecting;
};

}  // namespace zproject::app
