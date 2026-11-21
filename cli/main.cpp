// zproject-cli: thin command-line client of zprojectd's /v1 (SPEC.md §10).
// The commands live in client/Cli.cpp; this file wires them to the process.
#include <QCoreApplication>
#include <QFile>
#include <QProcessEnvironment>
#include <QTextStream>

#include <cstdio>
#include <termios.h>
#include <unistd.h>

#include "client/Cli.hpp"
#include "core/Version.hpp"

namespace {

// Reads one line from the terminal with echo off; null when stdin is not a
// terminal.
QString promptSecret(const QString& prompt) {
  if (::isatty(STDIN_FILENO) == 0) return {};
  QTextStream(stderr) << prompt << Qt::flush;
  termios old {};
  ::tcgetattr(STDIN_FILENO, &old);
  termios quiet = old;
  quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
  ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
  QFile in;
  QByteArray line;
  if (in.open(stdin, QIODevice::ReadOnly)) line = in.readLine(4096);
  ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
  QTextStream(stderr) << '\n' << Qt::flush;
  return QString::fromLatin1(line).trimmed();
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("zproject-cli"));
  QCoreApplication::setApplicationVersion(zproject::appVersion());

  zproject::client::CliIo io;
  const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  for (const QString& k : env.keys()) io.env.insert(k, env.value(k));
  io.readStdin = [] {
    QFile in;
    return in.open(stdin, QIODevice::ReadOnly) ? in.readAll() : QByteArray();
  };
  io.promptSecret = promptSecret;
  io.stdoutIsTty = ::isatty(STDOUT_FILENO) != 0;
#ifdef Q_OS_MACOS
  io.macos = true;
#endif

  QTextStream out(stdout);
  QTextStream err(stderr);
  return zproject::client::runCli(QCoreApplication::arguments().mid(1), io, out, err);
}
