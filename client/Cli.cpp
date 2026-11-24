// zproject-cli commands (SPEC.md §10): a thin client of /v1.
#include "client/Cli.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSysInfo>
#include <QUrlQuery>
#include <QtNetwork/qtnetworkglobal.h>
#if QT_CONFIG(ssl)
#include <QSslSocket>
#endif

#include <cerrno>
#include <fcntl.h>
#include <array>
#include <optional>
#include <unistd.h>

#include "client/ApiClient.hpp"
#include "client/ClientConfig.hpp"
#include "core/Export.hpp"
#include "core/LinkNotation.hpp"
#include "core/Model.hpp"
#include "core/Version.hpp"

namespace zproject::client {

QString cliUsage() {
  return QStringLiteral(
      "Usage: zproject-cli [--url URL] [--json] COMMAND ...\n"
      "\n"
      "Connection and token\n"
      "  login [--url URL] [--store keychain|file] [--from-file PATH | --stdin]\n"
      "                       check a token against the server and store it (prompts\n"
      "                       without echo by default)\n"
      "  logout               remove the stored token from this machine\n"
      "  status               server version and whether the token is accepted\n"
      "  doctor               local settings and TLS support (no network)\n"
      "\n"
      "Projects\n"
      "  projects list [--all]\n"
      "  projects show ID\n"
      "  projects create NAME [--description T] [--owner T] [--start D] [--finish D]\n"
      "                       [--status S] [--rag R] [--priority N] [--dry-run]\n"
      "  projects update ID [--name T] [same options] [--rev N] [--dry-run]\n"
      "                       [--calendar ID|none] (create too): its working calendar\n"
      "                       [--objectives T] [--scope T] [--out-of-scope T]\n"
      "                       [--success-criteria T] (create too): the details page's\n"
      "                       text (Markdown); \"\" clears one\n"
      "                       [--sponsor T] [--budget N|none] [--color #rrggbb|none]\n"
      "                       (create too)\n"
      "  projects delete ID [--rev N] [--dry-run] [--confirm CONFIRM_ID]\n"
      "\n"
      "Tasks (dates are YYYY-MM-DD; \"none\" clears a date)\n"
      "  tasks list PROJECT [--status todo|in_progress|done] [--search TEXT] [--all]\n"
      "  tasks show ID\n"
      "  tasks add PROJECT TITLE [--parent ID] [--milestone] [--duration DAYS]\n"
      "                       [--start D] [--finish D] [--deadline D] [--priority 1-4]\n"
      "                       [--notes T] [--manual] [--calendar ID|none] [--dry-run]\n"
      "  tasks update ID [--title T] [--parent ID|none] [--status S] [--percent N]\n"
      "                       [--actual-start D] [--actual-finish D] [add options]\n"
      "                       [--rev N] [--dry-run]\n"
      "  tasks done ID [--dry-run]\n"
      "  tasks move ID [--parent ID|none] [--position N] [--rev N] [--dry-run]\n"
      "                       reparent and/or reorder within the project; N counts\n"
      "                       from 1 among the new parent's children (default: last)\n"
      "  tasks delete ID [--rev N] [--dry-run] [--confirm CONFIRM_ID]\n"
      "  tasks history ID\n"
      "\n"
      "Dependencies (links; TASK is a WBS code like 1.2 or a task ID)\n"
      "  deps list PROJECT [--task TASK] [--all]\n"
      "  deps add PROJECT TASK LINKS [--dry-run]\n"
      "                       LINKS are TASK's predecessors, as MS Project writes them:\n"
      "                       \"3FS+2d\" (after 3 finishes, plus 2 working days),\n"
      "                       \"1.2SS\", \"4FF-1d\", several separated by commas\n"
      "  deps update ID [--type FS|SS|FF] [--lag DAYS] [--rev N] [--dry-run]\n"
      "  deps remove ID [--rev N] [--dry-run] [--confirm CONFIRM_ID]\n"
      "\n"
      "Calendars (shared by every project; writing needs a token for all projects)\n"
      "  calendars list [--all]\n"
      "  calendars show ID   the calendar and its holidays and other exceptions\n"
      "  calendars create NAME [--base ID] [--week DAYS[:HOURS]] [--hours-per-day N]\n"
      "                       [--dry-run]   DAYS like Mon-Fri or Mon,Tue,Thu (default\n"
      "                       the base's week, at the root Mon-Fri, 8 hours)\n"
      "  calendars update ID [--name T] [--base ID|none] [--week DAYS[:HOURS]|inherit]\n"
      "                       [--hours-per-day N] [--rev N] [--dry-run]\n"
      "  calendars delete ID [--rev N] [--dry-run] [--confirm CONFIRM_ID]\n"
      "  calendars exception add ID FROM [TO] [--kind holiday|non_working|custom_hours]\n"
      "                       [--hours N] [--label T] [--dry-run]\n"
      "  calendars exception remove EXCEPTION_ID [--dry-run] [--confirm CONFIRM_ID]\n"
      "  calendars import-holidays ID YEAR [--country US] [--dry-run]\n"
      "                       a year's public holidays, computed (not fetched)\n"
      "\n"
      "Export\n"
      "  export PROJECT [--format json|csv|ods|mspdi] [--out-file PATH | --stdout]\n"
      "                       the project as a file: zproject JSON (full fidelity,\n"
      "                       the default), CSV (the task table), an OpenDocument\n"
      "                       spreadsheet, or MS Project XML (mspdi). Without\n"
      "                       --format, PATH's extension picks it (.json .csv .ods\n"
      "                       .xml). Without PATH, a new file named after the\n"
      "                       project in this directory. PDF: the desktop app.\n"
      "\n"
      "Administration (owner token)\n"
      "  token list\n"
      "  token create --name NAME --kind device|app|agent [--scopes S,S] [--projects ID,ID]\n"
      "                       [--expires 90d|never] [--require-confirm | --no-require-confirm]\n"
      "                       (--out-file PATH | --stdout)\n"
      "                       the new token goes only to a new 0600 file or to a pipe\n"
      "  token revoke ID [--dry-run] [--confirm CONFIRM_ID]\n"
      "  audit [--since SEQ] [--limit N] [--token ID]\n"
      "  backup               run a server backup now\n"
      "\n"
      "Settings: ZPROJECT_URL or ~/.config/zproject/cli.ini; token from ZPROJECT_TOKEN,\n"
      "the keychain or ~/.config/zproject/token (never a command-line flag).\n"
      "Exit codes: 0 ok, 1 error, 2 usage, 3 validation or conflict, 4 auth, 5 rate limited.\n");
}

namespace {

// ---------------------------------------------------------------- arguments

struct Args {
  QStringList pos;
  QHash<QString, QString> opts;  // --name value
  QSet<QString> flags;           // --flag
  QString error;
  bool has(const QString& o) const { return opts.contains(o); }
  QString opt(const QString& o) const { return opts.value(o); }
};

const QSet<QString>& valueOptions() {
  static const QSet<QString> s{
      QStringLiteral("url"),        QStringLiteral("store"),         QStringLiteral("from-file"),
      QStringLiteral("description"), QStringLiteral("owner"),        QStringLiteral("start"),
      QStringLiteral("finish"),     QStringLiteral("status"),        QStringLiteral("rag"),
      QStringLiteral("priority"),   QStringLiteral("rev"),           QStringLiteral("confirm"),
      QStringLiteral("name"),       QStringLiteral("search"),        QStringLiteral("parent"),
      QStringLiteral("duration"),   QStringLiteral("deadline"),      QStringLiteral("notes"),
      QStringLiteral("title"),      QStringLiteral("percent"),       QStringLiteral("actual-start"),
      QStringLiteral("actual-finish"), QStringLiteral("kind"),       QStringLiteral("scopes"),
      QStringLiteral("projects"),   QStringLiteral("expires"),       QStringLiteral("out-file"),
      QStringLiteral("since"),      QStringLiteral("limit"),         QStringLiteral("token"),
      QStringLiteral("position"),   QStringLiteral("task"),          QStringLiteral("type"),
      QStringLiteral("lag"),        QStringLiteral("base"),          QStringLiteral("week"),
      QStringLiteral("hours-per-day"), QStringLiteral("calendar"),   QStringLiteral("hours"),
      QStringLiteral("label"),      QStringLiteral("country"),       QStringLiteral("format"),
      QStringLiteral("objectives"), QStringLiteral("scope"),         QStringLiteral("out-of-scope"),
      QStringLiteral("success-criteria"), QStringLiteral("sponsor"), QStringLiteral("budget"),
      QStringLiteral("color")};
  return s;
}
const QSet<QString>& flagOptions() {
  static const QSet<QString> s{QStringLiteral("json"),      QStringLiteral("help"),
                               QStringLiteral("version"),   QStringLiteral("stdin"),
                               QStringLiteral("all"),       QStringLiteral("dry-run"),
                               QStringLiteral("milestone"), QStringLiteral("manual"),
                               QStringLiteral("stdout"),    QStringLiteral("require-confirm"),
                               QStringLiteral("no-require-confirm")};
  return s;
}

Args parseArgs(const QStringList& in) {
  Args a;
  bool rest = false;
  for (qsizetype i = 0; i < in.size(); ++i) {
    const QString& s = in.at(i);
    if (rest || !s.startsWith(QLatin1String("-")) || s == QLatin1String("-")) {
      a.pos << s;
      continue;
    }
    if (s == QLatin1String("--")) {
      rest = true;
      continue;
    }
    if (s == QLatin1String("-h")) {
      a.flags.insert(QStringLiteral("help"));
      continue;
    }
    if (!s.startsWith(QLatin1String("--"))) {
      a.error = QStringLiteral("unknown option %1").arg(s);
      return a;
    }
    QString name = s.mid(2);
    QString value;
    bool inlineValue = false;
    if (const qsizetype eq = name.indexOf(QLatin1Char('=')); eq > 0) {
      value = name.mid(eq + 1);
      name = name.left(eq);
      inlineValue = true;
    }
    if (flagOptions().contains(name)) {
      if (inlineValue) {
        a.error = QStringLiteral("--%1 takes no value").arg(name);
        return a;
      }
      a.flags.insert(name);
    } else if (valueOptions().contains(name)) {
      if (!inlineValue) {
        if (i + 1 >= in.size()) {
          a.error = QStringLiteral("--%1 needs a value").arg(name);
          return a;
        }
        value = in.at(++i);
      }
      if (a.opts.contains(name)) {
        a.error = QStringLiteral("--%1 given twice").arg(name);
        return a;
      }
      a.opts.insert(name, value);
    } else {
      a.error = QStringLiteral("unknown option --%1").arg(name);
      return a;
    }
  }
  return a;
}

// ------------------------------------------------------------------ output

QString str(const QJsonValue& v) {
  if (v.isNull() || v.isUndefined()) return QStringLiteral("-");
  if (v.isBool()) return v.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
  if (v.isDouble()) {
    const double d = v.toDouble();
    return d == static_cast<double>(static_cast<qint64>(d)) ? QString::number(static_cast<qint64>(d))
                                                            : QString::number(d);
  }
  if (v.isArray()) {
    QStringList parts;
    for (const auto& x : v.toArray()) parts << str(x);
    return parts.join(QLatin1Char(','));
  }
  if (v.isObject()) return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
  const QString s = v.toString();
  return s.isEmpty() ? QStringLiteral("-") : s;
}

void printTable(QTextStream& out, const QStringList& header, const QList<QStringList>& rows) {
  QList<qsizetype> w;
  for (const QString& h : header) w << h.size();
  for (const QStringList& r : rows) {
    for (qsizetype i = 0; i < r.size() && i < w.size(); ++i) w[i] = std::max(w[i], r[i].size());
  }
  auto line = [&](const QStringList& cells) {
    QString s;
    for (qsizetype i = 0; i < cells.size(); ++i) {
      if (i + 1 == cells.size()) {
        s += cells[i];
      } else {
        s += cells[i].leftJustified(w[i]) + QStringLiteral("  ");
      }
    }
    while (s.endsWith(QLatin1Char(' '))) s.chop(1);
    out << s << '\n';
  };
  line(header);
  for (const QStringList& r : rows) line(r);
}

void printFields(QTextStream& out, const QJsonObject& o, const QStringList& order) {
  QStringList keys = order;
  for (const QString& k : o.keys()) {
    if (!keys.contains(k)) keys << k;
  }
  qsizetype width = 0;
  for (const QString& k : keys) {
    if (o.contains(k)) width = std::max(width, k.size());
  }
  for (const QString& k : keys) {
    if (!o.contains(k)) continue;
    QString v = str(o.value(k));
    v.replace(QLatin1Char('\n'), QLatin1String("\n") + QString(width + 2, QLatin1Char(' ')));
    out << (k + QLatin1Char(':')).leftJustified(width + 2) << v << '\n';
  }
}

int exitFor(const ApiResponse& r) {
  if (r.ok()) return kExitOk;
  switch (r.status) {
    case 0:
      return kExitError;
    case 400:
    case 409:
    case 412:
    case 415:
      return kExitConflict;
    case 401:
    case 403:
      return kExitAuth;
    case 429:
      return kExitRateLimited;
    default:
      return kExitError;
  }
}

const QRegularExpression& idRe() {
  static const QRegularExpression re(QStringLiteral("^[0-9a-f]{20}$"));
  return re;
}

// ------------------------------------------------------------------- runner

class Runner {
 public:
  Runner(CliIo& io, QTextStream& out, QTextStream& err) : io_(io), out_(out), err_(err) {}

