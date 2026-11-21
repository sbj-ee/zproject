#pragma once

#include <QHostAddress>
#include <QString>
#include <QStringList>

namespace zproject::server {

// Where zprojectd listens (SPEC.md §9.1): 127.0.0.1:8788 by default, behind
// `tailscale serve`. Only loopback addresses are accepted: no raw LAN bind,
// never a public interface.
struct ListenAddress {
  QHostAddress host{QHostAddress::LocalHost};
  quint16 port = 8788;
};

constexpr quint16 kDefaultPort = 8788;

// Parses "HOST:PORT", ":PORT" or "PORT". Returns false and sets *error for
// anything that is not a loopback address with a port in 1..65535.
bool parseListenAddress(const QString& text, ListenAddress* out, QString* error);

// zprojectd settings (SPEC.md §4.4, §9.1): an INI file, by default
// /etc/zproject/zprojectd.conf, overridable on the command line. The public
// repository only carries placeholders; real host names live on the host.
//
//   [server]
//   listen=127.0.0.1:8788
//   database=/var/lib/zproject/zproject.db
//   allowed_hosts=<server-host>.<tailnet>.ts.net     ; comma-separated
//   [backup]
//   dir=/var/lib/zproject/backups
//   [alerts]
//   notify_hub_url=<notify-hub-url>                  ; empty = alerts off
//   topic=zproject
//   token_file=/etc/zproject/notify-hub.token
struct Config {
  ListenAddress listen;
  QString database = QStringLiteral("/var/lib/zproject/zproject.db");
  // Host header values accepted besides the loopback names for the port
  // (DNS-rebinding guard, SPEC.md §14). An entry without a port matches any.
  QStringList allowedHosts;
  QString backupDir = QStringLiteral("/var/lib/zproject/backups");
  QString notifyHubUrl;
  QString notifyTopic = QStringLiteral("zproject");
  QString notifyTokenFile = QStringLiteral("/etc/zproject/notify-hub.token");
};

constexpr const char* kDefaultConfigPath = "/etc/zproject/zprojectd.conf";

// Reads `path` over the defaults in *config. A missing file is an error only
// when `required`. Validation errors (non-loopback listen, a bad URL) fail.
bool loadConfig(const QString& path, bool required, Config* config, QString* error);

// The Host values a request may carry: loopback names for the listen port
// plus config.allowedHosts.
QStringList effectiveAllowedHosts(const Config& config);
bool hostAllowed(const QStringList& allowed, const QByteArray& hostHeader);

}  // namespace zproject::server
