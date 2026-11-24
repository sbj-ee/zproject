// zproject-cli: thin command-line client of zprojectd's /v1 (SPEC.md §10).
// The commands live in client/Cli.cpp; this file wires them to the process.
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
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

// macOS: zproject-cli lives in zproject.app/Contents/MacOS next to the GUI.
// Qt finds the bundle's qt.conf (and so Contents/PlugIns) only for the
// bundle's main executable, so the CLI would look for its TLS backend in
// Contents/MacOS/tls and, outside a machine with Homebrew Qt, find none
// (no https). Point it at the bundle's PlugIns before any network use.
void addBundlePlugins() {
#ifdef Q_OS_MACOS
  const QDir exeDir(QCoreApplication::applicationDirPath());
  const QString plugins = QDir::cleanPath(exeDir.absoluteFilePath(QStringLiteral("../PlugIns")));
  if (exeDir.dirName() == QLatin1String("MacOS") && QFileInfo(plugins).isDir()) {
    QCoreApplication::addLibraryPath(plugins);
  }
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("zproject-cli"));
  QCoreApplication::setApplicationVersion(zproject::appVersion());
  addBundlePlugins();

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
  io.writeStdout = [&out](const QByteArray& bytes) {
    out.flush();
    const bool ok = std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout) ==
                    static_cast<size_t>(bytes.size());
    return std::fflush(stdout) == 0 && ok;
  };
  return zproject::client::runCli(QCoreApplication::arguments().mid(1), io, out, err);
}