  int run(const QStringList& argv) {
    a_ = parseArgs(argv);
    if (!a_.error.isEmpty()) return usage(a_.error);
    json_ = a_.flags.contains(QStringLiteral("json"));
    if (a_.flags.contains(QStringLiteral("version"))) {
      out_ << versionLine(QStringLiteral("zproject-cli")) << '\n';
      return kExitOk;
    }
    if (a_.flags.contains(QStringLiteral("help"))) {
      out_ << cliUsage();
      return kExitOk;
    }
    if (a_.pos.isEmpty()) return usage(QStringLiteral("no command given"));
    config_ = loadClientConfig(io_.env, a_.opt(QStringLiteral("url")));
    store_ = std::make_unique<TokenStore>(config_.tokenFile, io_.runner, io_.macos);

    const QString cmd = a_.pos.takeFirst();
    if (cmd == QLatin1String("login")) return login();
    if (cmd == QLatin1String("logout")) return logout();
    if (cmd == QLatin1String("status")) return status();
    if (cmd == QLatin1String("doctor")) return doctor();
    if (cmd == QLatin1String("projects")) return projects();
    if (cmd == QLatin1String("tasks")) return tasks();
    if (cmd == QLatin1String("deps")) return deps();
    if (cmd == QLatin1String("calendars")) return calendars();
    if (cmd == QLatin1String("token")) return token();
    if (cmd == QLatin1String("audit")) return audit();
    if (cmd == QLatin1String("backup")) return backup();
    if (cmd == QLatin1String("export")) return exportProject();
    return usage(QStringLiteral("unknown command '%1'").arg(cmd));
  }

 private:
  // ------------------------------------------------------------- helpers
  int usage(const QString& why) {
    err_ << "zproject-cli: " << why << "\nRun `zproject-cli --help` for usage.\n";
    return kExitUsage;
  }
  int fail(int code, const QString& message) {
    err_ << "zproject-cli: " << message << '\n';
    return code;
  }
  // Refuses options the command does not take.
  bool allow(const QStringList& opts) {
    static const QStringList global{QStringLiteral("url"), QStringLiteral("json")};
    for (auto it = a_.opts.constBegin(); it != a_.opts.constEnd(); ++it) {
      if (!opts.contains(it.key()) && !global.contains(it.key())) {
        badOption_ = it.key();
        return false;
      }
    }
    for (const QString& f : a_.flags) {
      if (!opts.contains(f) && !global.contains(f)) {
        badOption_ = f;
        return false;
      }
    }
    return true;
  }
  int badOption() { return usage(QStringLiteral("--%1 does not apply to this command").arg(badOption_)); }
  bool flag(const char* f) const { return a_.flags.contains(QLatin1String(f)); }

  void printJson(const QJsonValue& v) {
    if (v.isArray()) {
      out_ << QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented);
    } else {
      out_ << QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented);
    }
  }

  int apiError(const ApiResponse& r) {
    if (json_ && r.status != 0 && !r.json.isEmpty()) {
      err_ << QJsonDocument(r.json).toJson(QJsonDocument::Compact) << '\n';
    }
    QString msg = r.errorMessage();
    if (r.status != 0) {
      msg += QStringLiteral(" (HTTP %1%2)").arg(r.status).arg(r.errorCode().isEmpty() ? QString()
                                                                                       : QLatin1Char(' ') + r.errorCode());
    }
    err_ << "zproject-cli: " << msg << '\n';
    if (r.errorCode() == QLatin1String("confirm_required")) {
      err_ << "  Run the same command with --dry-run to see what it does and get a confirm ID,\n"
              "  then again with --confirm CONFIRM_ID.\n";
    } else if (r.status == 429) {
      err_ << "  Retry after " << r.header("Retry-After") << " s.\n";
    } else if (r.status == 401) {
      err_ << "  The token was refused; run `zproject-cli login` with a valid token.\n";
    }
    return exitFor(r);
  }

  // The token, resolved as the desktop app does (client/TokenStore.hpp).
  std::optional<QString> resolveToken(QString* source, int* code) {
    auto t = resolveDeviceToken(io_.env, config_.store, *store_);
    if (!t) {
      *code = fail(kExitAuth, t.error().message);
      return std::nullopt;
    }
    *source = t->source;
    return t->token;
  }

  // Connects with the stored token and checks the server's API version.
  int connect(bool needToken = true) {
    auto url = parseServerUrl(config_.url);
    if (!url) return fail(kExitUsage, url.error().message);
    QString token;
    if (needToken) {
      int code = kExitOk;
      auto t = resolveToken(&tokenSource_, &code);
      if (!t) return code;
      token = *t;
    }
    api_ = std::make_unique<ApiClient>(url.value(), token.toLatin1());
    return checkServer();
  }

  int checkServer() {
    health_ = api_->get(QStringLiteral("/v1/health"));
    if (health_.status == 0) return fail(kExitError, health_.networkError);
    if (!health_.ok()) return apiError(health_);
    const int apiVersion = health_.json.value(QStringLiteral("apiVersion")).toInt();
    if (apiVersion != kApiVersion) {
      return fail(kExitError, QStringLiteral("the server speaks API v%1 (zprojectd %2); this zproject-cli %3 speaks v%4. "
                                             "Install matching versions.")
                                  .arg(apiVersion)
                                  .arg(health_.json.value(QStringLiteral("version")).toString(), appVersion())
                                  .arg(kApiVersion));
    }
    return kExitOk;
  }

