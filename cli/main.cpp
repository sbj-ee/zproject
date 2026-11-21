// zproject-cli: thin command-line client of /v1 (SPEC.md §10). The
// subcommands arrive with the server API in phase 1a.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#include "core/Version.hpp"

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("zproject-cli"));
  QCoreApplication::setApplicationVersion(zproject::appVersion());

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("zproject command-line client"));
  parser.addHelpOption();
  const QCommandLineOption versionOpt(QStringLiteral("version"),
                                      QStringLiteral("Print the version and exit."));
  parser.addOption(versionOpt);
  parser.process(app);

  if (parser.isSet(versionOpt)) {
    QTextStream(stdout) << zproject::versionLine(QStringLiteral("zproject-cli")) << '\n';
    return 0;
  }
  QTextStream(stderr) << "zproject-cli: no commands yet in this build (phase 0 skeleton).\n";
  return 2;  // usage (SPEC.md §10 exit codes)
}
