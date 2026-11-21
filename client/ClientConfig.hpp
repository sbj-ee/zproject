#pragma once

#include <QHash>
#include <QString>
#include <QUrl>

#include "core/Error.hpp"

namespace zproject::client {

// zproject-cli settings (SPEC.md §10, §16.5). The config directory is
// ZPROJECT_CONFIG_DIR, else $XDG_CONFIG_HOME/zproject, else
// ~/.config/zproject (macOS too, like other command-line tools). It holds
// cli.ini and, with the file store, the token.
//
// cli.ini:
//   [server]
//   url=https://<server-host>.<tailnet>.ts.net
//   [auth]
//   store=keychain          ; or file
//   token_file=/path/token  ; optional, file store only
struct ClientConfig {
  QString configDir;
  QString url;         // as configured; validate with parseServerUrl
  QString urlSource;   // "--url", "ZPROJECT_URL", "cli.ini" or ""
  QString store;       // "keychain", "file" or "" (not chosen yet)
  QString tokenFile;   // ZPROJECT_TOKEN_FILE, cli.ini token_file, or <configDir>/token

  QString iniPath() const { return configDir + QStringLiteral("/cli.ini"); }
};

ClientConfig loadClientConfig(const QHash<QString, QString>& env, const QString& urlFlag);
// Writes [server] url and [auth] store to cli.ini (mode 0600).
Status saveClientConfig(const ClientConfig& config);

// https://host[:port] for any host; http:// only for a loopback host, so a
// token never crosses a network in clear text. No path, query, fragment or
// user info. Returns the URL without a trailing slash.
Result<QUrl> parseServerUrl(const QString& text);

}  // namespace zproject::client
