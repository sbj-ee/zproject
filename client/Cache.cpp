#include "client/Cache.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace zproject::client {

namespace {

const QString kProjectKind = QStringLiteral("project");
const QString kTaskKind = QStringLiteral("task");

QString toText(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
QJsonObject fromText(const QString& s) { return QJsonDocument::fromJson(s.toUtf8()).object(); }

qint64 revOf(const QJsonObject& o) { return static_cast<qint64>(o.value(QStringLiteral("rev")).toDouble(0)); }

// The file exists, is ours and is 0600 before SQLite opens it; the directory
// is 0700.
Status prepareFile(const QString& path) {
  const QString dir = QFileInfo(path).absolutePath();
  if (!QDir().mkpath(dir)) return Error{QStringLiteral("io"), QStringLiteral("cannot create %1").arg(dir), {}};
  ::chmod(QFile::encodeName(dir).constData(), 0700);
  const QByteArray p = QFile::encodeName(path);
  const int fd = ::open(p.constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return Error{QStringLiteral("io"), QStringLiteral("cannot open %1").arg(path), {}};
  const bool ok = ::fchmod(fd, 0600) == 0;
  ::close(fd);
  if (!ok) return Error{QStringLiteral("io"), QStringLiteral("cannot protect %1").arg(path), {}};
  return Ok{};
}

}  // namespace

Result<std::unique_ptr<Cache>> Cache::open(const QString& path) {
  if (path != QLatin1String(":memory:")) {
    if (auto s = prepareFile(path); !s) return s.error();
  }
  auto db = store::Database::open(path);
  if (!db) return db.error();
  std::unique_ptr<Cache> c(new Cache(std::move(db).value()));
  if (auto s = c->createSchema(); !s) return s.error();
  return c;
}

Status Cache::removeFiles(const QString& path) {
  for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
    const QString f = path + suffix;
    if (QFile::exists(f) && !QFile::remove(f)) {
      return Error{QStringLiteral("io"), QStringLiteral("cannot remove %1").arg(f), {}};
    }
  }
  return Ok{};
}

Status Cache::createSchema() {
  if (auto s = db_.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"));
      !s) {
    return s;
  }
  const QString format = meta(QStringLiteral("format"));
  if (!format.isEmpty() && format != QString::number(kFormat)) {
    if (auto s = db_.exec(QStringLiteral("DROP TABLE IF EXISTS project; DROP TABLE IF EXISTS task; "
                                         "DROP TABLE IF EXISTS wbs; DROP TABLE IF EXISTS activity; DELETE FROM meta;"));
        !s) {
      return s;
    }
  }
  if (auto s = db_.exec(QStringLiteral(
          "CREATE TABLE IF NOT EXISTS project (id TEXT PRIMARY KEY, rev INTEGER NOT NULL, deleted INTEGER NOT NULL, "
          "  ord INTEGER NOT NULL, json TEXT NOT NULL);"
          "CREATE TABLE IF NOT EXISTS task (id TEXT PRIMARY KEY, project TEXT NOT NULL, rev INTEGER NOT NULL, "
          "  deleted INTEGER NOT NULL, ord INTEGER NOT NULL, json TEXT NOT NULL);"
          "CREATE INDEX IF NOT EXISTS task_project ON task (project);"
          "CREATE TABLE IF NOT EXISTS wbs (project TEXT PRIMARY KEY, json TEXT NOT NULL);"
          "CREATE TABLE IF NOT EXISTS activity (seq INTEGER PRIMARY KEY, ts TEXT NOT NULL, actor TEXT NOT NULL, "
          "  entityKind TEXT NOT NULL, entityId TEXT NOT NULL, project TEXT NOT NULL, before TEXT NOT NULL, "
          "  after TEXT NOT NULL, requestId TEXT NOT NULL);"
          "CREATE INDEX IF NOT EXISTS activity_project ON activity (project, seq);"));
      !s) {
    return s;
  }
  return setMeta(QStringLiteral("format"), QString::number(kFormat));
}

QString Cache::meta(const QString& key) const {
  auto st = db_.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?1"));
  if (!st) return {};
  st.value().bind(1, key);
  return st.value().step() ? st.value().text(0) : QString();
}

Status Cache::setMeta(const QString& key, const QString& value) {
  auto st = db_.prepare(QStringLiteral("INSERT INTO meta (key, value) VALUES (?1, ?2) "
                                       "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
  if (!st) return st.error();
  st.value().bind(1, key).bind(2, value);
  return st.value().run();
}

qint64 Cache::lastSeq() const { return meta(QStringLiteral("lastSeq")).toLongLong(); }
QString Cache::lastSeqTs() const { return meta(QStringLiteral("lastSeqTs")); }
int Cache::schemaVersion() const { return meta(QStringLiteral("schemaVersion")).toInt(); }
QString Cache::serverVersion() const { return meta(QStringLiteral("serverVersion")); }
QDateTime Cache::refreshedAt() const {
  return QDateTime::fromString(meta(QStringLiteral("refreshedAt")), Qt::ISODateWithMs);
}

bool Cache::isEmpty() const {
  auto n = db_.scalar(QStringLiteral("SELECT (SELECT COUNT(*) FROM project) + (SELECT COUNT(*) FROM task)"));
  return !n || n->toLongLong() == 0;
}

Status Cache::setServer(const ServerHealth& health) {
  if (auto s = setMeta(QStringLiteral("schemaVersion"), QString::number(health.schemaVersion)); !s) return s;
  return setMeta(QStringLiteral("serverVersion"), health.version);
}

Status Cache::markRefreshed(const QDateTime& when) {
  return setMeta(QStringLiteral("refreshedAt"), when.toUTC().toString(Qt::ISODateWithMs));
}

Status Cache::reset() {
  store::Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  if (auto s = db_.exec(QStringLiteral("DELETE FROM project; DELETE FROM task; DELETE FROM wbs; DELETE FROM activity; "
                                       "DELETE FROM meta WHERE key <> 'format';"));
      !s) {
    return s;
  }
  return tx.commit();
}

Result<Cache::ApplyResult> Cache::apply(const QList<ChangeRecord>& records) {
  ApplyResult result;
  store::Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  const qint64 start = lastSeq();
  qint64 last = start;
  QString lastTs;
  for (const ChangeRecord& c : records) {
    if (c.seq <= last) continue;
    last = c.seq;
    lastTs = c.ts;
    QString projectId;
    const bool isProject = c.entityKind == kProjectKind;
    const bool isTask = c.entityKind == kTaskKind;
    if (isProject || isTask) {
      const QString table = isProject ? kProjectKind : kTaskKind;
      std::optional<QJsonObject> cur = row(table, c.entityId);
      const qint64 newRev = revOf(c.after);
      if (cur && newRev > 0 && revOf(*cur) >= newRev) {
        // Already as new (a snapshot or an earlier replay).
        projectId = isProject ? c.entityId : cur->value(QStringLiteral("project")).toString();
      } else {
        if (!cur && !c.before.isEmpty()) result.incomplete = true;  // an update to something unseen
        QJsonObject merged = cur.value_or(QJsonObject{});
        for (auto it = c.after.constBegin(); it != c.after.constEnd(); ++it) merged.insert(it.key(), it.value());
        merged.insert(QStringLiteral("id"), c.entityId);
        projectId = isProject ? c.entityId : merged.value(QStringLiteral("project")).toString();
        if (isTask && projectId.isEmpty()) {
          result.incomplete = true;
          continue;
        }
        const bool deleted = merged.value(QStringLiteral("deleted")).toBool();
        const auto ord = static_cast<qint64>(merged.value(QStringLiteral("order")).toDouble(0));
        auto st = isProject ? db_.prepare(QStringLiteral(
                                  "INSERT OR REPLACE INTO project (id, rev, deleted, ord, json) VALUES (?1, ?2, ?3, ?4, ?5)"))
                            : db_.prepare(QStringLiteral("INSERT OR REPLACE INTO task (id, rev, deleted, ord, json, project) "
                                                         "VALUES (?1, ?2, ?3, ?4, ?5, ?6)"));
        if (!st) return st.error();
        st.value().bind(1, c.entityId).bind(2, revOf(merged)).bind(3, deleted).bind(4, ord).bind(5, toText(merged));
        if (isTask) st.value().bind(6, projectId);
        if (auto s = st.value().run(); !s) return s.error();
        ++result.applied;
        result.projects.insert(projectId);
      }
    }
    auto st = db_.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO activity (seq, ts, actor, entityKind, entityId, project, before, after, requestId) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"));
    if (!st) return st.error();
    st.value()
        .bind(1, c.seq)
        .bind(2, c.ts)
        .bind(3, c.actor)
        .bind(4, c.entityKind)
        .bind(5, c.entityId)
        .bind(6, projectId)
        .bind(7, toText(c.before))
        .bind(8, toText(c.after))
        .bind(9, c.requestId);
    if (auto s = st.value().run(); !s) return s.error();
  }
  if (last != start) {
    if (auto s = setMeta(QStringLiteral("lastSeq"), QString::number(last)); !s) return s.error();
    if (auto s = setMeta(QStringLiteral("lastSeqTs"), lastTs); !s) return s.error();
    auto trim = db_.prepare(QStringLiteral(
        "DELETE FROM activity WHERE seq <= (SELECT seq FROM activity ORDER BY seq DESC LIMIT 1 OFFSET ?1)"));
    if (!trim) return trim.error();
    trim.value().bind(1, kActivityCap);
    if (auto s = trim.value().run(); !s) return s.error();
  }
  if (auto s = tx.commit(); !s) return s.error();
  return result;
}

Status Cache::replaceSnapshot(const QList<QJsonObject>& projects, const QList<QJsonObject>& tasks) {
  store::Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  if (auto s = db_.exec(QStringLiteral("DELETE FROM project; DELETE FROM task;")); !s) return s;
  for (const QJsonObject& p : projects) {
    auto st = db_.prepare(QStringLiteral("INSERT OR REPLACE INTO project (id, rev, deleted, ord, json) "
                                         "VALUES (?1, ?2, ?3, ?4, ?5)"));
    if (!st) return st.error();
    st.value()
        .bind(1, p.value(QStringLiteral("id")).toString())
        .bind(2, revOf(p))
        .bind(3, p.value(QStringLiteral("deleted")).toBool())
        .bind(4, static_cast<qint64>(p.value(QStringLiteral("order")).toDouble(0)))
        .bind(5, toText(p));
    if (auto s = st.value().run(); !s) return s;
  }
  for (const QJsonObject& t : tasks) {
    auto st = db_.prepare(QStringLiteral("INSERT OR REPLACE INTO task (id, project, rev, deleted, ord, json) "
                                         "VALUES (?1, ?2, ?3, ?4, ?5, ?6)"));
    if (!st) return st.error();
    st.value()
        .bind(1, t.value(QStringLiteral("id")).toString())
        .bind(2, t.value(QStringLiteral("project")).toString())
        .bind(3, revOf(t))
        .bind(4, t.value(QStringLiteral("deleted")).toBool())
        .bind(5, static_cast<qint64>(t.value(QStringLiteral("order")).toDouble(0)))
        .bind(6, toText(t));
    if (auto s = st.value().run(); !s) return s;
  }
  return tx.commit();
}

Status Cache::setWbs(const QString& projectId, const QJsonArray& items) {
  auto st = db_.prepare(QStringLiteral("INSERT OR REPLACE INTO wbs (project, json) VALUES (?1, ?2)"));
  if (!st) return st.error();
  st.value().bind(1, projectId).bind(2, QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Compact)));
  return st.value().run();
}

std::optional<QJsonObject> Cache::row(const QString& table, const QString& id) const {
  auto st = db_.prepare(QStringLiteral("SELECT json FROM %1 WHERE id = ?1").arg(table));
  if (!st) return std::nullopt;
  st.value().bind(1, id);
  if (!st.value().step()) return std::nullopt;
  return fromText(st.value().text(0));
}

QList<QJsonObject> Cache::rows(const QString& sql, const QString& arg) const {
  QList<QJsonObject> out;
  auto st = db_.prepare(sql);
  if (!st) return out;
  if (!arg.isNull()) st.value().bind(1, arg);
  while (st.value().step()) out.append(fromText(st.value().text(0)));
  return out;
}

QList<QJsonObject> Cache::projects() const {
  return rows(QStringLiteral("SELECT json FROM project WHERE deleted = 0 ORDER BY ord, id"), QString());
}

std::optional<QJsonObject> Cache::project(const QString& id) const {
  auto p = row(kProjectKind, id);
  if (p && p->value(QStringLiteral("deleted")).toBool()) return std::nullopt;
  return p;
}

QList<QJsonObject> Cache::tasks(const QString& projectId) const {
  return rows(QStringLiteral("SELECT json FROM task WHERE project = ?1 AND deleted = 0 ORDER BY ord, id"), projectId);
}

std::optional<QJsonObject> Cache::task(const QString& id) const {
  auto t = row(kTaskKind, id);
  if (t && t->value(QStringLiteral("deleted")).toBool()) return std::nullopt;
  return t;
}

QJsonArray Cache::wbs(const QString& projectId) const {
  auto st = db_.prepare(QStringLiteral("SELECT json FROM wbs WHERE project = ?1"));
  if (!st) return {};
  st.value().bind(1, projectId);
  if (!st.value().step()) return {};
  return QJsonDocument::fromJson(st.value().text(0).toUtf8()).array();
}

QList<ChangeRecord> Cache::activity(const QString& projectId, int limit) const {
  QList<ChangeRecord> out;
  auto st = projectId.isEmpty()
                ? db_.prepare(QStringLiteral("SELECT seq, ts, actor, entityKind, entityId, before, after, requestId "
                                             "FROM activity ORDER BY seq DESC LIMIT ?1"))
                : db_.prepare(QStringLiteral("SELECT seq, ts, actor, entityKind, entityId, before, after, requestId "
                                             "FROM activity WHERE project = ?2 ORDER BY seq DESC LIMIT ?1"));
  if (!st) return out;
  st.value().bind(1, limit);
  if (!projectId.isEmpty()) st.value().bind(2, projectId);
  while (st.value().step()) {
    ChangeRecord c;
    c.seq = st.value().int64(0);
    c.ts = st.value().text(1);
    c.actor = st.value().text(2);
    c.entityKind = st.value().text(3);
    c.entityId = st.value().text(4);
    c.before = fromText(st.value().text(5));
    c.after = fromText(st.value().text(6));
    c.requestId = st.value().text(7);
    out.append(std::move(c));
  }
  return out;
}

}  // namespace zproject::client
