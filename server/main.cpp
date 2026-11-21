// zprojectd: the zproject server (SPEC.md §4, §9, §10, §11, §16). Serves
// /v1 and /mcp on 127.0.0.1:8788 behind `tailscale serve`, and has the
// one-shot admin commands of SPEC.md §10.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSocketNotifier>
#include <QTextStream>

#include <csignal>
#include <unistd.h>

#include "core/Version.hpp"
#include "server/Api.hpp"
#include "server/Notifier.hpp"
#include "server/ServerConfig.hpp"
#include "server/http/HttpServer.hpp"
#include "store/Audit.hpp"
#include "store/Backup.hpp"
#include "store/Migrations.hpp"
#include "store/SqliteStore.hpp"

namespace {

using namespace zproject;

int signalPipe[2] = {-1, -1};

void onSignal(int) {
  const char c = 1;
  // Only async-signal-safe calls here.
  [[maybe_unused]] const auto n = ::write(signalPipe[1], &c, 1);
}

server::Notifier::Settings notifierSettings(const server::Config& cfg) {
  server::Notifier::Settings s;
  s.url = cfg.notifyHubUrl;
  s.topic = cfg.notifyTopic;
  s.tokenFile = cfg.notifyTokenFile;
  return s;
}

void alert(const server::Config& cfg, const QString& title, const QString& body, int priority) {
  server::Notifier n(notifierSettings(cfg));
  QString error;
  if (n.enabled() && !n.sendBlocking(title, body, priority, {QStringLiteral("zproject")}, &error)) {
    QTextStream(stderr) << "zprojectd: alert not delivered: " << error << '\n';
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("zprojectd"));
  QCoreApplication::setApplicationVersion(appVersion());

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("zproject server"));
  parser.addHelpOption();
  const QCommandLineOption versionOpt(QStringLiteral("version"), QStringLiteral("Print the version and exit."));
  const QCommandLineOption configOpt(
      QStringLiteral("config"),
      QStringLiteral("Config file (default %1, used if present).").arg(QLatin1String(server::kDefaultConfigPath)),
      QStringLiteral("file"));
  const QCommandLineOption listenOpt(QStringLiteral("listen"),
                                     QStringLiteral("Loopback address to listen on (default 127.0.0.1:%1).")
                                         .arg(server::kDefaultPort),
                                     QStringLiteral("host:port"));
  const QCommandLineOption dbOpt(QStringLiteral("db"),
                                 QStringLiteral("Database file (default /var/lib/zproject/zproject.db)."),
                                 QStringLiteral("file"));
  const QCommandLineOption hostOpt(QStringLiteral("allowed-host"),
                                   QStringLiteral("Also accept this Host header (repeatable)."),
                                   QStringLiteral("name"));
  const QCommandLineOption backupDirOpt(QStringLiteral("backup-dir"),
                                        QStringLiteral("Backup directory (default /var/lib/zproject/backups)."),
                                        QStringLiteral("dir"));
  const QCommandLineOption migrateOpt(QStringLiteral("migrate-only"),
                                      QStringLiteral("Create the database or apply pending migrations, then exit."));
  const QCommandLineOption ownerOpt(QStringLiteral("create-owner-token"),
                                    QStringLiteral("Create an owner (admin) token and print it once."));
  const QCommandLineOption nameOpt(QStringLiteral("name"), QStringLiteral("Name for --create-owner-token."),
                                   QStringLiteral("name"), QStringLiteral("owner"));
  const QCommandLineOption backupOpt(QStringLiteral("backup"),
                                     QStringLiteral("Take a backup now (VACUUM INTO + integrity check + JSON "
                                                    "export + retention), then exit."));
  const QCommandLineOption restoreOpt(QStringLiteral("restore"),
                                      QStringLiteral("Restore the database from FILE (stop the service first)."),
                                      QStringLiteral("file"));
  const QCommandLineOption verifyOpt(QStringLiteral("verify-audit"),
                                     QStringLiteral("Check the audit log's hash chain, then exit."));
  const QCommandLineOption notifyTestOpt(QStringLiteral("notify-test"),
                                         QStringLiteral("Send a test alert to the notification hub, then exit."));
  parser.addOptions({versionOpt, configOpt, listenOpt, dbOpt, hostOpt, backupDirOpt, migrateOpt, ownerOpt, nameOpt,
                     backupOpt, restoreOpt, verifyOpt, notifyTestOpt});
  parser.process(app);

  QTextStream out(stdout);
  QTextStream err(stderr);
  if (parser.isSet(versionOpt)) {
    out << versionLine(QStringLiteral("zprojectd")) << '\n'
        << "apiVersion " << kApiVersion << '\n'
        << "schemaVersion " << store::headSchemaVersion() << '\n';
    return 0;
  }

  server::Config cfg;
  QString error;
  const bool explicitConfig = parser.isSet(configOpt);
  const QString configPath = explicitConfig ? parser.value(configOpt) : QString::fromLatin1(server::kDefaultConfigPath);
  if (!server::loadConfig(configPath, explicitConfig, &cfg, &error)) {
    err << "zprojectd: " << error << '\n';
    return 2;
  }
  if (parser.isSet(listenOpt) && !server::parseListenAddress(parser.value(listenOpt), &cfg.listen, &error)) {
    err << "zprojectd: " << error << '\n';
    return 2;
  }
  if (parser.isSet(dbOpt)) cfg.database = parser.value(dbOpt);
  if (parser.isSet(backupDirOpt)) cfg.backupDir = parser.value(backupDirOpt);
  for (const QString& h : parser.values(hostOpt)) cfg.allowedHosts.append(h.trimmed().toLower());

