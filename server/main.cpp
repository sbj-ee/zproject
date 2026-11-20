// zprojectd: the zproject server (SPEC.md §4). This skeleton parses its
// command line and validates the listen address; the HTTP API, store and
// admin commands arrive in phase 1a.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#include "core/Version.hpp"
#include "server/ServerConfig.hpp"

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("zprojectd"));
  QCoreApplication::setApplicationVersion(zproject::appVersion());

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("zproject server"));
  parser.addHelpOption();
  const QCommandLineOption versionOpt(QStringLiteral("version"),
                                      QStringLiteral("Print the version and exit."));
  const QCommandLineOption listenOpt(
      QStringLiteral("listen"),
      QStringLiteral("Loopback address to listen on (default 127.0.0.1:%1).")
          .arg(zproject::server::kDefaultPort),
      QStringLiteral("host:port"),
      QStringLiteral("127.0.0.1:%1").arg(zproject::server::kDefaultPort));
  parser.addOption(versionOpt);
  parser.addOption(listenOpt);
  parser.process(app);

  QTextStream out(stdout);
  QTextStream err(stderr);
  if (parser.isSet(versionOpt)) {
    out << zproject::versionLine(QStringLiteral("zprojectd")) << '\n'
        << "apiVersion " << zproject::kApiVersion << '\n';
    return 0;
  }

  zproject::server::ListenAddress listen;
  QString error;
  if (!zproject::server::parseListenAddress(parser.value(listenOpt), &listen, &error)) {
    err << "zprojectd: " << error << '\n';
    return 2;
  }

  err << "zprojectd " << zproject::appVersion()
      << ": the server is not implemented yet in this build (phase 0 skeleton).\n";
  return 1;
}
