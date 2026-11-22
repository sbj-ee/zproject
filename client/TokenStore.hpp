#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>

#include "core/Error.hpp"

namespace zproject::client {

// One external command (security, secret-tool). Tests replace the runner.
struct CommandResult {
  int exitCode = -1;  // -1: could not start
  QByteArray out;
  QByteArray err;
};
using CommandRunner =
    std::function<CommandResult(const QString& program, const QStringList& args, const QByteArray& input)>;
// QProcess with a 15 s timeout; `input` goes to stdin, never to argv.
CommandResult runCommand(const QString& program, const QStringList& args, const QByteArray& input);

// True for "zpt_<20 hex>.<43 base64url>" (SPEC.md §9.2).
bool looksLikeToken(const QString& token);
// "zpt_<id>": safe to show; the secret part is never printed.
QString tokenLabel(const QString& token);

// Where zproject-cli keeps its token (SPEC.md §10: never a command-line
// flag). Two stores:
//   keychain  macOS: the login Keychain (/usr/bin/security), service
//             "ee.sbj.zproject", account "zproject-cli". Linux: the Secret
//             Service through `secret-tool` (libsecret-tools), same
//             attributes. The token reaches both tools on stdin.
//   file      <config dir>/token (or ZPROJECT_TOKEN_FILE): a regular file
//             owned by the user, mode 0600, in a 0700 directory; anything
//             readable by others is refused.
class TokenStore {
 public:
  enum class Kind { File, Keychain };

  TokenStore(QString tokenFile, CommandRunner runner, bool macos);

  static QString kindName(Kind k) { return k == Kind::File ? QStringLiteral("file") : QStringLiteral("keychain"); }
  QString tokenFile() const { return tokenFile_; }
  bool macos() const { return macos_; }

  // File store. read: not_found when there is no file; insecure when its
  // owner or mode is wrong.
  Result<QString> readFile() const;
  Status writeFile(const QString& token) const;
  Status removeFile() const;

  // Keychain store. read: not_found when no entry; unavailable when the tool
  // is missing (secret-tool not installed, no session bus).
  Result<QString> readKeychain() const;
  Status writeKeychain(const QString& token) const;
  Status removeKeychain() const;

  static constexpr const char* kService = "ee.sbj.zproject";
  static constexpr const char* kAccount = "zproject-cli";

 private:
  QString tokenFile_;
  CommandRunner run_;
  bool macos_;
};

// The token this machine uses, shared by zproject-cli and the desktop app so
// each machine keeps one device identity (SPEC.md §10, §16.5): ZPROJECT_TOKEN,
// else the store cli.ini names (`[auth] store=keychain`), else the token file,
// else (macOS, no store chosen) the Keychain. `source` says where it came
// from ("ZPROJECT_TOKEN", "keychain" or "file <path>"); it never holds the
// secret. Errors: code "auth" with a message fit for the user.
struct ResolvedToken {
  QString token;
  QString source;
};
Result<ResolvedToken> resolveDeviceToken(const QHash<QString, QString>& env, const QString& configuredStore,
                                         const TokenStore& store);

}  // namespace zproject::client
