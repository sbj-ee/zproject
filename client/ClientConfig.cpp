#include "client/ClientConfig.hpp"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QSettings>

#include <sys/stat.h>

namespace zproject::client {

ClientConfig loadClientConfig(const QHash<QString, QString>& env, const QString& urlFlag) {
  ClientConfig c;
  if (!env.value(QStringLiteral("ZPROJECT_CONFIG_DIR")).isEmpty()) {
    c.configDir = env.value(QStringLiteral("ZPROJECT_CONFIG_DIR"));
  } else if (!env.value(QStringLiteral("XDG_CONFIG_HOME")).isEmpty()) {
    c.configDir = env.value(QStringLiteral("XDG_CONFIG_HOME")) + QStringLiteral("/zproject");
  } else {
    const QString home = env.value(QStringLiteral("HOME"), QDir::homePath());
    c.configDir = home + QStringLiteral("/.config/zproject");
  }
  QSettings ini(c.iniPath(), QSettings::IniFormat);
  const QString iniUrl = ini.value(QStringLiteral("server/url")).toString().trimmed();
  if (!urlFlag.isEmpty()) {
    c.url = urlFlag;
    c.urlSource = QStringLiteral("--url");
  } else if (!env.value(QStringLiteral("ZPROJECT_URL")).isEmpty()) {
    c.url = env.value(QStringLiteral("ZPROJECT_URL"));
    c.urlSource = QStringLiteral("ZPROJECT_URL");
  } else if (!iniUrl.isEmpty()) {
    c.url = iniUrl;
    c.urlSource = QStringLiteral("cli.ini");
  }
  c.store = ini.value(QStringLiteral("auth/store")).toString().trimmed();
  if (!env.value(QStringLiteral("ZPROJECT_TOKEN_FILE")).isEmpty()) {
    c.tokenFile = env.value(QStringLiteral("ZPROJECT_TOKEN_FILE"));
  } else if (!ini.value(QStringLiteral("auth/token_file")).toString().isEmpty()) {
    c.tokenFile = ini.value(QStringLiteral("auth/token_file")).toString();
  } else {
    c.tokenFile = c.configDir + QStringLiteral("/token");
  }
  return c;
}

Status saveClientConfig(const ClientConfig& config) {
  if (!QDir().mkpath(config.configDir)) {
    return Error{QStringLiteral("io"), QStringLiteral("cannot create %1").arg(config.configDir), {}};
  }
  ::chmod(QFile::encodeName(config.configDir).constData(), 0700);
  {
    QSettings ini(config.iniPath(), QSettings::IniFormat);
    if (!config.url.isEmpty()) ini.setValue(QStringLiteral("server/url"), config.url);
    if (!config.store.isEmpty()) ini.setValue(QStringLiteral("auth/store"), config.store);
    ini.sync();
    if (ini.status() != QSettings::NoError) {
      return Error{QStringLiteral("io"), QStringLiteral("cannot write %1").arg(config.iniPath()), {}};
    }
  }
  ::chmod(QFile::encodeName(config.iniPath()).constData(), 0600);
  return Ok{};
}

Result<QUrl> parseServerUrl(const QString& text) {
  auto bad = [&](const QString& why) {
    return Error::validation(QStringLiteral("server URL '%1' %2").arg(text, why));
  };
  QString t = text.trimmed();
  while (t.endsWith(QLatin1Char('/'))) t.chop(1);
  if (t.isEmpty()) {
    return Error::validation(
        QStringLiteral("no server URL: pass --url, set ZPROJECT_URL, or run `zproject-cli login --url URL`"));
  }
  const QUrl u(t, QUrl::StrictMode);
  if (!u.isValid() || u.host().isEmpty()) return bad(QStringLiteral("is not a valid URL"));
  if (!u.userInfo().isEmpty() || u.hasQuery() || u.hasFragment() || !u.path().isEmpty()) {
    return bad(QStringLiteral("must be just scheme://host[:port]"));
  }
  const QString scheme = u.scheme().toLower();
  if (scheme == QLatin1String("https")) return u;
  if (scheme == QLatin1String("http")) {
    const QHostAddress addr(u.host());
    if (u.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 || (!addr.isNull() && addr.isLoopback())) {
      return u;
    }
    return bad(QStringLiteral("uses http:// to a non-loopback host; use https:// (tailscale serve)"));
  }
  return bad(QStringLiteral("must start with https://"));
}

}  // namespace zproject::client
