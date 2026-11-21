#include "ServerConfig.hpp"

namespace zproject::server {

bool parseListenAddress(const QString& text, ListenAddress* out, QString* error) {
  const QString t = text.trimmed();
  QString hostPart;
  QString portPart = t;
  const qsizetype colon = t.lastIndexOf(QLatin1Char(':'));
  if (colon >= 0) {
    hostPart = t.left(colon);
    portPart = t.mid(colon + 1);
  }
  if (hostPart.startsWith(QLatin1Char('[')) && hostPart.endsWith(QLatin1Char(']'))) {
    hostPart = hostPart.mid(1, hostPart.size() - 2);
  }

  bool ok = false;
  const uint port = portPart.toUInt(&ok);
  if (!ok || port == 0 || port > 65535) {
    if (error) *error = QStringLiteral("invalid port in '%1'").arg(text);
    return false;
  }

  QHostAddress host(QHostAddress::LocalHost);
  if (!hostPart.isEmpty()) {
    if (hostPart == QLatin1String("localhost")) {
      host = QHostAddress(QHostAddress::LocalHost);
    } else if (!host.setAddress(hostPart)) {
      if (error) *error = QStringLiteral("invalid address '%1'").arg(hostPart);
      return false;
    }
  }
  if (!host.isLoopback()) {
    if (error) {
      *error = QStringLiteral("refusing to listen on %1: only loopback addresses are allowed "
                              "(publish with `tailscale serve`)").arg(host.toString());
    }
    return false;
  }

  if (out) {
    out->host = host;
    out->port = static_cast<quint16>(port);
  }
  return true;
}

}  // namespace zproject::server

#include <QFileInfo>
#include <QSettings>
#include <QUrl>

namespace zproject::server {

bool loadConfig(const QString& path, bool required, Config* config, QString* error) {
  if (!QFileInfo::exists(path)) {
    if (required && error) *error = QStringLiteral("config file %1 not found").arg(path);
    return !required;
  }
  QSettings ini(path, QSettings::IniFormat);
  if (ini.status() != QSettings::NoError) {
    if (error) *error = QStringLiteral("cannot parse %1").arg(path);
    return false;
  }
  auto str = [&](const char* key, QString* out) {
    const QVariant v = ini.value(QLatin1String(key));
    if (v.isValid()) {
      // QSettings splits unquoted values at commas.
      *out = v.typeId() == QMetaType::QStringList ? v.toStringList().join(QLatin1Char(',')).trimmed()
                                                   : v.toString().trimmed();
    }
  };
  QString listen;
  str("server/listen", &listen);
  if (!listen.isEmpty() && !parseListenAddress(listen, &config->listen, error)) return false;
  str("server/database", &config->database);
  QString hosts;
  str("server/allowed_hosts", &hosts);
  for (const QString& h : hosts.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const QString t = h.trimmed().toLower();
    if (!t.isEmpty() && !t.startsWith(QLatin1Char('<'))) config->allowedHosts.append(t);
  }
  str("backup/dir", &config->backupDir);
  str("alerts/notify_hub_url", &config->notifyHubUrl);
  if (config->notifyHubUrl.startsWith(QLatin1Char('<'))) config->notifyHubUrl.clear();  // placeholder
  str("alerts/topic", &config->notifyTopic);
  str("alerts/token_file", &config->notifyTokenFile);
  if (!config->notifyHubUrl.isEmpty()) {
    const QUrl url(config->notifyHubUrl);
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
      if (error) *error = QStringLiteral("alerts/notify_hub_url is not an http(s) URL");
      return false;
    }
  }
  return true;
}

QStringList effectiveAllowedHosts(const Config& config) {
  const QString port = QString::number(config.listen.port);
  QStringList out{QStringLiteral("127.0.0.1:") + port, QStringLiteral("localhost:") + port,
                  QStringLiteral("[::1]:") + port};
  out += config.allowedHosts;
  return out;
}

bool hostAllowed(const QStringList& allowed, const QByteArray& hostHeader) {
  const QString host = QString::fromLatin1(hostHeader).trimmed().toLower();
  if (host.isEmpty()) return false;
  // Split off a port, keeping IPv6 literals intact.
  QString name = host;
  const qsizetype colon = host.lastIndexOf(QLatin1Char(':'));
  if (colon > 0 && !host.endsWith(QLatin1Char(']'))) name = host.left(colon);
  for (const QString& a : allowed) {
    const bool hasPort = a.lastIndexOf(QLatin1Char(':')) > a.lastIndexOf(QLatin1Char(']'));
    if (hasPort ? host == a : name == a) return true;
  }
  return false;
}

}  // namespace zproject::server
