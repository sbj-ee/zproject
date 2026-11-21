// zprojectd: the zproject server (SPEC.md §4). So far it parses its command
// line, validates the listen address and can create or migrate the database
// (--migrate-only); the HTTP API and the other admin commands follow in
// phase 1a.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#include "core/Version.hpp"
#include "server/ServerConfig.hpp"
#include "store/Migrations.hpp"
#include "store/SqliteStore.hpp"

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
  const QCommandLineOption dbOpt(QStringLiteral("db"),
                                 QStringLiteral("Database file (default /var/lib/zproject/zproject.db)."),
                                 QStringLiteral("file"), QStringLiteral("/var/lib/zproject/zproject.db"));
  const QCommandLineOption migrateOpt(
      QStringLiteral("migrate-only"),
      QStringLiteral("Create the database or apply pending schema migrations, then exit."));
  parser.addOption(versionOpt);
  parser.addOption(listenOpt);
  parser.addOption(dbOpt);
  parser.addOption(migrateOpt);
  parser.process(app);

  QTextStream out(stdout);
  QTextStream err(stderr);
  if (parser.isSet(versionOpt)) {
    out << zproject::versionLine(QStringLiteral("zprojectd")) << '\n'
        << "apiVersion " << zproject::kApiVersion << '\n'
        << "schemaVersion " << zproject::store::headSchemaVersion() << '\n';
    return 0;
  }

  if (parser.isSet(migrateOpt)) {
    auto store = zproject::store::SqliteStore::open(parser.value(dbOpt));
    if (!store) {
      err << "zprojectd: " << store.error().message << '\n';
      return 1;
    }
    out << "zprojectd: " << parser.value(dbOpt) << " is at schema " << store.value()->schemaVersion()
        << '\n';
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
