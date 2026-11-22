#include "client/DesktopSettings.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include <sys/stat.h>

namespace zproject::client {

DesktopPaths desktopPaths(const QHash<QString, QString>& env, bool macos) {
  DesktopPaths p;
  const QString home = env.value(QStringLiteral("HOME"), QDir::homePath());
  if (macos) {
    p.settingsFile = home + QStringLiteral("/Library/Preferences/zproject/zproject.ini");
    p.cacheDir = home + QStringLiteral("/Library/Caches/zproject");
  } else {
    // Same directory as cli.ini (ClientConfig).
    const ClientConfig c = loadClientConfig(env, QString());
    p.settingsFile = c.configDir + QStringLiteral("/zproject.ini");
    const QString xdgCache = env.value(QStringLiteral("XDG_CACHE_HOME"));
    p.cacheDir = (xdgCache.isEmpty() ? home + QStringLiteral("/.cache") : xdgCache) + QStringLiteral("/zproject");
  }
  if (!env.value(QStringLiteral("ZPROJECT_CACHE_DIR")).isEmpty()) {
    p.cacheDir = env.value(QStringLiteral("ZPROJECT_CACHE_DIR"));
  }
  return p;
}

DesktopSettings loadDesktopSettings(const DesktopPaths& paths) {
  DesktopSettings s;
  const QSettings ini(paths.settingsFile, QSettings::IniFormat);
  s.serverUrl = ini.value(QStringLiteral("server/url")).toString().trimmed();
  s.cacheEnabled = ini.value(QStringLiteral("cache/enabled"), true).toBool();
  s.theme = ini.value(QStringLiteral("appearance/theme"), s.theme).toString();
  s.zoomPercent = ini.value(QStringLiteral("appearance/zoomPercent"), s.zoomPercent).toInt();
  return s;
}

Status saveDesktopSettings(const DesktopPaths& paths, const DesktopSettings& settings) {
  const QString dir = QFileInfo(paths.settingsFile).absolutePath();
  if (!QDir().mkpath(dir)) return Error{QStringLiteral("io"), QStringLiteral("cannot create %1").arg(dir), {}};
  ::chmod(QFile::encodeName(dir).constData(), 0700);
  {
    QSettings ini(paths.settingsFile, QSettings::IniFormat);
    if (settings.serverUrl.isEmpty()) {
      ini.remove(QStringLiteral("server/url"));
    } else {
      ini.setValue(QStringLiteral("server/url"), settings.serverUrl);
    }
    ini.setValue(QStringLiteral("cache/enabled"), settings.cacheEnabled);
    ini.setValue(QStringLiteral("appearance/theme"), settings.theme);
    ini.setValue(QStringLiteral("appearance/zoomPercent"), settings.zoomPercent);
    ini.sync();
    if (ini.status() != QSettings::NoError) {
      return Error{QStringLiteral("io"), QStringLiteral("cannot write %1").arg(paths.settingsFile), {}};
    }
  }
  ::chmod(QFile::encodeName(paths.settingsFile).constData(), 0600);
  return Ok{};
}

Result<Connection> resolveConnection(const QHash<QString, QString>& env, const DesktopSettings& settings,
                                     const ClientConfig& cli, const TokenStore& store) {
  Connection c;
  QString url;
  if (!env.value(QStringLiteral("ZPROJECT_URL")).isEmpty()) {
    url = env.value(QStringLiteral("ZPROJECT_URL"));
    c.urlSource = QStringLiteral("ZPROJECT_URL");
  } else if (!settings.serverUrl.isEmpty()) {
    url = settings.serverUrl;
    c.urlSource = QStringLiteral("zproject.ini");
  } else if (!cli.url.isEmpty()) {
    url = cli.url;
    c.urlSource = QStringLiteral("cli.ini");
  }
  auto u = parseServerUrl(url);
  if (!u) return u.error();
  c.url = u.value();
  auto t = resolveDeviceToken(env, cli.store, store);
  if (!t) return t.error();
  c.token = t->token;
  c.tokenSource = t->source;
  c.tokenLabel = tokenLabel(t->token);
  return c;
}

}  // namespace zproject::client
