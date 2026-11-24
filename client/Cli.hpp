#pragma once

#include <QDate>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <functional>

#include "client/TokenStore.hpp"

namespace zproject::client {

// Exit codes (SPEC.md §10).
enum ExitCode : int {
  kExitOk = 0,
  kExitError = 1,
  kExitUsage = 2,
  kExitConflict = 3,  // validation or conflict (400, 409, 412, 415)
  kExitAuth = 4,      // 401, 403, no or unusable token
  kExitRateLimited = 5,
};

// Everything zproject-cli takes from its process, so tests can run it in
// process: environment, stdin, the no-echo prompt, whether stdout is a
// terminal, the platform and the runner for keychain tools.
struct CliIo {
  QHash<QString, QString> env;
  std::function<QByteArray()> readStdin;
  // Reads a secret from the terminal without echo; a null QString when
  // there is no terminal.
  std::function<QString(const QString& prompt)> promptSecret;
  bool stdoutIsTty = false;
  // Writes raw bytes to standard output (`export --stdout`, which may be
  // binary). Unset: the bytes go to `out` as UTF-8 text.
  std::function<bool(const QByteArray&)> writeStdout;
  bool macos = false;
  CommandRunner runner = runCommand;
  QDate today = QDate::currentDate();
};

// Runs one zproject-cli command line (without argv[0]).
int runCli(const QStringList& args, CliIo& io, QTextStream& out, QTextStream& err);

// The usage text (`zproject-cli --help`).
QString cliUsage();

}  // namespace zproject::client
