// zproject-cli end to end (SPEC.md §10): runCli in process against a real
// zprojectd /v1 (Api + http::Server on a loopback port). Covers login and
// the token stores, exit codes, projects and tasks, dry-run and confirm,
// token creation that never prints to a terminal, and the API-version check.
// Tokens are generated per run; nothing here is real.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

#include "client/ClientConfig.hpp"
#include "client/Cli.hpp"
#include "client/TokenStore.hpp"
#include "core/UpdateSigningKey.hpp"
#include "core/Version.hpp"
#include "server/Api.hpp"
#include "server/http/HttpServer.hpp"
#include "store/SqliteStore.hpp"

using namespace zproject;
using namespace zproject::client;

namespace {

struct Run {
  int code = -1;
  QString out;
  QString err;
  QJsonValue json() const {
    const QJsonDocument d = QJsonDocument::fromJson(out.toUtf8());
    return d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object());
  }
};

int fileMode(const QString& path) {
  struct stat st {};
  if (::stat(QFile::encodeName(path).constData(), &st) != 0) return -1;
  return static_cast<int>(st.st_mode & 0777);
}

// A fake `security` / `secret-tool` keeping one secret in memory, recording
// every call so tests can check the token never reaches argv.
struct FakeKeychain {
  QString secret;
  QList<QPair<QString, QStringList>> calls;
  QList<QByteArray> inputs;

  CommandRunner runner() {
    return [this](const QString& program, const QStringList& args, const QByteArray& input) {
      calls.append({program, args});
      inputs.append(input);
      CommandResult r;
      r.exitCode = 0;
      const QString verb = args.value(0);
      if (verb == QLatin1String("-i")) {  // security -i: command on stdin
        const QList<QByteArray> parts = input.trimmed().split(' ');
        secret = QString::fromLatin1(parts.last());
      } else if (verb == QLatin1String("find-generic-password")) {
        if (secret.isEmpty()) {
          r.exitCode = 44;
        } else {
          r.out = secret.toLatin1() + '\n';
        }
      } else if (verb == QLatin1String("delete-generic-password")) {
        r.exitCode = secret.isEmpty() ? 44 : 0;
        secret.clear();
      } else if (verb == QLatin1String("store")) {  // secret-tool store: secret on stdin
        secret = QString::fromLatin1(input);
      } else if (verb == QLatin1String("lookup")) {
        if (secret.isEmpty()) {
          r.exitCode = 1;
        } else {
          r.out = secret.toLatin1();
        }
      } else if (verb == QLatin1String("clear")) {
        secret.clear();
      }
      return r;
    };
  }
};

}  // namespace

class TestCli : public QObject {
  Q_OBJECT

  std::unique_ptr<store::SqliteStore> store_;
  std::unique_ptr<server::Api> api_;
  std::unique_ptr<server::http::Server> http_;
  QString owner_;
  QString url_;
  std::unique_ptr<QTemporaryDir> dir_;

  CliIo io(const QHash<QString, QString>& extraEnv = {}) {
    CliIo i;
    i.env.insert(QStringLiteral("ZPROJECT_CONFIG_DIR"), dir_->path() + QStringLiteral("/config"));
    i.env.insert(QStringLiteral("ZPROJECT_URL"), url_);
    for (auto it = extraEnv.constBegin(); it != extraEnv.constEnd(); ++it) i.env.insert(it.key(), it.value());
    i.runner = [](const QString&, const QStringList&, const QByteArray&) { return CommandResult{}; };  // no tools
    i.today = QDate(2026, 11, 23);
    return i;
  }

