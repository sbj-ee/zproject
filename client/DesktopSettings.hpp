#pragma once

#include <QHash>
#include <QString>
#include <QUrl>

#include "client/ClientConfig.hpp"
#include "client/TokenStore.hpp"
#include "core/Error.hpp"

namespace zproject::client {

// Where the desktop app keeps its own files (SPEC.md §16.5).
//   Linux: settings ~/.config/zproject/zproject.ini (XDG_CONFIG_HOME, or
//          ZPROJECT_CONFIG_DIR as for the CLI), cache ~/.cache/zproject/
//          (XDG_CACHE_HOME).
//   macOS: ~/Library/Preferences/zproject/zproject.ini and
//          ~/Library/Caches/zproject/.
// ZPROJECT_CACHE_DIR overrides the cache directory (tests). cli.ini and the
// token stay where zproject-cli keeps them (ClientConfig).
struct DesktopPaths {
  QString settingsFile;
  QString cacheDir;
  QString cacheFile() const { return cacheDir + QStringLiteral("/cache.db"); }
};
DesktopPaths desktopPaths(const QHash<QString, QString>& env, bool macos);

// The app's settings. The server URL defaults to cli.ini's, so a machine set
// up for zproject-cli needs no second setup.
struct DesktopSettings {
  QString serverUrl;          // empty: use cli.ini's
  bool cacheEnabled = true;   // off: nothing is written to the cache
  QString theme = QStringLiteral("system");  // system, light, dark, high-contrast
  int zoomPercent = 100;      // UI text zoom (View → Zoom)
};
DesktopSettings loadDesktopSettings(const DesktopPaths& paths);
// Writes zproject.ini, mode 0600 in a 0700 directory. Never a token.
Status saveDesktopSettings(const DesktopPaths& paths, const DesktopSettings& settings);

// Everything needed to connect: the URL (https://, or http:// to loopback
// only) and this machine's device token from the CLI's store.
struct Connection {
  QUrl url;
  QString urlSource;    // "zproject.ini", "ZPROJECT_URL" or "cli.ini"
  QString token;        // the secret: never shown, logged or cached
  QString tokenSource;  // "file <path>", "keychain" or "ZPROJECT_TOKEN"
  QString tokenLabel;   // "zpt_<id>", safe to show
};
// Errors: validation (no or bad URL), auth (no usable token).
Result<Connection> resolveConnection(const QHash<QString, QString>& env, const DesktopSettings& settings,
                                     const ClientConfig& cli, const TokenStore& store);

}  // namespace zproject::client