  if (parser.isSet(notifyTestOpt)) {
    server::Notifier n(notifierSettings(cfg));
    if (!n.enabled()) {
      err << "zprojectd: alerts are off (no alerts/notify_hub_url in the config)\n";
      return 1;
    }
    if (!n.sendBlocking(QStringLiteral("zproject test alert"),
                        QStringLiteral("zprojectd %1 can reach the notification hub.").arg(appVersion()), 2,
                        {QStringLiteral("zproject")}, &error)) {
      err << "zprojectd: " << error << '\n';
      return 1;
    }
    out << "zprojectd: test alert sent\n";
    return 0;
  }

  if (parser.isSet(restoreOpt)) {
    auto kept = store::restore(parser.value(restoreOpt), cfg.database);
    if (!kept) {
      err << "zprojectd: restore failed: " << kept.error().message << '\n';
      return 1;
    }
    out << "zprojectd: restored " << cfg.database << " from " << parser.value(restoreOpt) << '\n';
    if (!kept->isEmpty()) out << "zprojectd: the previous database is kept as " << kept.value() << '\n';
    return 0;
  }

  // Snapshot before any schema migration (SPEC.md §16.3).
  if (auto snap = store::preMigrationSnapshot(cfg.database, cfg.backupDir); !snap) {
    err << "zprojectd: pre-migration snapshot failed: " << snap.error().message << '\n';
    return 1;
  } else if (!snap->isEmpty()) {
    err << "zprojectd: pre-migration snapshot " << snap.value() << '\n';
  }

  auto opened = store::SqliteStore::open(cfg.database);
  if (!opened) {
    err << "zprojectd: " << opened.error().message << '\n';
    return 1;
  }
  store::SqliteStore& db = *opened.value();

  if (parser.isSet(migrateOpt)) {
    out << "zprojectd: " << cfg.database << " is at schema " << db.schemaVersion() << '\n';
    return 0;
  }
  if (parser.isSet(verifyOpt)) {
    store::AuditLog audit(db.database());
    auto n = audit.verify();
    if (!n) {
      err << "zprojectd: AUDIT CHAIN BROKEN: " << n.error().message << '\n';
      alert(cfg, QStringLiteral("zproject audit chain broken"), n.error().message, 5);
      return 1;
    }
    out << "zprojectd: audit chain intact (" << n.value() << " rows)\n";
    return 0;
  }
  if (parser.isSet(backupOpt)) {
    auto r = store::backup(db, cfg.backupDir, QDate::currentDate());
    if (!r) {
      err << "zprojectd: backup failed: " << r.error().message << '\n';
      alert(cfg, QStringLiteral("zproject backup failed"), r.error().message, 4);
      return 1;
    }
    out << "zprojectd: wrote " << r->databaseFile << " and " << r->exportFile << '\n';
    for (const QString& f : r->removed) out << "zprojectd: pruned " << f << '\n';
    return 0;
  }

  server::ApiOptions options;
  options.allowedHosts = server::effectiveAllowedHosts(cfg);
  options.backupDir = cfg.backupDir;
  server::Api api(db, options);

  if (parser.isSet(ownerOpt)) {
    auto token = api.createOwnerToken(parser.value(nameOpt));
    if (!token) {
      err << "zprojectd: " << token.error().message << '\n';
      return 1;
    }
    err << "zprojectd: owner token created. It is shown once; keep it in a keychain, never in a file, "
           "chat or repository.\n";
    out << token.value() << '\n';
    return 0;
  }

  server::http::Server http([&api](const server::http::Request& req) {
    QElapsedTimer t;
    t.start();
    server::http::Response r = api.handle(req);
    QByteArray rid;
    for (const auto& h : r.headers) {
      if (h.first == "X-Request-Id") rid = h.second;
    }
    // Never the query string, headers or body: no secrets in logs.
    QTextStream(stderr) << req.method << ' ' << req.path << ' ' << r.status << ' ' << t.elapsed() << "ms rid="
                        << rid << '\n';
    return r;
  });
  if (!http.listen(cfg.listen.host, cfg.listen.port)) {
    err << "zprojectd: cannot listen on " << cfg.listen.host.toString() << ':' << cfg.listen.port << ": "
        << http.errorString() << '\n';
    return 1;
  }

  if (::pipe(signalPipe) == 0) {
    (void)std::signal(SIGTERM, onSignal);
    (void)std::signal(SIGINT, onSignal);
    auto* notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [&] {
      err << "zprojectd: stopping\n";
      err.flush();
      http.close();
      QCoreApplication::quit();
    });
  }
  err << "zprojectd " << appVersion() << " listening on " << cfg.listen.host.toString() << ':' << http.port()
      << " (schema " << db.schemaVersion() << ", database " << cfg.database << ")\n";
  err.flush();
  return QCoreApplication::exec();
}