  Run cli(const QStringList& args, CliIo i) {
    QString o;
    QString e;
    QTextStream out(&o);
    QTextStream err(&e);
    Run r;
    r.code = runCli(args, i, out, err);
    r.out = o;
    r.err = e;
    return r;
  }
  Run cli(const QStringList& args) { return cli(args, io()); }
  Run cliAs(const QString& token, const QStringList& args) {
    return cli(args, io({{QStringLiteral("ZPROJECT_TOKEN"), token}}));
  }
  Run loginOwner() {
    CliIo i = io();
    const QString t = owner_;
    i.readStdin = [t] { return t.toLatin1() + '\n'; };
    return cli({QStringLiteral("login"), QStringLiteral("--stdin")}, i);
  }
  QString createProject(const QString& name) {
    const Run r = cliAs(owner_, {QStringLiteral("--json"), QStringLiteral("projects"), QStringLiteral("create"), name});
    if (r.code != 0) qFatal("create project: %s", qPrintable(r.err));
    return r.json().toObject().value(QStringLiteral("id")).toString();
  }
  QString addTask(const QString& project, const QString& title, const QStringList& extra = {}) {
    const Run r = cliAs(owner_, QStringList{QStringLiteral("--json"), QStringLiteral("tasks"), QStringLiteral("add"),
                                            project, title} + extra);
    if (r.code != 0) qFatal("add task: %s", qPrintable(r.err));
    return r.json().toObject().value(QStringLiteral("id")).toString();
  }
  QString newToken(const QStringList& args) {
    const QString path = dir_->path() + QStringLiteral("/t-%1").arg(++tokenFiles_);
    const Run r = cliAs(owner_, QStringList{QStringLiteral("token"), QStringLiteral("create"), QStringLiteral("--out-file"),
                                            path} + args);
    if (r.code != 0) qFatal("token create: %s", qPrintable(r.err));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) qFatal("cannot read %s", qPrintable(path));
    return QString::fromLatin1(f.readAll()).trimmed();
  }
  int tokenFiles_ = 0;

 private slots:
  void init() {
    dir_ = std::make_unique<QTemporaryDir>();
    store_ = std::move(store::SqliteStore::open(QStringLiteral(":memory:"))).value();
    http_ = std::make_unique<server::http::Server>([this](const server::http::Request& r) { return api_->handle(r); });
    QVERIFY(http_->listen(QHostAddress::LocalHost, 0));
    server::ApiOptions o;
    o.allowedHosts = {QStringLiteral("127.0.0.1:%1").arg(http_->port())};
    api_ = std::make_unique<server::Api>(*store_, o);
    owner_ = api_->createOwnerToken(QStringLiteral("owner")).value();
    url_ = QStringLiteral("http://127.0.0.1:%1").arg(http_->port());
  }
  void cleanup() {
    http_.reset();
    api_.reset();
    store_.reset();
    dir_.reset();
  }

  void serverUrls() {
    QVERIFY(parseServerUrl(QStringLiteral("https://server-host.example.ts.net")).ok());
    QCOMPARE(parseServerUrl(QStringLiteral("https://server-host.example.ts.net/")).value().toString(),
             QStringLiteral("https://server-host.example.ts.net"));
    QVERIFY(parseServerUrl(QStringLiteral("http://127.0.0.1:8788")).ok());
    QVERIFY(parseServerUrl(QStringLiteral("http://localhost:8788")).ok());
    QVERIFY(parseServerUrl(QStringLiteral("http://[::1]:8788")).ok());
    QVERIFY(!parseServerUrl(QStringLiteral("http://server-host.example.ts.net")).ok());  // clear text off-host
    QVERIFY(!parseServerUrl(QStringLiteral("http://192.0.2.10:8788")).ok());
    QVERIFY(!parseServerUrl(QStringLiteral("https://user:pw@server-host.example.ts.net")).ok());
    QVERIFY(!parseServerUrl(QStringLiteral("https://server-host.example.ts.net/v1")).ok());
    QVERIFY(!parseServerUrl(QStringLiteral("ftp://server-host.example.ts.net")).ok());
    QVERIFY(!parseServerUrl(QString()).ok());
  }

  void usageAndExitCodes() {
    QCOMPARE(cli({QStringLiteral("--version")}).out, QStringLiteral("zproject-cli %1\n").arg(appVersion()));
    QCOMPARE(cli({QStringLiteral("--help")}).code, 0);
    QCOMPARE(cli({}).code, int(kExitUsage));
    QCOMPARE(cli({QStringLiteral("frobnicate")}).code, int(kExitUsage));
    QCOMPARE(cli({QStringLiteral("projects"), QStringLiteral("list"), QStringLiteral("--bogus")}).code, int(kExitUsage));
    QCOMPARE(cli({QStringLiteral("projects"), QStringLiteral("list"), QStringLiteral("--milestone")}).code, int(kExitUsage));
    QCOMPARE(cli({QStringLiteral("tasks"), QStringLiteral("show"), QStringLiteral("not-an-id")}).code, int(kExitUsage));
    // No token anywhere.
    Run r = cli({QStringLiteral("projects"), QStringLiteral("list")});
    QCOMPARE(r.code, int(kExitAuth));
    QVERIFY(r.err.contains(QStringLiteral("zproject-cli login")));
    // No URL.
    CliIo noUrl = io();
    noUrl.env.remove(QStringLiteral("ZPROJECT_URL"));
    QCOMPARE(cli({QStringLiteral("status")}, noUrl).code, int(kExitUsage));
    // Unreachable server.
    CliIo dead = io({{QStringLiteral("ZPROJECT_URL"), QStringLiteral("http://127.0.0.1:1")},
                     {QStringLiteral("ZPROJECT_TOKEN"), owner_}});
    QCOMPARE(cli({QStringLiteral("projects"), QStringLiteral("list")}, dead).code, int(kExitError));
  }

  void loginStoresTokenInA0600File() {
    Run r = loginOwner();
    QCOMPARE(r.code, 0);
    QVERIFY2(!r.out.contains(owner_.mid(owner_.indexOf(QLatin1Char('.')) + 1)), "secret printed");
    QVERIFY(r.out.contains(tokenLabel(owner_)));
    const QString config = dir_->path() + QStringLiteral("/config");
    QCOMPARE(fileMode(config + QStringLiteral("/token")), 0600);
    QCOMPARE(fileMode(config), 0700);
    QCOMPARE(fileMode(config + QStringLiteral("/cli.ini")), 0600);
    QSettings ini(config + QStringLiteral("/cli.ini"), QSettings::IniFormat);
    QCOMPARE(ini.value(QStringLiteral("server/url")).toString(), url_);
    QCOMPARE(ini.value(QStringLiteral("auth/store")).toString(), QStringLiteral("file"));
    // The URL is remembered: no ZPROJECT_URL needed now.
    CliIo i = io();
    i.env.remove(QStringLiteral("ZPROJECT_URL"));
    r = cli({QStringLiteral("status")}, i);
    QCOMPARE(r.code, 0);
    QVERIFY(r.out.contains(QStringLiteral("auth:    ok")));
    QVERIFY(r.out.contains(QStringLiteral("cli.ini")));

    // A token file others can read is refused.
    QVERIFY(::chmod(QFile::encodeName(config + QStringLiteral("/token")).constData(), 0644) == 0);
    r = cli({QStringLiteral("projects"), QStringLiteral("list")});
    QCOMPARE(r.code, int(kExitAuth));
    QVERIFY(r.err.contains(QStringLiteral("chmod 600")));

    QCOMPARE(cli({QStringLiteral("logout")}).code, 0);
    QVERIFY(!QFile::exists(config + QStringLiteral("/token")));
  }

  void loginRefusesBadTokens() {
    CliIo i = io();
    i.readStdin = [] { return QByteArray("hunter2\n"); };
    QCOMPARE(cli({QStringLiteral("login"), QStringLiteral("--stdin")}, i).code, int(kExitUsage));
    QString wrong = owner_;
    wrong[wrong.size() - 1] = wrong.endsWith(QLatin1Char('A')) ? QLatin1Char('B') : QLatin1Char('A');
    i.readStdin = [wrong] { return wrong.toLatin1(); };
    QCOMPARE(cli({QStringLiteral("login"), QStringLiteral("--stdin")}, i).code, int(kExitAuth));
    QVERIFY(!QFile::exists(dir_->path() + QStringLiteral("/config/token")));
    // No terminal and no --stdin.
    i.promptSecret = [](const QString&) { return QString(); };
    QCOMPARE(cli({QStringLiteral("login")}, i).code, int(kExitUsage));
    // From a file (as delivered from the server host).
    const QString f = dir_->path() + QStringLiteral("/delivered.token");
    QFile out(f);
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(owner_.toLatin1());
    out.close();
    const Run r = cli({QStringLiteral("login"), QStringLiteral("--from-file"), f});
    QCOMPARE(r.code, 0);
    QVERIFY(r.out.contains(QStringLiteral("You can now delete")));
  }

  void macosKeychainNeverSeesTokenInArgv() {
    FakeKeychain kc;
    CliIo i = io();
    i.macos = true;
    i.runner = kc.runner();
    const QString t = owner_;
    i.readStdin = [t] { return t.toLatin1(); };
    QCOMPARE(cli({QStringLiteral("login"), QStringLiteral("--stdin")}, i).code, 0);  // keychain is the macOS default
    QCOMPARE(kc.secret, owner_);
    for (const auto& c : kc.calls) {
      QVERIFY2(!c.second.join(QLatin1Char(' ')).contains(owner_), "token on a command line");
      QCOMPARE(c.first, QStringLiteral("/usr/bin/security"));
    }
    QVERIFY(!QFile::exists(dir_->path() + QStringLiteral("/config/token")));
    i.readStdin = nullptr;
    Run r = cli({QStringLiteral("status")}, i);
    QCOMPARE(r.code, 0);
    QVERIFY(r.out.contains(QStringLiteral("(keychain)")));
    QCOMPARE(cli({QStringLiteral("logout")}, i).code, 0);
    QVERIFY(kc.secret.isEmpty());
    QCOMPARE(cli({QStringLiteral("status")}, i).code, int(kExitAuth));
  }

  void linuxSecretToolStore() {
    FakeKeychain kc;
    CliIo i = io();
    i.runner = kc.runner();
    const QString t = owner_;
    i.readStdin = [t] { return t.toLatin1(); };
    QCOMPARE(cli({QStringLiteral("login"), QStringLiteral("--stdin"), QStringLiteral("--store"), QStringLiteral("keychain")}, i)
                 .code,
             0);
    QCOMPARE(kc.secret, owner_);
    QCOMPARE(kc.calls.first().first, QStringLiteral("secret-tool"));
    QCOMPARE(kc.calls.first().second.first(), QStringLiteral("store"));
    QVERIFY(!kc.calls.first().second.join(QLatin1Char(' ')).contains(owner_));
    QCOMPARE(cli({QStringLiteral("projects"), QStringLiteral("list")}, i).code, 0);
    // secret-tool missing: a clear message, not a crash.
    i.runner = [](const QString&, const QStringList&, const QByteArray&) { return CommandResult{}; };
    const Run r = cli({QStringLiteral("projects"), QStringLiteral("list")}, i);
    QCOMPARE(r.code, int(kExitAuth));
    QVERIFY(r.err.contains(QStringLiteral("libsecret-tools")));
  }

  void projectsAndTasks() {
    const QString p = createProject(QStringLiteral("Garage"));
    Run r = cliAs(owner_, {QStringLiteral("projects"), QStringLiteral("list")});
    QCOMPARE(r.code, 0);
    QVERIFY(r.out.startsWith(QStringLiteral("ID")));
    QVERIFY(r.out.contains(QStringLiteral("Garage")));
    QCOMPARE(cliAs(owner_, {QStringLiteral("projects"), QStringLiteral("update"), p, QStringLiteral("--rag"),
                            QStringLiteral("amber"), QStringLiteral("--rev"), QStringLiteral("1")})
                 .code,
             0);
    r = cliAs(owner_, {QStringLiteral("projects"), QStringLiteral("update"), p, QStringLiteral("--rag"),
                       QStringLiteral("red"), QStringLiteral("--rev"), QStringLiteral("1")});
    QCOMPARE(r.code, int(kExitConflict));  // 412
    QVERIFY(r.err.contains(QStringLiteral("412")));
    QCOMPARE(cliAs(owner_, {QStringLiteral("projects"), QStringLiteral("update"), p, QStringLiteral("--start"),
                            QStringLiteral("23/11/2026")})
                 .code,
             int(kExitUsage));
    r = cliAs(owner_, {QStringLiteral("projects"), QStringLiteral("show"), p});
    QVERIFY(r.out.contains(QStringLiteral("rag:")) && r.out.contains(QStringLiteral("amber")));

    const QString a = addTask(p, QStringLiteral("Foundation"));
    const QString a1 = addTask(p, QStringLiteral("Dig"), {QStringLiteral("--parent"), a, QStringLiteral("--duration"),
                                                          QStringLiteral("2")});
    addTask(p, QStringLiteral("Inspection"), {QStringLiteral("--milestone"), QStringLiteral("--deadline"),
                                               QStringLiteral("2026-12-01")});
    r = cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("list"), p});
    QCOMPARE(r.code, 0);
    const QStringList lines = r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 4);
    QVERIFY(lines[1].startsWith(QStringLiteral("1 ")));
    QVERIFY(lines[2].startsWith(QStringLiteral("1.1")));
    QVERIFY(lines[2].endsWith(QStringLiteral("  Dig")));
    QVERIFY(lines[3].contains(QStringLiteral("2026-12-01")));

    // Dry run writes nothing.
    r = cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("add"), p, QStringLiteral("Ghost"), QStringLiteral("--dry-run")});
    QCOMPARE(r.code, 0);
    QVERIFY(r.out.startsWith(QStringLiteral("Dry run: 1 change")));
    r = cliAs(owner_, {QStringLiteral("--json"), QStringLiteral("tasks"), QStringLiteral("list"), p,
                       QStringLiteral("--search"), QStringLiteral("ghost")});
    QCOMPARE(r.json().toArray().size(), 0);

    // done: status, 100 %, actual dates.
    r = cliAs(owner_, {QStringLiteral("--json"), QStringLiteral("tasks"), QStringLiteral("done"), a1});
    QCOMPARE(r.code, 0);
    QJsonObject t = r.json().toObject();
    QCOMPARE(t.value(QStringLiteral("status")).toString(), QStringLiteral("done"));
    QCOMPARE(t.value(QStringLiteral("percentComplete")).toInt(), 100);
    QCOMPARE(t.value(QStringLiteral("actualFinish")).toString(), QStringLiteral("2026-11-23"));
    // Dig was Foundation's only child, so Foundation rolls up to done too.
    r = cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("list"), p, QStringLiteral("--status"), QStringLiteral("done")});
    QCOMPARE(r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size(), 3);
    // A summary's progress is derived: setting it is a validation error.
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("update"), a, QStringLiteral("--percent"),
                            QStringLiteral("50"), QStringLiteral("--actual-start"), QStringLiteral("2026-11-20")})
                 .code,
             int(kExitConflict));
    // Validation errors are exit 3.
    const QString b = addTask(p, QStringLiteral("Framing"));
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("update"), b, QStringLiteral("--percent"),
                            QStringLiteral("50")})
                 .code,
             int(kExitConflict));  // needs actualStart
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("update"), b, QStringLiteral("--percent"),
                            QStringLiteral("50"), QStringLiteral("--actual-start"), QStringLiteral("2026-11-20")})
                 .code,
             0);
    r = cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("history"), b});
    QCOMPARE(r.code, 0);
    QCOMPARE(r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size(), 3);  // header, create, update
    // Foundation's history shows the roll-up as its own change.
    r = cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("history"), a});
    QCOMPARE(r.code, 0);
    QCOMPARE(r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size(), 3);  // header, create, roll-up

    // Owner deletes need no confirmation; the subtree goes too.
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("delete"), a}).code, 0);
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("show"), a1}).code, int(kExitError));  // 404
    QCOMPARE(cliAs(owner_, {QStringLiteral("audit"), QStringLiteral("--limit"), QStringLiteral("5")}).code, 0);
  }

  void tasksMove() {
    const QString p = createProject(QStringLiteral("Garage"));
    const QString a = addTask(p, QStringLiteral("Foundation"));
    const QString b = addTask(p, QStringLiteral("Framing"));
    const QString c = addTask(p, QStringLiteral("Roof"));
    auto list = [&] {
      return cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("list"), p}).out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    };
    // Roof first: --position counts from 1.
    Run r = cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("move"), c, QStringLiteral("--position"), QStringLiteral("1"),
                           QStringLiteral("--dry-run")});
    QCOMPARE(r.code, 0);
    QVERIFY(r.out.startsWith(QStringLiteral("Dry run: 3 changes")));
    QVERIFY(r.out.contains(QStringLiteral("move task ") + c));
    QVERIFY(list()[1].endsWith(QStringLiteral("Foundation")));
    r = cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("move"), c, QStringLiteral("--position"), QStringLiteral("1"),
                       QStringLiteral("--rev"), QStringLiteral("1")});
    QCOMPARE(r.code, 0);
    QVERIFY(r.out.startsWith(QStringLiteral("Moved task ") + c + QStringLiteral(" (rev 2): Roof")));
    QVERIFY(list()[1].endsWith(QStringLiteral("Roof")));
    // Under Foundation, then back to the top level.
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("move"), b, QStringLiteral("--parent"), a}).code, 0);
    QVERIFY(list()[3].startsWith(QStringLiteral("2.1")));
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("move"), b, QStringLiteral("--parent"), QStringLiteral("none")}).code, 0);
    QVERIFY(list()[3].startsWith(QStringLiteral("3 ")));
    // Usage errors, a cycle, a stale revision.
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("move"), b}).code, int(kExitUsage));
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("move"), b, QStringLiteral("--position"), QStringLiteral("0")}).code,
             int(kExitUsage));
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("move"), b, QStringLiteral("--parent"), QStringLiteral("x")}).code,
             int(kExitUsage));
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("move"), b, QStringLiteral("--title"), QStringLiteral("x")}).code,
             int(kExitUsage));
    const QString kid = addTask(p, QStringLiteral("Slab"), {QStringLiteral("--parent"), a});
    r = cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("move"), a, QStringLiteral("--parent"), kid});
    QCOMPARE(r.code, int(kExitConflict));
    QVERIFY(r.err.contains(QStringLiteral("409")));
    QCOMPARE(cliAs(owner_, {QStringLiteral("tasks"), QStringLiteral("move"), a, QStringLiteral("--position"), QStringLiteral("2"),
                            QStringLiteral("--rev"), QStringLiteral("1")})
                 .code,
             int(kExitConflict));  // 412
  }

  void tokenCreateNeverPrintsToATerminal() {
    const QString p = createProject(QStringLiteral("One"));
    const QString path = dir_->path() + QStringLiteral("/laptop.token");
    // Must say where the token goes.
    QCOMPARE(cliAs(owner_, {QStringLiteral("token"), QStringLiteral("create"), QStringLiteral("--name"),
                            QStringLiteral("laptop"), QStringLiteral("--kind"), QStringLiteral("device")})
                 .code,
             int(kExitUsage));
    // Never to a terminal.
    CliIo tty = io({{QStringLiteral("ZPROJECT_TOKEN"), owner_}});
    tty.stdoutIsTty = true;
    QCOMPARE(cli({QStringLiteral("token"), QStringLiteral("create"), QStringLiteral("--name"), QStringLiteral("laptop"),
                  QStringLiteral("--kind"), QStringLiteral("device"), QStringLiteral("--stdout")},
                 tty)
                 .code,
             int(kExitUsage));

    Run r = cliAs(owner_, {QStringLiteral("token"), QStringLiteral("create"), QStringLiteral("--name"),
                           QStringLiteral("laptop"), QStringLiteral("--kind"), QStringLiteral("device"),
                           QStringLiteral("--out-file"), path});
    QCOMPARE(r.code, 0);
    QVERIFY(!r.out.contains(QStringLiteral("zpt_") + QStringLiteral(".")));
    QCOMPARE(fileMode(path), 0600);
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString device = QString::fromLatin1(f.readAll()).trimmed();
    QVERIFY(looksLikeToken(device));
    QVERIFY(!r.out.contains(device));
    QVERIFY(!r.out.contains(device.mid(device.indexOf(QLatin1Char('.')))));
    // An existing file is never overwritten.
    QCOMPARE(cliAs(owner_, {QStringLiteral("token"), QStringLiteral("create"), QStringLiteral("--name"),
                            QStringLiteral("x"), QStringLiteral("--kind"), QStringLiteral("device"),
                            QStringLiteral("--out-file"), path})
                 .code,
             int(kExitUsage));
    // A refused token leaves no file behind.
    const QString bad = dir_->path() + QStringLiteral("/bad.token");
    QCOMPARE(cliAs(owner_, {QStringLiteral("token"), QStringLiteral("create"), QStringLiteral("--name"),
                            QStringLiteral("x"), QStringLiteral("--kind"), QStringLiteral("agent")})
                     .code == int(kExitUsage),
             true);
    QCOMPARE(cliAs(owner_, {QStringLiteral("token"), QStringLiteral("create"), QStringLiteral("--name"),
                            QStringLiteral("x"), QStringLiteral("--kind"), QStringLiteral("agent"),
                            QStringLiteral("--out-file"), bad})
                 .code,
             int(kExitConflict));  // agents need --projects
    QVERIFY(!QFile::exists(bad));
    // --stdout into a pipe: only the token on stdout, details on stderr.
    r = cliAs(owner_, {QStringLiteral("token"), QStringLiteral("create"), QStringLiteral("--name"),
                       QStringLiteral("agent-1"), QStringLiteral("--kind"), QStringLiteral("agent"),
                       QStringLiteral("--projects"), p, QStringLiteral("--stdout")});
    QCOMPARE(r.code, 0);
    QVERIFY(looksLikeToken(r.out.trimmed()));
    QVERIFY(r.err.contains(QStringLiteral("agent token")));

    // The device token works; listing shows no secrets.
    QCOMPARE(cliAs(device, {QStringLiteral("projects"), QStringLiteral("list")}).code, 0);
    r = cliAs(owner_, {QStringLiteral("token"), QStringLiteral("list")});
    QVERIFY(r.out.contains(QStringLiteral("laptop")));
    QVERIFY(!r.out.contains(QStringLiteral(".")) || !r.out.contains(device));
    // Revoke: the device token stops working.
    const QString id = device.mid(4, 20);
    QCOMPARE(cliAs(owner_, {QStringLiteral("token"), QStringLiteral("revoke"), id}).code, 0);
    QCOMPARE(cliAs(device, {QStringLiteral("projects"), QStringLiteral("list")}).code, int(kExitAuth));
  }

  void agentScopesAndConfirm() {
    const QString p = createProject(QStringLiteral("One"));
    const QString t = addTask(p, QStringLiteral("doomed"));
    const QString reader = newToken({QStringLiteral("--name"), QStringLiteral("reader"), QStringLiteral("--kind"),
                                     QStringLiteral("agent"), QStringLiteral("--projects"), p});
    Run r = cliAs(reader, {QStringLiteral("tasks"), QStringLiteral("add"), p, QStringLiteral("x")});
    QCOMPARE(r.code, int(kExitAuth));  // 403 forbidden_scope
    QCOMPARE(cliAs(reader, {QStringLiteral("token"), QStringLiteral("list")}).code, int(kExitAuth));

    const QString writer = newToken({QStringLiteral("--name"), QStringLiteral("writer"), QStringLiteral("--kind"),
                                     QStringLiteral("agent"), QStringLiteral("--projects"), p, QStringLiteral("--scopes"),
                                     QStringLiteral("write")});
    r = cliAs(writer, {QStringLiteral("tasks"), QStringLiteral("delete"), t});
    QCOMPARE(r.code, int(kExitConflict));
    QVERIFY(r.err.contains(QStringLiteral("--dry-run")));
    r = cliAs(writer, {QStringLiteral("--json"), QStringLiteral("tasks"), QStringLiteral("delete"), t,
                       QStringLiteral("--dry-run")});
    QCOMPARE(r.code, 0);
    const QString confirm = r.json().toObject().value(QStringLiteral("confirmId")).toString();
    QVERIFY(!confirm.isEmpty());
    r = cliAs(writer, {QStringLiteral("tasks"), QStringLiteral("delete"), t, QStringLiteral("--dry-run")});
    QVERIFY(r.out.contains(QStringLiteral("--confirm ")));
    QCOMPARE(cliAs(writer, {QStringLiteral("tasks"), QStringLiteral("delete"), t, QStringLiteral("--confirm"), confirm}).code,
             0);
    QCOMPARE(cliAs(writer, {QStringLiteral("tasks"), QStringLiteral("delete"), t, QStringLiteral("--confirm"),
                            QStringLiteral("x"), QStringLiteral("--dry-run")})
                 .code,
             int(kExitUsage));
  }

  void rateLimitIsExit5() {
    const QString p = createProject(QStringLiteral("One"));
    // rateLimit is not a CLI option yet; create it through the API.
    server::http::Request req;
    req.method = "POST";
    req.path = QStringLiteral("/v1/tokens");
    req.headers = {{"host", QStringLiteral("127.0.0.1:%1").arg(http_->port()).toLatin1()},
                   {"authorization", "Bearer " + owner_.toLatin1()},
                   {"content-type", "application/json"}};
    req.body = QJsonDocument(QJsonObject{{QStringLiteral("name"), QStringLiteral("slow")},
                                         {QStringLiteral("kind"), QStringLiteral("agent")},
                                         {QStringLiteral("projects"), QJsonArray{p}},
                                         {QStringLiteral("rateLimit"), QJsonObject{{QStringLiteral("read"), 1}}}})
                   .toJson();
    const auto resp = api_->handle(req);
    QCOMPARE(resp.status, 201);
    const QString slow = QJsonDocument::fromJson(resp.body).object().value(QStringLiteral("token")).toString();
    QCOMPARE(cliAs(slow, {QStringLiteral("projects"), QStringLiteral("list")}).code, 0);
    const Run r = cliAs(slow, {QStringLiteral("projects"), QStringLiteral("list")});
    QCOMPARE(r.code, int(kExitRateLimited));
    QVERIFY(r.err.contains(QStringLiteral("Retry after")));
  }

  void refusesAnotherApiVersion() {
    server::http::Server fake([](const server::http::Request&) {
      return server::http::Response::json(
          200, QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("version"), QStringLiteral("9.0.0")},
                           {QStringLiteral("apiVersion"), 2}, {QStringLiteral("schemaVersion"), 9}});
    });
    QVERIFY(fake.listen(QHostAddress::LocalHost, 0));
    const Run r = cli({QStringLiteral("projects"), QStringLiteral("list")},
                      io({{QStringLiteral("ZPROJECT_URL"), QStringLiteral("http://127.0.0.1:%1").arg(fake.port())},
                          {QStringLiteral("ZPROJECT_TOKEN"), owner_}}));
    QCOMPARE(r.code, int(kExitError));
    QVERIFY(r.err.contains(QStringLiteral("API v2")));
  }

  void releaseSigningKeyIsAMinisignKey() {
    // The release workflow greps this constant out of core/UpdateSigningKey.cpp.
    const QByteArray key(kUpdateSigningPublicKey);
    QCOMPARE(key.size(), 56);
    QVERIFY(key.startsWith("RW"));
    const QByteArray raw = QByteArray::fromBase64(key);
    QCOMPARE(raw.size(), 42);  // "Ed" + key id (8) + public key (32)
    QCOMPARE(raw.left(2), QByteArray("Ed"));
  }

  void doctorReportsTls() {
    const Run r = cli({QStringLiteral("--json"), QStringLiteral("doctor")});
    const QJsonObject o = r.json().toObject();
    QCOMPARE(o.value(QStringLiteral("version")).toString(), appVersion());
    QCOMPARE(r.code, o.value(QStringLiteral("tls")).toBool() ? 0 : 1);
  }
};

QTEST_GUILESS_MAIN(TestCli)
#include "tst_cli.moc"