  // All pages of a list endpoint.
  std::optional<QJsonArray> listAll(const QString& path, QUrlQuery query, int* code) {
    QJsonArray items;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("500"));
    for (int page = 0; page < 1000; ++page) {
      ApiResponse r = api_->get(path, query);
      if (!r.ok()) {
        *code = apiError(r);
        return std::nullopt;
      }
      for (const auto& v : r.json.value(QStringLiteral("items")).toArray()) items.append(v);
      const QString next = r.json.value(QStringLiteral("nextCursor")).toString();
      if (next.isEmpty()) break;
      query.removeAllQueryItems(QStringLiteral("cursor"));
      query.addQueryItem(QStringLiteral("cursor"), next);
    }
    return items;
  }

  // Date options: YYYY-MM-DD, or "none" for null.
  bool dateOpt(const QString& opt, const QString& field, QJsonObject* body) {
    if (!a_.has(opt)) return true;
    const QString v = a_.opt(opt);
    if (v == QLatin1String("none")) {
      body->insert(field, QJsonValue());
      return true;
    }
    if (!QDate::fromString(v, Qt::ISODate).isValid() || v.size() != 10) {
      badValue_ = QStringLiteral("--%1 must be YYYY-MM-DD or none").arg(opt);
      return false;
    }
    body->insert(field, v);
    return true;
  }
  bool intOpt(const QString& opt, const QString& field, QJsonObject* body, int lo, int hi) {
    if (!a_.has(opt)) return true;
    bool ok = false;
    const int v = a_.opt(opt).toInt(&ok);
    if (!ok || v < lo || v > hi) {
      badValue_ = QStringLiteral("--%1 must be a whole number from %2 to %3").arg(opt).arg(lo).arg(hi);
      return false;
    }
    body->insert(field, v);
    return true;
  }
  bool numOpt(const QString& opt, const QString& field, QJsonObject* body) {
    if (!a_.has(opt)) return true;
    bool ok = false;
    const double v = a_.opt(opt).toDouble(&ok);
    if (!ok || v < 0) {
      badValue_ = QStringLiteral("--%1 must be a number >= 0").arg(opt);
      return false;
    }
    body->insert(field, v);
    return true;
  }
  void strOpt(const QString& opt, const QString& field, QJsonObject* body) {
    if (a_.has(opt)) body->insert(field, a_.opt(opt));
  }
  bool checkId(const QString& id, const char* what) {
    if (idRe().match(id).hasMatch()) return true;
    badValue_ = QStringLiteral("%1 must be a 20-character hex id").arg(QLatin1String(what));
    return false;
  }

  // ?dryRun, If-Match and X-Zproject-Confirm for a write.
  struct WriteOpts {
    QUrlQuery query;
    QList<QPair<QByteArray, QByteArray>> headers;
    bool dryRun = false;
  };
  bool writeOpts(WriteOpts* w) {
    w->dryRun = flag("dry-run");
    if (w->dryRun) w->query.addQueryItem(QStringLiteral("dryRun"), QStringLiteral("true"));
    if (a_.has(QStringLiteral("rev"))) {
      bool ok = false;
      const qint64 rev = a_.opt(QStringLiteral("rev")).toLongLong(&ok);
      if (!ok || rev < 1) {
        badValue_ = QStringLiteral("--rev must be a revision number");
        return false;
      }
      w->headers.append({QByteArray("If-Match"), QByteArray::number(rev)});
    }
    if (a_.has(QStringLiteral("confirm"))) {
      if (w->dryRun) {
        badValue_ = QStringLiteral("--confirm and --dry-run do not go together");
        return false;
      }
      w->headers.append({QByteArray("X-Zproject-Confirm"), a_.opt(QStringLiteral("confirm")).toLatin1()});
    }
    return true;
  }

  int dryRunResult(const ApiResponse& r, const QString& command) {
    if (json_) {
      printJson(r.json);
      return kExitOk;
    }
    const QJsonArray changes = r.json.value(QStringLiteral("changes")).toArray();
    out_ << "Dry run: " << changes.size() << (changes.size() == 1 ? " change" : " changes") << ", nothing written.\n";
    for (const auto& v : changes) {
      const QJsonObject c = v.toObject();
      out_ << "  " << str(c.value(QStringLiteral("op"))) << ' ' << str(c.value(QStringLiteral("entityKind"))) << ' '
           << str(c.value(QStringLiteral("entityId")));
      const QJsonObject after = c.value(QStringLiteral("after")).toObject();
      if (!after.isEmpty()) out_ << "  " << QJsonDocument(after).toJson(QJsonDocument::Compact);
      out_ << '\n';
    }
    const QString confirm = r.json.value(QStringLiteral("confirmId")).toString();
    if (!confirm.isEmpty()) {
      out_ << "To go ahead: zproject-cli " << command << " --confirm " << confirm << "\n  (single use, valid until "
           << r.json.value(QStringLiteral("confirmExpiresAt")).toString() << ")\n";
    }
    return kExitOk;
  }

  // ----------------------------------------------------------- commands
  int login() {
    if (!allow({QStringLiteral("store"), QStringLiteral("from-file"), QStringLiteral("stdin")})) return badOption();
    if (!a_.pos.isEmpty()) return usage(QStringLiteral("login takes no arguments"));
    const QString storeName =
        a_.has(QStringLiteral("store")) ? a_.opt(QStringLiteral("store"))
                                        : (io_.macos ? QStringLiteral("keychain") : QStringLiteral("file"));
    if (storeName != QLatin1String("keychain") && storeName != QLatin1String("file")) {
      return usage(QStringLiteral("--store must be keychain or file"));
    }
    if (a_.has(QStringLiteral("from-file")) && flag("stdin")) {
      return usage(QStringLiteral("use --from-file or --stdin, not both"));
    }
    auto url = parseServerUrl(config_.url);
    if (!url) return fail(kExitUsage, url.error().message);

    QString token;
    if (a_.has(QStringLiteral("from-file"))) {
      QFile f(a_.opt(QStringLiteral("from-file")));
      if (!f.open(QIODevice::ReadOnly)) return fail(kExitError, QStringLiteral("cannot read %1").arg(f.fileName()));
      token = QString::fromLatin1(f.read(4096)).trimmed();
    } else if (flag("stdin")) {
      token = QString::fromLatin1(io_.readStdin ? io_.readStdin().left(4096) : QByteArray()).trimmed();
    } else {
      token = io_.promptSecret ? io_.promptSecret(QStringLiteral("zproject token: ")) : QString();
      if (token.isNull()) return usage(QStringLiteral("no terminal to prompt on; use --stdin or --from-file"));
      token = token.trimmed();
    }
    if (!looksLikeToken(token)) return fail(kExitUsage, QStringLiteral("that is not a zproject token (zpt_<id>.<secret>)"));

    api_ = std::make_unique<ApiClient>(url.value(), token.toLatin1());
    if (int c = checkServer(); c != kExitOk) return c;
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
    const ApiResponse probe = api_->get(QStringLiteral("/v1/projects"), q);
    if (probe.status == 401 || probe.status == 0 || (probe.status >= 500)) return apiError(probe);
    // 403 is fine: a valid token without projects:read.

    const TokenStore::Kind kind = storeName == QLatin1String("keychain") ? TokenStore::Kind::Keychain : TokenStore::Kind::File;
    const Status saved = kind == TokenStore::Kind::Keychain ? store_->writeKeychain(token) : store_->writeFile(token);
    if (!saved) return fail(kExitError, saved.error().message);
    // One copy only: drop the other store's.
    if (kind == TokenStore::Kind::Keychain) {
      (void)store_->removeFile();
    } else if (io_.macos) {
      (void)store_->removeKeychain();
    }
    config_.url = url.value().toString();
    config_.store = storeName;
    if (const Status s = saveClientConfig(config_); !s) return fail(kExitError, s.error().message);

    const QString where = kind == TokenStore::Kind::Keychain
                              ? (io_.macos ? QStringLiteral("the macOS Keychain") : QStringLiteral("the Secret Service keyring"))
                              : config_.tokenFile;
    if (json_) {
      printJson(QJsonObject{{QStringLiteral("url"), config_.url},
                            {QStringLiteral("token"), tokenLabel(token)},
                            {QStringLiteral("store"), storeName},
                            {QStringLiteral("server"), health_.json}});
    } else {
      out_ << "Logged in to " << config_.url << " (zprojectd " << health_.json.value(QStringLiteral("version")).toString()
           << ") as " << tokenLabel(token) << "; token stored in " << where << ".\n";
      if (probe.status == 403) out_ << "Note: this token cannot list projects (no projects:read).\n";
      if (a_.has(QStringLiteral("from-file"))) {
        out_ << "You can now delete " << a_.opt(QStringLiteral("from-file")) << ".\n";
      }
    }
    return kExitOk;
  }

  int logout() {
    if (!allow({})) return badOption();
    const Status f = store_->removeFile();
    Status k = Ok{};
    if (io_.macos || config_.store == QLatin1String("keychain")) k = store_->removeKeychain();
    if (!f) return fail(kExitError, f.error().message);
    if (!k) return fail(kExitError, k.error().message);
    out_ << "Removed the stored token from this machine. It stays valid on the server until revoked "
            "(`zproject-cli token revoke ID` with the owner token).\n";
    return kExitOk;
  }

  int status() {
    if (!allow({})) return badOption();
    auto url = parseServerUrl(config_.url);
    if (!url) return fail(kExitUsage, url.error().message);
    QString source;
    int code = kExitOk;
    auto t = resolveToken(&source, &code);
    api_ = std::make_unique<ApiClient>(url.value(), t ? t->toLatin1() : QByteArray());
    if (int c = checkServer(); c != kExitOk) return c;
    QString auth = QStringLiteral("no token");
    int rc = code == kExitOk ? kExitAuth : code;
    if (t) {
      QUrlQuery q;
      q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
      const ApiResponse r = api_->get(QStringLiteral("/v1/projects"), q);
      if (r.ok()) {
        auth = QStringLiteral("ok");
        rc = kExitOk;
      } else if (r.status == 403) {
        auth = QStringLiteral("ok (cannot list projects)");
        rc = kExitOk;
      } else {
        auth = QStringLiteral("refused: ") + r.errorMessage();
        rc = exitFor(r);
      }
    }
    if (json_) {
      printJson(QJsonObject{{QStringLiteral("url"), url.value().toString()},
                            {QStringLiteral("urlSource"), config_.urlSource},
                            {QStringLiteral("server"), health_.json},
                            {QStringLiteral("token"), t ? QJsonValue(tokenLabel(*t)) : QJsonValue()},
                            {QStringLiteral("tokenSource"), t ? QJsonValue(source) : QJsonValue()},
                            {QStringLiteral("auth"), auth}});
    } else {
      out_ << "server:  " << url.value().toString() << " (from " << config_.urlSource << ")\n"
           << "zprojectd " << health_.json.value(QStringLiteral("version")).toString() << ", API v"
           << health_.json.value(QStringLiteral("apiVersion")).toInt() << ", schema "
           << health_.json.value(QStringLiteral("schemaVersion")).toInt() << '\n'
           << "token:   " << (t ? tokenLabel(*t) + QStringLiteral(" (") + source + QLatin1Char(')') : QStringLiteral("none"))
           << '\n'
           << "auth:    " << auth << '\n';
    }
    return rc;
  }

  int doctor() {
    if (!allow({})) return badOption();
    QJsonObject o;
    o.insert(QStringLiteral("version"), appVersion());
    o.insert(QStringLiteral("platform"), QSysInfo::prettyProductName() + QStringLiteral(" ") + QSysInfo::currentCpuArchitecture());
    o.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
    o.insert(QStringLiteral("configDir"), config_.configDir);
    auto url = parseServerUrl(config_.url);
    o.insert(QStringLiteral("url"), url ? url.value().toString() : QStringLiteral("(%1)").arg(url.error().message));
    o.insert(QStringLiteral("store"), config_.store.isEmpty() ? QStringLiteral("(not chosen)") : config_.store);
    o.insert(QStringLiteral("tokenFile"), config_.tokenFile);
    bool tls = false;
#if QT_CONFIG(ssl)
    tls = QSslSocket::supportsSsl();
    o.insert(QStringLiteral("tlsBackend"), QSslSocket::activeBackend());
    o.insert(QStringLiteral("tlsLibrary"), QSslSocket::sslLibraryVersionString());
#endif
    o.insert(QStringLiteral("tls"), tls);
    if (json_) {
      printJson(o);
    } else {
      printFields(out_, o,
                  {QStringLiteral("version"), QStringLiteral("platform"), QStringLiteral("qt"), QStringLiteral("tls"),
                   QStringLiteral("tlsBackend"), QStringLiteral("tlsLibrary"), QStringLiteral("configDir"),
                   QStringLiteral("url"), QStringLiteral("store"), QStringLiteral("tokenFile")});
    }
    if (!tls) return fail(kExitError, QStringLiteral("TLS is not available: https:// servers cannot be reached"));
    return kExitOk;
  }

  // ---------------------------------------------------------- projects
  QJsonObject projectBody() {
    QJsonObject b;
    strOpt(QStringLiteral("name"), QStringLiteral("name"), &b);
    strOpt(QStringLiteral("description"), QStringLiteral("description"), &b);
    strOpt(QStringLiteral("owner"), QStringLiteral("owner"), &b);
    strOpt(QStringLiteral("objectives"), QStringLiteral("objectives"), &b);
    strOpt(QStringLiteral("scope"), QStringLiteral("scope"), &b);
    strOpt(QStringLiteral("out-of-scope"), QStringLiteral("outOfScope"), &b);
    strOpt(QStringLiteral("success-criteria"), QStringLiteral("successCriteria"), &b);
    strOpt(QStringLiteral("sponsor"), QStringLiteral("sponsor"), &b);
    strOpt(QStringLiteral("status"), QStringLiteral("status"), &b);
    strOpt(QStringLiteral("rag"), QStringLiteral("rag"), &b);
    if (!dateOpt(QStringLiteral("start"), QStringLiteral("start"), &b) ||
        !dateOpt(QStringLiteral("finish"), QStringLiteral("finish"), &b) ||
        !intOpt(QStringLiteral("priority"), QStringLiteral("priority"), &b, 1, 4) ||
        !calendarOpt(&b) || !budgetOpt(&b) || !colorOpt(&b)) {
      b.insert(QStringLiteral("__invalid"), true);
    }
    return b;
  }
  // --budget N|none: a number >= 0, or null (no budget).
  bool budgetOpt(QJsonObject* b) {
    if (!a_.has(QStringLiteral("budget"))) return true;
    const QString v = a_.opt(QStringLiteral("budget"));
    if (v == QLatin1String("none")) {
      b->insert(QStringLiteral("budget"), QJsonValue());
      return true;
    }
    return numOpt(QStringLiteral("budget"), QStringLiteral("budget"), b);
  }
  // --color #rrggbb|none: the project's color (sidebar, milestones), or null.
  bool colorOpt(QJsonObject* b) {
    if (!a_.has(QStringLiteral("color"))) return true;
    const QString v = a_.opt(QStringLiteral("color"));
    if (v == QLatin1String("none")) {
      b->insert(QStringLiteral("color"), QJsonValue());
      return true;
    }
    static const QRegularExpression re(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    if (!re.match(v).hasMatch()) {
      badValue_ = QStringLiteral("--color must be #rrggbb or none");
      return false;
    }
    b->insert(QStringLiteral("color"), v.toLower());
    return true;
  }
  // --calendar ID|none: a calendar id, or null (Standard / the project's).
  bool calendarOpt(QJsonObject* b) {
    if (!a_.has(QStringLiteral("calendar"))) return true;
    const QString v = a_.opt(QStringLiteral("calendar"));
    if (v == QLatin1String("none")) {
      b->insert(QStringLiteral("calendar"), QJsonValue());
      return true;
    }
    if (!checkId(v, "--calendar")) return false;
    b->insert(QStringLiteral("calendar"), v);
    return true;
  }

  int projects() {
    if (a_.pos.isEmpty()) return usage(QStringLiteral("projects: list, show, create, update or delete"));
    const QString sub = a_.pos.takeFirst();
    const QStringList fields{QStringLiteral("description"), QStringLiteral("owner"), QStringLiteral("start"),
                             QStringLiteral("finish"),      QStringLiteral("status"), QStringLiteral("rag"),
                             QStringLiteral("priority"),    QStringLiteral("dry-run"),  QStringLiteral("calendar"),
                             QStringLiteral("objectives"),  QStringLiteral("scope"),    QStringLiteral("out-of-scope"),
                             QStringLiteral("success-criteria"), QStringLiteral("sponsor"), QStringLiteral("budget"),
                             QStringLiteral("color")};
    if (sub == QLatin1String("list")) {
      if (!allow({QStringLiteral("all")})) return badOption();
      if (!a_.pos.isEmpty()) return usage(QStringLiteral("projects list takes no arguments"));
      if (int c = connect(); c != kExitOk) return c;
      QUrlQuery q;
      if (flag("all")) q.addQueryItem(QStringLiteral("includeDeleted"), QStringLiteral("true"));
      int code = kExitOk;
      auto items = listAll(QStringLiteral("/v1/projects"), q, &code);
      if (!items) return code;
      if (json_) {
        printJson(*items);
        return kExitOk;
      }
      QList<QStringList> rows;
      for (const auto& v : *items) {
        const QJsonObject p = v.toObject();
        rows << QStringList{str(p.value(QStringLiteral("id"))),     str(p.value(QStringLiteral("name"))),
                            str(p.value(QStringLiteral("status"))), str(p.value(QStringLiteral("rag"))),
                            str(p.value(QStringLiteral("start"))),  str(p.value(QStringLiteral("finish")))};
      }
      printTable(out_, {QStringLiteral("ID"), QStringLiteral("NAME"), QStringLiteral("STATUS"), QStringLiteral("RAG"),
                        QStringLiteral("START"), QStringLiteral("FINISH")},
                 rows);
      return kExitOk;
    }
    if (sub == QLatin1String("show")) {
      if (!allow({})) return badOption();
      if (a_.pos.size() != 1) return usage(QStringLiteral("projects show ID"));
      if (!checkId(a_.pos[0], "project ID")) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r = api_->get(QStringLiteral("/v1/projects/") + a_.pos[0]);
      if (!r.ok()) return apiError(r);
      if (json_) {
        printJson(r.json);
      } else {
        printFields(out_, r.json, {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("status"),
                                   QStringLiteral("rag"), QStringLiteral("start"), QStringLiteral("finish"),
                                   QStringLiteral("owner"), QStringLiteral("sponsor"), QStringLiteral("budget"),
                                   QStringLiteral("color"), QStringLiteral("description"), QStringLiteral("objectives"),
                                   QStringLiteral("scope"), QStringLiteral("outOfScope"),
                                   QStringLiteral("successCriteria"), QStringLiteral("rev")});
      }
      return kExitOk;
    }
    if (sub == QLatin1String("create")) {
      if (!allow(fields)) return badOption();
      if (a_.pos.size() != 1) return usage(QStringLiteral("projects create NAME"));
      QJsonObject b = projectBody();
      if (b.contains(QStringLiteral("__invalid"))) return usage(badValue_);
      b.insert(QStringLiteral("name"), a_.pos[0]);
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r = api_->send("POST", QStringLiteral("/v1/projects"), w.query, &b, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("projects create"));
      return written(r, QStringLiteral("Created project"), QStringLiteral("name"));
    }
    if (sub == QLatin1String("update")) {
      if (!allow(fields + QStringList{QStringLiteral("name"), QStringLiteral("rev")})) return badOption();
      if (a_.pos.size() != 1) return usage(QStringLiteral("projects update ID [options]"));
      if (!checkId(a_.pos[0], "project ID")) return usage(badValue_);
      QJsonObject b = projectBody();
      if (b.contains(QStringLiteral("__invalid"))) return usage(badValue_);
      if (b.isEmpty()) return usage(QStringLiteral("nothing to change: give at least one field option"));
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r =
          api_->send("PATCH", QStringLiteral("/v1/projects/") + a_.pos[0], w.query, &b, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("projects update ") + a_.pos[0]);
      return written(r, QStringLiteral("Updated project"), QStringLiteral("name"));
    }
    if (sub == QLatin1String("delete")) {
      return deleteCommand(QStringLiteral("projects"), QStringLiteral("/v1/projects/"), "project ID");
    }
    return usage(QStringLiteral("unknown projects command '%1'").arg(sub));
  }

  int written(const ApiResponse& r, const QString& what, const QString& titleKey) {
    if (json_) {
      printJson(r.json);
    } else {
      out_ << what << ' ' << str(r.json.value(QStringLiteral("id"))) << " (rev "
           << str(r.json.value(QStringLiteral("rev"))) << "): " << str(r.json.value(titleKey)) << '\n';
    }
    return kExitOk;
  }

  int deleteCommand(const QString& noun, const QString& base, const char* what) {
    QStringList allowed{QStringLiteral("rev"), QStringLiteral("dry-run"), QStringLiteral("confirm")};
    if (!allow(allowed)) return badOption();
    if (a_.pos.size() != 1) return usage(QStringLiteral("%1 delete ID").arg(noun));
    if (!checkId(a_.pos[0], what)) return usage(badValue_);
    WriteOpts w;
    if (!writeOpts(&w)) return usage(badValue_);
    if (int c = connect(); c != kExitOk) return c;
    const ApiResponse r = api_->send("DELETE", base + a_.pos[0], w.query, nullptr, w.headers);
    if (!r.ok()) return apiError(r);
    if (w.dryRun) return dryRunResult(r, noun + QStringLiteral(" delete ") + a_.pos[0]);
    if (json_) {
      printJson(r.json);
    } else {
      out_ << "Deleted " << a_.pos[0] << " (a tombstone: history keeps it).\n";
    }
    return kExitOk;
  }

  // -------------------------------------------------------------- tasks
  bool taskBody(QJsonObject* b) {
    strOpt(QStringLiteral("title"), QStringLiteral("title"), b);
    strOpt(QStringLiteral("notes"), QStringLiteral("notes"), b);
    strOpt(QStringLiteral("status"), QStringLiteral("status"), b);
    if (flag("milestone")) {
      b->insert(QStringLiteral("type"), QStringLiteral("milestone"));
      b->insert(QStringLiteral("duration"), 0);
    }
    if (flag("manual")) b->insert(QStringLiteral("mode"), QStringLiteral("manual"));
    if (a_.has(QStringLiteral("parent"))) {
      const QString p = a_.opt(QStringLiteral("parent"));
      if (p == QLatin1String("none")) {
        b->insert(QStringLiteral("parent"), QJsonValue());
      } else {
        if (!checkId(p, "--parent")) return false;
        b->insert(QStringLiteral("parent"), p);
      }
    }
    return calendarOpt(b) && numOpt(QStringLiteral("duration"), QStringLiteral("duration"), b) &&
           dateOpt(QStringLiteral("start"), QStringLiteral("start"), b) &&
           dateOpt(QStringLiteral("finish"), QStringLiteral("finish"), b) &&
           dateOpt(QStringLiteral("deadline"), QStringLiteral("deadline"), b) &&
           dateOpt(QStringLiteral("actual-start"), QStringLiteral("actualStart"), b) &&
           dateOpt(QStringLiteral("actual-finish"), QStringLiteral("actualFinish"), b) &&
           intOpt(QStringLiteral("priority"), QStringLiteral("priority"), b, 1, 4) &&
           intOpt(QStringLiteral("percent"), QStringLiteral("percentComplete"), b, 0, 100);
  }

  int tasks() {
    if (a_.pos.isEmpty()) return usage(QStringLiteral("tasks: list, show, add, update, move, done, delete or history"));
    const QString sub = a_.pos.takeFirst();
    const QStringList addFields{QStringLiteral("parent"),   QStringLiteral("milestone"), QStringLiteral("duration"),
                                QStringLiteral("start"),    QStringLiteral("finish"),    QStringLiteral("deadline"),
                                QStringLiteral("priority"), QStringLiteral("notes"),     QStringLiteral("manual"),
                                QStringLiteral("dry-run"),  QStringLiteral("calendar")};
    if (sub == QLatin1String("list")) {
      if (!allow({QStringLiteral("status"), QStringLiteral("search"), QStringLiteral("all")})) return badOption();
      if (a_.pos.size() != 1) return usage(QStringLiteral("tasks list PROJECT"));
      if (!checkId(a_.pos[0], "project ID")) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      QUrlQuery q;
      if (a_.has(QStringLiteral("status"))) q.addQueryItem(QStringLiteral("status"), a_.opt(QStringLiteral("status")));
      if (a_.has(QStringLiteral("search"))) q.addQueryItem(QStringLiteral("q"), a_.opt(QStringLiteral("search")));
      if (flag("all")) q.addQueryItem(QStringLiteral("includeDeleted"), QStringLiteral("true"));
      int code = kExitOk;
      auto items = listAll(QStringLiteral("/v1/projects/%1/tasks").arg(a_.pos[0]), q, &code);
      if (!items) return code;
      if (json_) {
        printJson(*items);
        return kExitOk;
      }
      const ApiResponse wbs = api_->get(QStringLiteral("/v1/projects/%1/wbs").arg(a_.pos[0]));
      QHash<QString, QPair<QString, int>> codes;
      for (const auto& v : wbs.json.value(QStringLiteral("items")).toArray()) {
        const QJsonObject o = v.toObject();
        codes.insert(o.value(QStringLiteral("id")).toString(),
                     {o.value(QStringLiteral("wbs")).toString(), o.value(QStringLiteral("depth")).toInt()});
      }
      QList<QStringList> rows;
      for (const auto& v : *items) {
        const QJsonObject t = v.toObject();
        const auto c = codes.value(t.value(QStringLiteral("id")).toString(), {QStringLiteral("-"), 0});
        QString title = QString(static_cast<qsizetype>(c.second) * 2, QLatin1Char(' ')) + t.value(QStringLiteral("title")).toString();
        if (t.value(QStringLiteral("type")).toString() == QLatin1String("milestone")) title += QStringLiteral(" ◆");
        if (t.value(QStringLiteral("deleted")).toBool()) title += QStringLiteral(" (deleted)");
        rows << QStringList{c.first,
                            str(t.value(QStringLiteral("id"))),
                            str(t.value(QStringLiteral("status"))),
                            str(t.value(QStringLiteral("percentComplete"))) + QLatin1Char('%'),
                            str(t.value(QStringLiteral("start"))),
                            str(t.value(QStringLiteral("finish"))),
                            str(t.value(QStringLiteral("deadline"))),
                            title};
      }
      printTable(out_, {QStringLiteral("WBS"), QStringLiteral("ID"), QStringLiteral("STATUS"), QStringLiteral("DONE"),
                        QStringLiteral("START"), QStringLiteral("FINISH"), QStringLiteral("DUE"), QStringLiteral("TITLE")},
                 rows);
      return kExitOk;
    }
    if (sub == QLatin1String("show")) {
      if (!allow({})) return badOption();
      if (a_.pos.size() != 1) return usage(QStringLiteral("tasks show ID"));
      if (!checkId(a_.pos[0], "task ID")) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r = api_->get(QStringLiteral("/v1/tasks/") + a_.pos[0]);
      if (!r.ok()) return apiError(r);
      if (json_) {
        printJson(r.json);
      } else {
        printFields(out_, r.json,
                    {QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("project"), QStringLiteral("parent"),
                     QStringLiteral("type"), QStringLiteral("status"), QStringLiteral("percentComplete"),
                     QStringLiteral("duration"), QStringLiteral("start"), QStringLiteral("finish"),
                     QStringLiteral("deadline"), QStringLiteral("actualStart"), QStringLiteral("actualFinish"),
                     QStringLiteral("priority"), QStringLiteral("notes"), QStringLiteral("rev")});
      }
      return kExitOk;
    }
    if (sub == QLatin1String("add")) {
      if (!allow(addFields)) return badOption();
      if (a_.pos.size() != 2) return usage(QStringLiteral("tasks add PROJECT TITLE"));
      if (!checkId(a_.pos[0], "project ID")) return usage(badValue_);
      QJsonObject b;
      if (!taskBody(&b)) return usage(badValue_);
      b.insert(QStringLiteral("title"), a_.pos[1]);
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r =
          api_->send("POST", QStringLiteral("/v1/projects/%1/tasks").arg(a_.pos[0]), w.query, &b, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("tasks add"));
      return written(r, QStringLiteral("Added task"), QStringLiteral("title"));
    }
    if (sub == QLatin1String("update")) {
      if (!allow(addFields + QStringList{QStringLiteral("title"), QStringLiteral("status"), QStringLiteral("percent"),
                                         QStringLiteral("actual-start"), QStringLiteral("actual-finish"),
                                         QStringLiteral("rev")})) {
        return badOption();
      }
      if (a_.pos.size() != 1) return usage(QStringLiteral("tasks update ID [options]"));
      if (!checkId(a_.pos[0], "task ID")) return usage(badValue_);
      QJsonObject b;
      if (!taskBody(&b)) return usage(badValue_);
      if (b.isEmpty()) return usage(QStringLiteral("nothing to change: give at least one field option"));
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r = api_->send("PATCH", QStringLiteral("/v1/tasks/") + a_.pos[0], w.query, &b, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("tasks update ") + a_.pos[0]);
      return written(r, QStringLiteral("Updated task"), QStringLiteral("title"));
    }
    if (sub == QLatin1String("done")) {
      if (!allow({QStringLiteral("dry-run")})) return badOption();
      if (a_.pos.size() != 1) return usage(QStringLiteral("tasks done ID"));
      if (!checkId(a_.pos[0], "task ID")) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const QString path = QStringLiteral("/v1/tasks/") + a_.pos[0];
      const ApiResponse cur = api_->get(path);
      if (!cur.ok()) return apiError(cur);
      const QString today = io_.today.toString(Qt::ISODate);
      QJsonObject b{{QStringLiteral("status"), QStringLiteral("done")}, {QStringLiteral("percentComplete"), 100}};
      if (cur.json.value(QStringLiteral("actualStart")).toString().isEmpty()) b.insert(QStringLiteral("actualStart"), today);
      if (cur.json.value(QStringLiteral("actualFinish")).toString().isEmpty()) {
        b.insert(QStringLiteral("actualFinish"), today);
      }
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      w.headers.append({QByteArray("If-Match"),
                        QByteArray::number(cur.json.value(QStringLiteral("rev")).toInteger())});
      const ApiResponse r = api_->send("PATCH", path, w.query, &b, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("tasks done ") + a_.pos[0]);
      return written(r, QStringLiteral("Done:"), QStringLiteral("title"));
    }
    if (sub == QLatin1String("delete")) return deleteCommand(QStringLiteral("tasks"), QStringLiteral("/v1/tasks/"), "task ID");
    if (sub == QLatin1String("move")) {
      if (!allow({QStringLiteral("parent"), QStringLiteral("position"), QStringLiteral("rev"),
                  QStringLiteral("dry-run")})) {
        return badOption();
      }
      if (a_.pos.size() != 1) return usage(QStringLiteral("tasks move ID [--parent ID|none] [--position N]"));
      if (!checkId(a_.pos[0], "task ID")) return usage(badValue_);
      QJsonObject b;
      if (a_.has(QStringLiteral("parent"))) {
        const QString p = a_.opt(QStringLiteral("parent"));
        if (p == QLatin1String("none")) {
          b.insert(QStringLiteral("parent"), QJsonValue());
        } else {
          if (!checkId(p, "--parent")) return usage(badValue_);
          b.insert(QStringLiteral("parent"), p);
        }
      }
      // --position counts from 1 (as WBS numbers do); the API's position from 0.
      if (!intOpt(QStringLiteral("position"), QStringLiteral("position"), &b, 1, 1000000)) return usage(badValue_);
      if (b.contains(QStringLiteral("position"))) {
        b.insert(QStringLiteral("position"), b.value(QStringLiteral("position")).toInt() - 1);
      }
      if (b.isEmpty()) return usage(QStringLiteral("nothing to do: give --parent and/or --position"));
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r =
          api_->send("POST", QStringLiteral("/v1/tasks/%1/move").arg(a_.pos[0]), w.query, &b, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("tasks move ") + a_.pos[0]);
      return written(r, QStringLiteral("Moved task"), QStringLiteral("title"));
    }
    if (sub == QLatin1String("history")) {
      if (!allow({})) return badOption();
      if (a_.pos.size() != 1) return usage(QStringLiteral("tasks history ID"));
      if (!checkId(a_.pos[0], "task ID")) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r = api_->get(QStringLiteral("/v1/tasks/%1/history").arg(a_.pos[0]));
      if (!r.ok()) return apiError(r);
      const QJsonArray items = r.json.value(QStringLiteral("items")).toArray();
      if (json_) {
        printJson(items);
        return kExitOk;
      }
      QList<QStringList> rows;
      for (const auto& v : items) {
        const QJsonObject c = v.toObject();
        rows << QStringList{str(c.value(QStringLiteral("seq"))), str(c.value(QStringLiteral("ts"))),
                            str(c.value(QStringLiteral("actor"))), str(c.value(QStringLiteral("after")))};
      }
      printTable(out_, {QStringLiteral("SEQ"), QStringLiteral("TIME"), QStringLiteral("BY"), QStringLiteral("CHANGE")},
                 rows);
      return kExitOk;
    }
    return usage(QStringLiteral("unknown tasks command '%1'").arg(sub));
  }

  // ---------------------------------------------------------- calendars
  // "Mon-Fri", "Mon,Tue,Thu", "Mon-Thu:10" -> a work week (hours default 8).
  static std::optional<QJsonArray> parseWeek(const QString& spec, QString* why) {
    static const QStringList names{QStringLiteral("mon"), QStringLiteral("tue"), QStringLiteral("wed"),
                                   QStringLiteral("thu"), QStringLiteral("fri"), QStringLiteral("sat"),
                                   QStringLiteral("sun")};
    QString days = spec.trimmed().toLower();
    double hours = 8;
    if (const qsizetype colon = days.indexOf(QLatin1Char(':')); colon >= 0) {
      bool ok = false;
      hours = days.mid(colon + 1).toDouble(&ok);
      if (!ok || hours <= 0 || hours > 24) {
        *why = QStringLiteral("--week hours must be more than 0 and at most 24");
        return std::nullopt;
      }
      days = days.left(colon);
    }
    std::array<bool, 7> on{};
    for (const QString& part : days.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
      const QStringList ends = part.trimmed().split(QLatin1Char('-'));
      const qsizetype a = names.indexOf(ends.value(0).left(3));
      const qsizetype b = ends.size() == 2 ? names.indexOf(ends.value(1).left(3)) : a;
      if (a < 0 || b < 0 || b < a || ends.size() > 2) {
        *why = QStringLiteral("--week: days like Mon-Fri or Mon,Tue,Thu, optionally :HOURS");
        return std::nullopt;
      }
      for (qsizetype i = a; i <= b; ++i) on[static_cast<size_t>(i)] = true;
    }
    QJsonArray week;
    for (bool w : on) {
      week.append(QJsonObject{{QStringLiteral("working"), w}, {QStringLiteral("hours"), w ? hours : 0.0}});
    }
    return week;
  }
  static QString weekText(const QJsonValue& v) {
    if (!v.isArray()) return QStringLiteral("(inherited)");
    static const QStringList names{QStringLiteral("Mon"), QStringLiteral("Tue"), QStringLiteral("Wed"),
                                   QStringLiteral("Thu"), QStringLiteral("Fri"), QStringLiteral("Sat"),
                                   QStringLiteral("Sun")};
    QStringList on;
    for (int i = 0; i < 7 && i < v.toArray().size(); ++i) {
      const QJsonObject d = v.toArray().at(i).toObject();
      if (d.value(QStringLiteral("working")).toBool()) {
        on << QStringLiteral("%1 %2h").arg(names.at(i)).arg(d.value(QStringLiteral("hours")).toDouble());
      }
    }
    return on.join(QStringLiteral(", "));
  }
  bool calendarBody(QJsonObject* b) {
    strOpt(QStringLiteral("name"), QStringLiteral("name"), b);
    if (a_.has(QStringLiteral("base"))) {
      const QString v = a_.opt(QStringLiteral("base"));
      if (v == QLatin1String("none")) {
        b->insert(QStringLiteral("base"), QJsonValue());
      } else {
        if (!checkId(v, "--base")) return false;
        b->insert(QStringLiteral("base"), v);
      }
    }
    if (a_.has(QStringLiteral("week"))) {
      if (a_.opt(QStringLiteral("week")) == QLatin1String("inherit")) {
        b->insert(QStringLiteral("workWeek"), QJsonValue());
      } else {
        auto w = parseWeek(a_.opt(QStringLiteral("week")), &badValue_);
        if (!w) return false;
        b->insert(QStringLiteral("workWeek"), *w);
      }
    }
    if (a_.has(QStringLiteral("hours-per-day"))) {
      bool ok = false;
      const double h = a_.opt(QStringLiteral("hours-per-day")).toDouble(&ok);
      if (!ok || h <= 0 || h > 24) {
        badValue_ = QStringLiteral("--hours-per-day must be more than 0 and at most 24");
        return false;
      }
      b->insert(QStringLiteral("hoursPerDay"), h);
    }
    return true;
  }

  int calendars() {
    if (a_.pos.isEmpty()) {
      return usage(QStringLiteral("calendars: list, show, create, update, delete, exception or import-holidays"));
    }
    const QString sub = a_.pos.takeFirst();
    if (sub == QLatin1String("list")) {
      if (!allow({QStringLiteral("all")})) return badOption();
      if (!a_.pos.isEmpty()) return usage(QStringLiteral("calendars list"));
      if (int c = connect(); c != kExitOk) return c;
      QUrlQuery q;
      if (flag("all")) q.addQueryItem(QStringLiteral("includeDeleted"), QStringLiteral("true"));
      int code = kExitOk;
      auto items = listAll(QStringLiteral("/v1/calendars"), q, &code);
      if (!items) return code;
      if (json_) {
        printJson(*items);
        return kExitOk;
      }
      QList<QStringList> rows;
      for (const auto& v : *items) {
        const QJsonObject o = v.toObject();
        QString name = o.value(QStringLiteral("name")).toString();
        if (o.value(QStringLiteral("deleted")).toBool()) name += QStringLiteral(" (deleted)");
        rows << QStringList{str(o.value(QStringLiteral("id"))), name, str(o.value(QStringLiteral("base"))),
                            weekText(o.value(QStringLiteral("workWeek"))), str(o.value(QStringLiteral("hoursPerDay")))};
      }
      printTable(out_, {QStringLiteral("ID"), QStringLiteral("NAME"), QStringLiteral("BASE"), QStringLiteral("WEEK"),
                        QStringLiteral("HOURS/DAY")},
                 rows);
      return kExitOk;
    }
    if (sub == QLatin1String("show")) {
      if (!allow({})) return badOption();
      if (a_.pos.size() != 1) return usage(QStringLiteral("calendars show ID"));
      if (!checkId(a_.pos[0], "calendar ID")) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r = api_->get(QStringLiteral("/v1/calendars/") + a_.pos[0]);
      if (!r.ok()) return apiError(r);
      int code = kExitOk;
      auto excs = listAll(QStringLiteral("/v1/calendars/%1/exceptions").arg(a_.pos[0]), {}, &code);
      if (!excs) return code;
      if (json_) {
        QJsonObject o = r.json;
        o.insert(QStringLiteral("exceptions"), *excs);
        printJson(o);
        return kExitOk;
      }
      QJsonObject shown = r.json;
      shown.insert(QStringLiteral("workWeek"), weekText(r.json.value(QStringLiteral("workWeek"))));
      printFields(out_, shown,
                  {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("base"), QStringLiteral("workWeek"),
                   QStringLiteral("hoursPerDay"), QStringLiteral("rev")});
      QList<QStringList> rows;
      for (const auto& v : *excs) {
        const QJsonObject e = v.toObject();
        const QString from = e.value(QStringLiteral("from")).toString();
        const QString to = e.value(QStringLiteral("to")).toString();
        QString kind = e.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("custom_hours")) kind += QStringLiteral(" %1h").arg(e.value(QStringLiteral("hours")).toDouble());
        rows << QStringList{str(e.value(QStringLiteral("id"))), from == to ? from : from + QStringLiteral("..") + to, kind,
                            e.value(QStringLiteral("label")).toString()};
      }
      out_ << '\n';
      printTable(out_, {QStringLiteral("EXCEPTION"), QStringLiteral("DATES"), QStringLiteral("KIND"), QStringLiteral("LABEL")},
                 rows);
      return kExitOk;
    }
    if (sub == QLatin1String("create") || sub == QLatin1String("update")) {
      const bool create = sub == QLatin1String("create");
      QStringList opts{QStringLiteral("base"), QStringLiteral("week"), QStringLiteral("hours-per-day"),
                       QStringLiteral("dry-run")};
      if (!create) opts << QStringLiteral("name") << QStringLiteral("rev");
      if (!allow(opts)) return badOption();
      if (a_.pos.size() != 1) return usage(create ? QStringLiteral("calendars create NAME") : QStringLiteral("calendars update ID"));
      if (!create && !checkId(a_.pos[0], "calendar ID")) return usage(badValue_);
      QJsonObject b;
      if (!calendarBody(&b)) return usage(badValue_);
      if (create) {
        b.insert(QStringLiteral("name"), a_.pos[0]);
      } else if (b.isEmpty()) {
        return usage(QStringLiteral("nothing to change: give at least one option"));
      }
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r = create ? api_->send("POST", QStringLiteral("/v1/calendars"), w.query, &b, w.headers)
                                   : api_->send("PATCH", QStringLiteral("/v1/calendars/") + a_.pos[0], w.query, &b, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("calendars ") + sub);
      return written(r, create ? QStringLiteral("Created calendar") : QStringLiteral("Updated calendar"), QStringLiteral("name"));
    }
    if (sub == QLatin1String("delete")) {
      return deleteCommand(QStringLiteral("calendars"), QStringLiteral("/v1/calendars/"), "calendar ID");
    }
    if (sub == QLatin1String("exception")) {
      const QString what = a_.pos.isEmpty() ? QString() : a_.pos.takeFirst();
      if (what == QLatin1String("remove") || what == QLatin1String("delete")) {
        return deleteCommand(QStringLiteral("calendars exception"), QStringLiteral("/v1/calendar-exceptions/"),
                             "exception ID");
      }
      if (what != QLatin1String("add")) return usage(QStringLiteral("calendars exception add|remove ..."));
      if (!allow({QStringLiteral("kind"), QStringLiteral("hours"), QStringLiteral("label"), QStringLiteral("dry-run")})) {
        return badOption();
      }
      if (a_.pos.size() < 2 || a_.pos.size() > 3) return usage(QStringLiteral("calendars exception add ID FROM [TO]"));
      if (!checkId(a_.pos[0], "calendar ID")) return usage(badValue_);
      QJsonObject b{{QStringLiteral("from"), a_.pos[1]}, {QStringLiteral("to"), a_.pos.value(2, a_.pos[1])}};
      for (const QString& d : {b.value(QStringLiteral("from")).toString(), b.value(QStringLiteral("to")).toString()}) {
        if (d.size() != 10 || !QDate::fromString(d, Qt::ISODate).isValid()) return usage(QStringLiteral("dates are YYYY-MM-DD"));
      }
      strOpt(QStringLiteral("kind"), QStringLiteral("kind"), &b);
      strOpt(QStringLiteral("label"), QStringLiteral("label"), &b);
      if (!numOpt(QStringLiteral("hours"), QStringLiteral("hours"), &b)) return usage(badValue_);
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r =
          api_->send("POST", QStringLiteral("/v1/calendars/%1/exceptions").arg(a_.pos[0]), w.query, &b, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("calendars exception add"));
      return written(r, QStringLiteral("Added exception"), QStringLiteral("from"));
    }
    if (sub == QLatin1String("import-holidays")) {
      if (!allow({QStringLiteral("country"), QStringLiteral("dry-run")})) return badOption();
      if (a_.pos.size() != 2) return usage(QStringLiteral("calendars import-holidays ID YEAR"));
      if (!checkId(a_.pos[0], "calendar ID")) return usage(badValue_);
      bool ok = false;
      const int year = a_.pos[1].toInt(&ok);
      if (!ok) return usage(QStringLiteral("YEAR must be a year, like 2027"));
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      w.query.addQueryItem(QStringLiteral("country"), a_.has(QStringLiteral("country")) ? a_.opt(QStringLiteral("country"))
                                                                                         : QStringLiteral("US"));
      w.query.addQueryItem(QStringLiteral("year"), QString::number(year));
      if (int c = connect(); c != kExitOk) return c;
      QJsonObject none;  // POSTs are JSON, even with nothing to say
      const ApiResponse r = api_->send("POST", QStringLiteral("/v1/calendars/%1/import-holidays").arg(a_.pos[0]),
                                       w.query, &none, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("calendars import-holidays"));
      const QJsonArray items = r.json.value(QStringLiteral("items")).toArray();
      if (json_) {
        printJson(items);
        return kExitOk;
      }
      out_ << "Imported " << items.size() << (items.size() == 1 ? " holiday" : " holidays") << " for " << year
           << (items.isEmpty() ? " (all were there already).\n" : ":\n");
      for (const auto& v : items) {
        const QJsonObject e = v.toObject();
        out_ << "  " << e.value(QStringLiteral("from")).toString() << "  " << e.value(QStringLiteral("label")).toString() << '\n';
      }
      return kExitOk;
    }
    return usage(QStringLiteral("unknown calendars command '%1'").arg(sub));
  }

  // ------------------------------------------------------- dependencies
  // A project's WBS codes and titles by task id, and ids by WBS code.
  struct Wbs {
    QHash<QString, QString> code;   // id -> "1.2"
    QHash<QString, QString> title;  // id -> title
    QHash<QString, QString> byCode;  // "1.2" -> id
  };
  std::optional<Wbs> loadWbs(const QString& project, int* code) {
    const ApiResponse r = api_->get(QStringLiteral("/v1/projects/%1/wbs").arg(project));
    if (!r.ok()) {
      *code = apiError(r);
      return std::nullopt;
    }
    Wbs w;
    for (const auto& v : r.json.value(QStringLiteral("items")).toArray()) {
      const QJsonObject o = v.toObject();
      const QString id = o.value(QStringLiteral("id")).toString();
      w.code.insert(id, o.value(QStringLiteral("wbs")).toString());
      w.title.insert(id, o.value(QStringLiteral("title")).toString());
      w.byCode.insert(o.value(QStringLiteral("wbs")).toString(), id);
    }
    return w;
  }
  // A WBS code or a task id to a task id of the project; empty if unknown.
  static QString resolveTask(const Wbs& w, const QString& ref) {
    if (idRe().match(ref).hasMatch()) return w.code.contains(ref) ? ref : QString();
    return w.byCode.value(ref);
  }

  int deps() {
    if (a_.pos.isEmpty()) return usage(QStringLiteral("deps: list, add, update or remove"));
    const QString sub = a_.pos.takeFirst();
    if (sub == QLatin1String("list")) {
      if (!allow({QStringLiteral("task"), QStringLiteral("all")})) return badOption();
      if (a_.pos.size() != 1) return usage(QStringLiteral("deps list PROJECT [--task TASK]"));
      if (!checkId(a_.pos[0], "project ID")) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      int code = kExitOk;
      auto wbs = loadWbs(a_.pos[0], &code);
      if (!wbs) return code;
      QUrlQuery q;
      if (a_.has(QStringLiteral("task"))) {
        const QString t = resolveTask(*wbs, a_.opt(QStringLiteral("task")));
        if (t.isEmpty()) return fail(kExitConflict, QStringLiteral("no task %1 in this project").arg(a_.opt(QStringLiteral("task"))));
        q.addQueryItem(QStringLiteral("task"), t);
      }
      if (flag("all")) q.addQueryItem(QStringLiteral("includeDeleted"), QStringLiteral("true"));
      auto items = listAll(QStringLiteral("/v1/projects/%1/dependencies").arg(a_.pos[0]), q, &code);
      if (!items) return code;
      if (json_) {
        printJson(*items);
        return kExitOk;
      }
      auto name = [&](const QString& id) {
        const QString c = wbs->code.value(id, QStringLiteral("-"));
        return QStringLiteral("%1 %2").arg(c, wbs->title.value(id, id));
      };
      QList<QStringList> rows;
      for (const auto& v : *items) {
        const QJsonObject d = v.toObject();
        const QString pred = d.value(QStringLiteral("predecessor")).toString();
        QString link = formatLink(wbs->code.value(pred, pred), d.value(QStringLiteral("type")).toString(),
                                  d.value(QStringLiteral("lag")).toInt());
        if (d.value(QStringLiteral("deleted")).toBool()) link += QStringLiteral(" (deleted)");
        rows << QStringList{str(d.value(QStringLiteral("id"))), name(d.value(QStringLiteral("successor")).toString()),
                            link, name(pred)};
      }
      printTable(out_, {QStringLiteral("ID"), QStringLiteral("TASK"), QStringLiteral("AFTER"), QStringLiteral("PREDECESSOR")},
                 rows);
      return kExitOk;
    }
    if (sub == QLatin1String("add")) {
      if (!allow({QStringLiteral("dry-run")})) return badOption();
      if (a_.pos.size() != 3) return usage(QStringLiteral("deps add PROJECT TASK LINKS, e.g. deps add P 4 \"3FS+2d\""));
      if (!checkId(a_.pos[0], "project ID")) return usage(badValue_);
      auto specs = parseLinkNotation(a_.pos[2]);
      if (!specs) return usage(specs.error().message);
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      int code = kExitOk;
      auto wbs = loadWbs(a_.pos[0], &code);
      if (!wbs) return code;
      const QString succ = resolveTask(*wbs, a_.pos[1]);
      if (succ.isEmpty()) return fail(kExitConflict, QStringLiteral("no task %1 in this project").arg(a_.pos[1]));
      // One link per predecessor, in order; the first refusal stops the rest.
      QJsonArray created;
      for (const LinkSpec& s : specs.value()) {
        const QString pred = resolveTask(*wbs, s.ref);
        if (pred.isEmpty()) return fail(kExitConflict, QStringLiteral("no task %1 in this project").arg(s.ref));
        QJsonObject b{{QStringLiteral("predecessor"), pred},
                      {QStringLiteral("successor"), succ},
                      {QStringLiteral("type"), s.type},
                      {QStringLiteral("lag"), s.lag}};
        const ApiResponse r = api_->send("POST", QStringLiteral("/v1/dependencies"), w.query, &b, w.headers);
        if (!r.ok()) return apiError(r);
        const QString text = QStringLiteral("%1 after %2").arg(a_.pos[1], formatLink(s.ref, s.type, s.lag));
        if (w.dryRun) {
          if (!json_) out_ << text << ": ";
          dryRunResult(r, QStringLiteral("deps add"));
          continue;
        }
        if (json_) {
          created.append(r.json);
        } else {
          out_ << "Linked " << text << " (" << str(r.json.value(QStringLiteral("id"))) << ").\n";
        }
      }
      if (json_ && !w.dryRun) printJson(created);
      return kExitOk;
    }
    if (sub == QLatin1String("update")) {
      if (!allow({QStringLiteral("type"), QStringLiteral("lag"), QStringLiteral("rev"), QStringLiteral("dry-run")})) {
        return badOption();
      }
      if (a_.pos.size() != 1) return usage(QStringLiteral("deps update ID [--type FS|SS|FF] [--lag DAYS]"));
      if (!checkId(a_.pos[0], "dependency ID")) return usage(badValue_);
      QJsonObject b;
      if (a_.has(QStringLiteral("type"))) {
        const QString t = a_.opt(QStringLiteral("type")).toUpper();
        if (t != QLatin1String("FS") && t != QLatin1String("SS") && t != QLatin1String("FF")) {
          return usage(QStringLiteral("--type must be FS, SS or FF"));
        }
        b.insert(QStringLiteral("type"), t);
      }
      QString lag = a_.opt(QStringLiteral("lag"));
      if (lag.endsWith(QLatin1Char('d'))) lag.chop(1);
      if (a_.has(QStringLiteral("lag"))) {
        bool ok = false;
        const int v = lag.toInt(&ok);
        if (!ok || v < -kMaxLagDays || v > kMaxLagDays) {
          return usage(QStringLiteral("--lag must be whole working days from -%1 to %1").arg(kMaxLagDays));
        }
        b.insert(QStringLiteral("lag"), v);
      }
      if (b.isEmpty()) return usage(QStringLiteral("nothing to change: give --type and/or --lag"));
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r =
          api_->send("PATCH", QStringLiteral("/v1/dependencies/") + a_.pos[0], w.query, &b, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("deps update ") + a_.pos[0]);
      if (json_) {
        printJson(r.json);
      } else {
        out_ << "Updated link " << a_.pos[0] << " (rev " << str(r.json.value(QStringLiteral("rev"))) << "): "
             << str(r.json.value(QStringLiteral("type"))) << ' ' << str(r.json.value(QStringLiteral("lag")))
             << " days.\n";
      }
      return kExitOk;
    }
    if (sub == QLatin1String("remove") || sub == QLatin1String("delete")) {
      return deleteCommand(QStringLiteral("deps"), QStringLiteral("/v1/dependencies/"), "dependency ID");
    }
    return usage(QStringLiteral("unknown deps command '%1'").arg(sub));
  }

  // -------------------------------------------------------------- token
  int token() {
    if (a_.pos.isEmpty()) return usage(QStringLiteral("token: list, create or revoke"));
    const QString sub = a_.pos.takeFirst();
    if (sub == QLatin1String("list")) {
      if (!allow({})) return badOption();
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r = api_->get(QStringLiteral("/v1/tokens"));
      if (!r.ok()) return apiError(r);
      const QJsonArray items = r.json.value(QStringLiteral("items")).toArray();
      if (json_) {
        printJson(items);
        return kExitOk;
      }
      QList<QStringList> rows;
      for (const auto& v : items) {
        const QJsonObject t = v.toObject();
        rows << QStringList{str(t.value(QStringLiteral("id"))),        str(t.value(QStringLiteral("name"))),
                            str(t.value(QStringLiteral("kind"))),      str(t.value(QStringLiteral("scopes"))),
                            str(t.value(QStringLiteral("projects"))),  str(t.value(QStringLiteral("expiresAt"))),
                            str(t.value(QStringLiteral("lastUsedAt"))), str(t.value(QStringLiteral("active")))};
      }
      printTable(out_, {QStringLiteral("ID"), QStringLiteral("NAME"), QStringLiteral("KIND"), QStringLiteral("SCOPES"),
                        QStringLiteral("PROJECTS"), QStringLiteral("EXPIRES"), QStringLiteral("LAST USED"),
                        QStringLiteral("ACTIVE")},
                 rows);
      return kExitOk;
    }
    if (sub == QLatin1String("create")) return tokenCreate();
    if (sub == QLatin1String("revoke")) {
      if (!allow({QStringLiteral("dry-run"), QStringLiteral("confirm")})) return badOption();
      if (a_.pos.size() != 1) return usage(QStringLiteral("token revoke ID"));
      if (!checkId(a_.pos[0], "token ID")) return usage(badValue_);
      WriteOpts w;
      if (!writeOpts(&w)) return usage(badValue_);
      if (int c = connect(); c != kExitOk) return c;
      const ApiResponse r = api_->send("DELETE", QStringLiteral("/v1/tokens/") + a_.pos[0], w.query, nullptr, w.headers);
      if (!r.ok()) return apiError(r);
      if (w.dryRun) return dryRunResult(r, QStringLiteral("token revoke ") + a_.pos[0]);
      if (json_) {
        printJson(r.json);
      } else {
        out_ << "Revoked token " << a_.pos[0] << ".\n";
      }
      return kExitOk;
    }
    return usage(QStringLiteral("unknown token command '%1'").arg(sub));
  }

  int tokenCreate() {
    if (!allow({QStringLiteral("name"), QStringLiteral("kind"), QStringLiteral("scopes"), QStringLiteral("projects"),
                QStringLiteral("expires"), QStringLiteral("require-confirm"), QStringLiteral("no-require-confirm"),
                QStringLiteral("out-file"), QStringLiteral("stdout")})) {
      return badOption();
    }
    if (!a_.pos.isEmpty()) return usage(QStringLiteral("token create takes options only"));
    if (!a_.has(QStringLiteral("name")) || !a_.has(QStringLiteral("kind"))) {
      return usage(QStringLiteral("token create needs --name and --kind"));
    }
    const bool toFile = a_.has(QStringLiteral("out-file"));
    if (toFile == flag("stdout")) {
      return usage(QStringLiteral("say where the new token goes: --out-file PATH (a new 0600 file) or --stdout (a pipe)"));
    }
    if (flag("stdout") && io_.stdoutIsTty) {
      return usage(QStringLiteral("refusing to print a token to a terminal; use --out-file PATH or pipe the output"));
    }
    if (flag("require-confirm") && flag("no-require-confirm")) {
      return usage(QStringLiteral("--require-confirm or --no-require-confirm, not both"));
    }
    QJsonObject b{{QStringLiteral("name"), a_.opt(QStringLiteral("name"))},
                  {QStringLiteral("kind"), a_.opt(QStringLiteral("kind"))}};
    auto list = [](const QString& s) {
      QJsonArray out;
      for (const QString& part : s.split(QLatin1Char(','), Qt::SkipEmptyParts)) out.append(part.trimmed());
      return out;
    };
    if (a_.has(QStringLiteral("scopes"))) b.insert(QStringLiteral("scopes"), list(a_.opt(QStringLiteral("scopes"))));
    if (a_.has(QStringLiteral("projects"))) b.insert(QStringLiteral("projects"), list(a_.opt(QStringLiteral("projects"))));
    if (a_.has(QStringLiteral("expires"))) {
      QString e = a_.opt(QStringLiteral("expires"));
      if (e == QLatin1String("never")) {
        b.insert(QStringLiteral("expiresInDays"), QJsonValue());
      } else {
        if (e.endsWith(QLatin1Char('d'))) e.chop(1);
        bool ok = false;
        const int days = e.toInt(&ok);
        if (!ok || days < 1) return usage(QStringLiteral("--expires must be like 90d, or never"));
        b.insert(QStringLiteral("expiresInDays"), days);
      }
    }
    if (flag("require-confirm")) b.insert(QStringLiteral("requireConfirm"), true);
    if (flag("no-require-confirm")) b.insert(QStringLiteral("requireConfirm"), false);

    if (int c = connect(); c != kExitOk) return c;
    // Reserve the file first (new, 0600) so a token is never created with
    // nowhere to put it.
    int fd = -1;
    QByteArray path;
    if (toFile) {
      path = QFile::encodeName(a_.opt(QStringLiteral("out-file")));
      fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd < 0) {
        return fail(errno == EEXIST ? kExitUsage : kExitError,
                    errno == EEXIST ? QStringLiteral("%1 already exists; pick a new file").arg(QString::fromLocal8Bit(path))
                                    : QStringLiteral("cannot create %1").arg(QString::fromLocal8Bit(path)));
      }
    }
    const ApiResponse r = api_->send("POST", QStringLiteral("/v1/tokens"), {}, &b);
    if (!r.ok()) {
      if (fd >= 0) {
        ::close(fd);
        ::unlink(path.constData());
      }
      return apiError(r);
    }
    const QByteArray secret = r.json.value(QStringLiteral("token")).toString().toLatin1() + '\n';
    QJsonObject meta = r.json;
    meta.remove(QStringLiteral("token"));
    if (toFile) {
      const bool ok = ::write(fd, secret.constData(), static_cast<size_t>(secret.size())) == secret.size() &&
                      ::fsync(fd) == 0;
      if (::close(fd) != 0 || !ok) {
        ::unlink(path.constData());
        return fail(kExitError, QStringLiteral("could not write %1; revoke token %2")
                                    .arg(QString::fromLocal8Bit(path), meta.value(QStringLiteral("id")).toString()));
      }
    } else {
      out_ << secret;
      out_.flush();
    }
    QTextStream& info = toFile ? out_ : err_;
    if (json_) {
      info << QJsonDocument(meta).toJson(QJsonDocument::Indented);
    } else {
      info << "Created " << str(meta.value(QStringLiteral("kind"))) << " token " << str(meta.value(QStringLiteral("id")))
           << " '" << str(meta.value(QStringLiteral("name"))) << "': scopes " << str(meta.value(QStringLiteral("scopes")))
           << ", projects " << str(meta.value(QStringLiteral("projects"))) << ", expires "
           << str(meta.value(QStringLiteral("expiresAt"))) << ".\n";
      if (toFile) {
        info << "The token is in " << QString::fromLocal8Bit(path)
             << " (mode 0600). Move it to the client and run `zproject-cli login --from-file FILE`, then delete it.\n";
      }
    }
    return kExitOk;
  }

  int audit() {
    if (!allow({QStringLiteral("since"), QStringLiteral("limit"), QStringLiteral("token")})) return badOption();
    if (!a_.pos.isEmpty()) return usage(QStringLiteral("audit takes options only"));
    QUrlQuery q;
    if (a_.has(QStringLiteral("since"))) q.addQueryItem(QStringLiteral("since"), a_.opt(QStringLiteral("since")));
    q.addQueryItem(QStringLiteral("limit"), a_.has(QStringLiteral("limit")) ? a_.opt(QStringLiteral("limit")) : QStringLiteral("100"));
    if (a_.has(QStringLiteral("token"))) q.addQueryItem(QStringLiteral("tokenId"), a_.opt(QStringLiteral("token")));
    if (int c = connect(); c != kExitOk) return c;
    const ApiResponse r = api_->get(QStringLiteral("/v1/audit"), q);
    if (!r.ok()) return apiError(r);
    const QJsonArray items = r.json.value(QStringLiteral("items")).toArray();
    if (json_) {
      printJson(r.json);
      return kExitOk;
    }
    QList<QStringList> rows;
    for (const auto& v : items) {
      const QJsonObject e = v.toObject();
      rows << QStringList{str(e.value(QStringLiteral("seq"))),       str(e.value(QStringLiteral("ts"))),
                          str(e.value(QStringLiteral("tokenName"))), str(e.value(QStringLiteral("tokenKind"))),
                          str(e.value(QStringLiteral("method"))),    str(e.value(QStringLiteral("status"))),
                          str(e.value(QStringLiteral("path")))};
    }
    printTable(out_, {QStringLiteral("SEQ"), QStringLiteral("TIME"), QStringLiteral("TOKEN"), QStringLiteral("KIND"),
                      QStringLiteral("METHOD"), QStringLiteral("STATUS"), QStringLiteral("PATH")},
               rows);
    return kExitOk;
  }

  int backup() {
    if (!allow({})) return badOption();
    if (int c = connect(); c != kExitOk) return c;
    const QJsonObject empty;
    const ApiResponse r = api_->send("POST", QStringLiteral("/v1/admin/backup"), {}, &empty);
    if (!r.ok()) return apiError(r);
    if (json_) {
      printJson(r.json);
    } else {
      printFields(out_, r.json, {});
    }
    return kExitOk;
  }

  // GET /v1/projects/{id}/export to a file (atomically, via QSaveFile) or
  // to standard output.
  int exportProject() {
    if (!allow({QStringLiteral("format"), QStringLiteral("out-file"), QStringLiteral("stdout")})) return badOption();
    if (a_.pos.size() != 1) return usage(QStringLiteral("export PROJECT"));
    const QString project = a_.pos[0];
    if (!idRe().match(project).hasMatch()) return usage(QStringLiteral("PROJECT must be a project ID"));
    const bool toStdout = flag("stdout");
    const QString outFile = a_.opt(QStringLiteral("out-file"));
    if (toStdout && a_.has(QStringLiteral("out-file"))) return usage(QStringLiteral("--out-file or --stdout, not both"));
    if (a_.has(QStringLiteral("out-file")) && outFile.isEmpty()) return usage(QStringLiteral("--out-file needs a path"));

    std::optional<ExportFormat> format;
    if (a_.has(QStringLiteral("format"))) {
      format = parseExportFormat(a_.opt(QStringLiteral("format")));
      if (!format) return usage(QStringLiteral("--format must be json, csv, ods or mspdi"));
    } else if (!outFile.isEmpty()) {
      const QString ext = QFileInfo(outFile).suffix().toLower();
      if (ext == QLatin1String("xml")) {
        format = ExportFormat::Mspdi;
      } else if (ext == QLatin1String("csv") || ext == QLatin1String("ods") || ext == QLatin1String("json")) {
        format = parseExportFormat(ext);
      }
    }
    if (!format) format = ExportFormat::Json;
    if (toStdout && *format == ExportFormat::Ods && io_.stdoutIsTty) {
      return usage(QStringLiteral("refusing to write a spreadsheet to a terminal; use --out-file PATH or a pipe"));
    }

    if (int c = connect(); c != kExitOk) return c;
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("format"), exportFormatName(*format));
    const ApiResponse r = api_->get(QStringLiteral("/v1/projects/%1/export").arg(project), q);
    if (!r.ok()) return apiError(r);

    if (toStdout) {
      if (!io_.writeStdout) {
        out_ << QString::fromUtf8(r.body);
        out_.flush();
        return kExitOk;
      }
      return io_.writeStdout(r.body) ? kExitOk : fail(kExitError, QStringLiteral("could not write to standard output"));
    }
    QString path = outFile;
    if (path.isEmpty()) {
      // The server's suggested name, checked again here: a plain file name
      // in this directory, never a path. An existing file is never replaced
      // without being named.
      static const QRegularExpression nameRe(QStringLiteral("filename=\"([A-Za-z0-9][A-Za-z0-9._-]{0,120})\""));
      const auto m = nameRe.match(QString::fromLatin1(r.header("Content-Disposition")));
      path = m.hasMatch() ? m.captured(1) : exportFileName(QString(), *format);
      if (QFileInfo::exists(path)) {
        return fail(kExitUsage, QStringLiteral("%1 already exists; name the file with --out-file PATH").arg(path));
      }
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(r.body) != r.body.size() || !file.commit()) {
      return fail(kExitError, QStringLiteral("could not write %1: %2").arg(path, file.errorString()));
    }
    if (json_) {
      printJson(QJsonObject{{QStringLiteral("file"), path},
                            {QStringLiteral("format"), exportFormatName(*format)},
                            {QStringLiteral("bytes"), static_cast<qint64>(r.body.size())}});
    } else {
      out_ << "Wrote " << path << " (" << exportFormatName(*format) << ", " << r.body.size() << " bytes).\n";
    }
    return kExitOk;
  }

  CliIo& io_;
  QTextStream& out_;
  QTextStream& err_;
  Args a_;
  bool json_ = false;
  ClientConfig config_;
  std::unique_ptr<TokenStore> store_;
  std::unique_ptr<ApiClient> api_;
  ApiResponse health_;
  QString tokenSource_;
  QString badOption_;
  QString badValue_;
};

}  // namespace

int runCli(const QStringList& args, CliIo& io, QTextStream& out, QTextStream& err) {
  Runner r(io, out, err);
  const int code = r.run(args);
  out.flush();
  err.flush();
  return code;
}

}  // namespace zproject::client
