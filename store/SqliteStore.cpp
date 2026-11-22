#include "SqliteStore.hpp"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <functional>

#include "core/Id.hpp"
#include "core/Rollup.hpp"
#include "store/Migrations.hpp"

namespace zproject::store {
namespace {

const QString kProject = QStringLiteral("project");
const QString kTask = QStringLiteral("task");

const QString kProjectColumns = QStringLiteral(
    "id, createdAt, updatedAt, deleted, rev, name, description, objectives, scope, outOfScope, "
    "successCriteria, owner, sponsor, status, rag, priority, start, finish, budget, color, ord");

const QString kTaskColumns = QStringLiteral(
    "id, createdAt, updatedAt, deleted, rev, project, parent, ord, title, notes, type, mode, "
    "duration, start, finish, constraintType, constraintDate, deadline, percentComplete, "
    "actualStart, actualFinish, status, priority, fixedCost");

Project readProject(const Statement& s) {
  Project p;
  p.id = s.text(0);
  p.createdAt = s.text(1);
  p.updatedAt = s.text(2);
  p.deleted = s.int64(3) != 0;
  p.rev = s.int64(4);
  p.name = s.text(5);
  p.description = s.text(6);
  p.objectives = s.text(7);
  p.scope = s.text(8);
  p.outOfScope = s.text(9);
  p.successCriteria = s.text(10);
  p.owner = s.text(11);
  p.sponsor = s.text(12);
  p.status = s.text(13);
  p.rag = s.text(14);
  p.priority = static_cast<int>(s.int64(15));
  p.start = s.text(16);
  p.finish = s.text(17);
  if (!s.isNull(18)) p.budget = s.real(18);
  p.color = s.text(19);
  p.order = s.int64(20);
  return p;
}

Task readTask(const Statement& s) {
  Task t;
  t.id = s.text(0);
  t.createdAt = s.text(1);
  t.updatedAt = s.text(2);
  t.deleted = s.int64(3) != 0;
  t.rev = s.int64(4);
  t.project = s.text(5);
  t.parent = s.text(6);
  t.order = s.int64(7);
  t.title = s.text(8);
  t.notes = s.text(9);
  t.type = s.text(10);
  t.mode = s.text(11);
  t.duration = s.real(12);
  t.start = s.text(13);
  t.finish = s.text(14);
  t.constraintType = s.text(15);
  t.constraintDate = s.text(16);
  t.deadline = s.text(17);
  t.percentComplete = static_cast<int>(s.int64(18));
  t.actualStart = s.text(19);
  t.actualFinish = s.text(20);
  t.status = s.text(21);
  t.priority = static_cast<int>(s.int64(22));
  t.fixedCost = s.real(23);
  return t;
}

QString compactJson(const QJsonObject& o) {
  return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

}  // namespace

Result<std::unique_ptr<SqliteStore>> SqliteStore::open(const QString& path) {
  auto db = Database::open(path);
  if (!db) return db.error();
  auto version = migrate(db.value());
  if (!version) return version.error();
  std::unique_ptr<SqliteStore> store(new SqliteStore(std::move(db).value(), *version));
  if (auto s = store->reconcileRollups(); !s) return s.error();
  return store;
}

Status SqliteStore::rollUp(const QString& projectId, const WriteContext& ctx) {
  auto all = tasks(projectId);
  if (!all) return all.error();
  QList<Task> rows = all.value();
  const QList<qsizetype> changed = rollUpSummaries(rows);
  for (qsizetype i : changed) {
    const Task& before = all.value()[i];
    Task t = rows[i];
    t.rev = before.rev + 1;
    t.updatedAt = nextTimestamp();
    if (auto s = writeTask(t, false); !s) return s;
    const FieldDiff d = diffFields(toJson(before), toJson(t));
    if (auto s = appendChange(kTask, t.id, d.before, d.after, ctx, t.updatedAt); !s) return s;
  }
  return Ok{};
}

Status SqliteStore::reconcileRollups() {
  auto ps = projects();
  if (!ps) return ps.error();
  Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  const WriteContext system;  // actor "system", no request id
  for (const Project& p : ps.value()) {
    if (auto s = rollUp(p.id, system); !s) return s;
  }
  return tx.commit();
}

QString SqliteStore::nextTimestamp() {
  const qint64 wall = QDateTime::currentMSecsSinceEpoch();
  lastTsMs_ = std::max(wall, lastTsMs_ + 1);
  return QDateTime::fromMSecsSinceEpoch(lastTsMs_).toUTC().toString(Qt::ISODateWithMs);
}

Status SqliteStore::appendChange(const QString& kind, const QString& id, const QJsonObject& before,
                                 const QJsonObject& after, const WriteContext& ctx, const QString& ts) {
  auto st = db_.prepare(QStringLiteral(
      "INSERT INTO changes (ts, actor, entityKind, entityId, before, after, requestId) "
      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"));
  if (!st) return st.error();
  st.value()
      .bind(1, ts)
      .bind(2, ctx.actor.isEmpty() ? QStringLiteral("system") : ctx.actor)
      .bind(3, kind)
      .bind(4, id)
      .bind(5, compactJson(before))
      .bind(6, compactJson(after))
      .bindNullable(7, ctx.requestId);
  return st.value().run();
}

Status SqliteStore::writeProject(const Project& p, bool insert) {
  const QString sql =
      insert ? QStringLiteral("INSERT INTO project (%1) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, "
                              "?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21)")
                   .arg(kProjectColumns)
             : QStringLiteral(
                   "UPDATE project SET createdAt = ?2, updatedAt = ?3, deleted = ?4, rev = ?5, name = ?6, "
                   "description = ?7, objectives = ?8, scope = ?9, outOfScope = ?10, successCriteria = ?11, "
                   "owner = ?12, sponsor = ?13, status = ?14, rag = ?15, priority = ?16, start = ?17, "
                   "finish = ?18, budget = ?19, color = ?20, ord = ?21 WHERE id = ?1");
  auto st = db_.prepare(sql);
  if (!st) return st.error();
  Statement& s = st.value();
  s.bind(1, p.id).bind(2, p.createdAt).bind(3, p.updatedAt).bind(4, p.deleted).bind(5, p.rev);
  s.bind(6, p.name).bind(7, p.description).bind(8, p.objectives).bind(9, p.scope);
  s.bind(10, p.outOfScope).bind(11, p.successCriteria).bind(12, p.owner).bind(13, p.sponsor);
  s.bind(14, p.status).bind(15, p.rag).bind(16, p.priority);
  s.bindNullable(17, p.start).bindNullable(18, p.finish);
  if (p.budget) {
    s.bind(19, *p.budget);
  } else {
    s.bindNull(19);
  }
  s.bindNullable(20, p.color).bind(21, p.order);
  return s.run();
}

Status SqliteStore::writeTask(const Task& t, bool insert) {
  const QString sql =
      insert ? QStringLiteral("INSERT INTO task (%1) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
                              "?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24)")
                   .arg(kTaskColumns)
             : QStringLiteral(
                   "UPDATE task SET createdAt = ?2, updatedAt = ?3, deleted = ?4, rev = ?5, project = ?6, "
                   "parent = ?7, ord = ?8, title = ?9, notes = ?10, type = ?11, mode = ?12, duration = ?13, "
                   "start = ?14, finish = ?15, constraintType = ?16, constraintDate = ?17, deadline = ?18, "
                   "percentComplete = ?19, actualStart = ?20, actualFinish = ?21, status = ?22, "
                   "priority = ?23, fixedCost = ?24 WHERE id = ?1");
  auto st = db_.prepare(sql);
  if (!st) return st.error();
  Statement& s = st.value();
  s.bind(1, t.id).bind(2, t.createdAt).bind(3, t.updatedAt).bind(4, t.deleted).bind(5, t.rev);
  s.bind(6, t.project).bindNullable(7, t.parent).bind(8, t.order).bind(9, t.title).bind(10, t.notes);
  s.bind(11, t.type).bind(12, t.mode).bind(13, t.duration);
  s.bindNullable(14, t.start).bindNullable(15, t.finish).bind(16, t.constraintType);
  s.bindNullable(17, t.constraintDate).bindNullable(18, t.deadline).bind(19, t.percentComplete);
  s.bindNullable(20, t.actualStart).bindNullable(21, t.actualFinish).bind(22, t.status);
  s.bind(23, t.priority).bind(24, t.fixedCost);
  return s.run();
}

// ---- projects ---------------------------------------------------------------

Result<Project> SqliteStore::createProject(const Project& in, const WriteContext& ctx) {
  if (auto e = toValidationError(validateProject(in))) return *e;
  Transaction tx(db_);
  if (!tx.ok()) return tx.error();

  Project p = in;
  p.id = newId();
  p.createdAt = p.updatedAt = nextTimestamp();
  p.deleted = false;
  p.rev = 1;
  if (p.order <= 0) {
    auto next = db_.scalar(QStringLiteral("SELECT COALESCE(MAX(ord), 0) + 1 FROM project"));
    if (!next) return next.error();
    p.order = next->toLongLong();
  }
  if (auto s = writeProject(p, true); !s) return s.error();
  const FieldDiff d = diffFields({}, toJson(p));
  if (auto s = appendChange(kProject, p.id, d.before, d.after, ctx, p.updatedAt); !s) return s.error();
  if (auto s = tx.commit(); !s) return s.error();
  return p;
}

Result<Project> SqliteStore::project(const QString& id, bool includeDeleted) {
  auto st = db_.prepare(QStringLiteral("SELECT %1 FROM project WHERE id = ?1").arg(kProjectColumns));
  if (!st) return st.error();
  st.value().bind(1, id);
  Error err;
  if (!st.value().step(&err)) {
    if (!err.code.isEmpty()) return err;
    return Error::notFound(QStringLiteral("project %1 not found").arg(id));
  }
  Project p = readProject(st.value());
  if (p.deleted && !includeDeleted) return Error::notFound(QStringLiteral("project %1 is deleted").arg(id));
  return p;
}

Result<QList<Project>> SqliteStore::projects(bool includeDeleted) {
  auto st = db_.prepare(QStringLiteral("SELECT %1 FROM project %2 ORDER BY ord, createdAt, id")
                            .arg(kProjectColumns, includeDeleted ? QString() : QStringLiteral("WHERE deleted = 0")));
  if (!st) return st.error();
  QList<Project> out;
  Error err;
  while (st.value().step(&err)) out.append(readProject(st.value()));
  if (!err.code.isEmpty()) return err;
  return out;
}

Result<Project> SqliteStore::updateProject(const Project& in, qint64 expectedRev, const WriteContext& ctx) {
  Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  auto cur = project(in.id);
  if (!cur) return cur.error();
  if (cur->rev != expectedRev) return Error::revMismatch(expectedRev, cur->rev);

  Project p = in;
  p.createdAt = cur->createdAt;
  p.deleted = false;
  p.rev = cur->rev;
  p.updatedAt = cur->updatedAt;
  if (auto e = toValidationError(validateProject(p))) return *e;
  const FieldDiff d = diffFields(toJson(*cur), toJson(p));
  if (d.empty()) return *cur;  // nothing changed: no new revision

  p.rev = cur->rev + 1;
  p.updatedAt = nextTimestamp();
  if (auto s = writeProject(p, false); !s) return s.error();
  if (auto s = appendChange(kProject, p.id, d.before, d.after, ctx, p.updatedAt); !s) return s.error();
  if (auto s = tx.commit(); !s) return s.error();
  return p;
}

Result<Project> SqliteStore::deleteProject(const QString& id, qint64 expectedRev, const WriteContext& ctx) {
  Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  auto cur = project(id);
  if (!cur) return cur.error();
  if (cur->rev != expectedRev) return Error::revMismatch(expectedRev, cur->rev);

  const QString ts = nextTimestamp();
  auto all = tasks(id);
  if (!all) return all.error();
  for (const Task& t : all.value()) {
    if (auto s = tombstoneTask(t, ctx, ts); !s) return s.error();
  }
  Project p = *cur;
  p.deleted = true;
  p.rev = cur->rev + 1;
  p.updatedAt = ts;
  if (auto s = writeProject(p, false); !s) return s.error();
  const FieldDiff d = diffFields(toJson(*cur), toJson(p));
  if (auto s = appendChange(kProject, p.id, d.before, d.after, ctx, ts); !s) return s.error();
  if (auto s = tx.commit(); !s) return s.error();
  return p;
}

// ---- tasks ------------------------------------------------------------------

Status SqliteStore::checkParent(const Task& t) {
  if (t.parent.isEmpty()) return Ok{};
  auto parent = task(t.parent);
  if (!parent) {
    return Error::validation(QStringLiteral("parent %1 not found").arg(t.parent),
                             QJsonObject{{QStringLiteral("field"), QStringLiteral("parent")}});
  }
  if (parent->project != t.project) {
    return Error::validation(QStringLiteral("parent %1 is in another project").arg(t.parent),
                             QJsonObject{{QStringLiteral("field"), QStringLiteral("parent")}});
  }
  if (parent->type == QLatin1String("milestone")) {
    return Error::validation(QStringLiteral("a milestone cannot have children"),
                             QJsonObject{{QStringLiteral("field"), QStringLiteral("parent")}});
  }
  // A task cannot be its own ancestor (SPEC.md §5.2): walk up from the new parent.
  if (!t.id.isEmpty()) {
    QStringList path{t.id};
    QString cursor = t.parent;
    while (!cursor.isEmpty()) {
      path.append(cursor);
      if (cursor == t.id) {
        QJsonArray cycle;
        for (const QString& id : path) cycle.append(id);
        return Error::cycle(QStringLiteral("task %1 would become its own ancestor").arg(t.id),
                            QJsonObject{{QStringLiteral("cycle"), cycle}});
      }
      auto up = task(cursor, true);
      if (!up) return up.error();
      cursor = up->parent;
      if (path.size() > 100000) return Error::internal(QStringLiteral("task tree too deep"));
    }
  }
  return Ok{};
}

Result<Task> SqliteStore::createTask(const Task& in, const WriteContext& ctx) {
  if (auto e = toValidationError(validateTask(in))) return *e;
  Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  auto proj = project(in.project);
  if (!proj) return proj.error();

  Task t = in;
  t.id.clear();
  if (auto s = checkParent(t); !s) return s.error();
  t.id = newId();
  t.createdAt = t.updatedAt = nextTimestamp();
  t.deleted = false;
  t.rev = 1;
  if (t.order <= 0) {
    auto st = db_.prepare(QStringLiteral(
        "SELECT COALESCE(MAX(ord), 0) + 1 FROM task WHERE project = ?1 AND parent IS ?2"));
    if (!st) return st.error();
    st.value().bind(1, t.project).bindNullable(2, t.parent);
    Error err;
    if (!st.value().step(&err)) return err.code.isEmpty() ? Error::internal(QStringLiteral("no order")) : err;
    t.order = st.value().int64(0);
  }
  if (auto s = writeTask(t, true); !s) return s.error();
  const FieldDiff d = diffFields({}, toJson(t));
  if (auto s = appendChange(kTask, t.id, d.before, d.after, ctx, t.updatedAt); !s) return s.error();
  if (auto s = rollUp(t.project, ctx); !s) return s.error();
  if (auto s = tx.commit(); !s) return s.error();
  return t;
}

Result<Task> SqliteStore::task(const QString& id, bool includeDeleted) {
  auto st = db_.prepare(QStringLiteral("SELECT %1 FROM task WHERE id = ?1").arg(kTaskColumns));
  if (!st) return st.error();
  st.value().bind(1, id);
  Error err;
  if (!st.value().step(&err)) {
    if (!err.code.isEmpty()) return err;
    return Error::notFound(QStringLiteral("task %1 not found").arg(id));
  }
  Task t = readTask(st.value());
  if (t.deleted && !includeDeleted) return Error::notFound(QStringLiteral("task %1 is deleted").arg(id));
  return t;
}

Result<QList<Task>> SqliteStore::tasks(const QString& projectId, bool includeDeleted) {
  auto st = db_.prepare(QStringLiteral("SELECT %1 FROM task WHERE project = ?1 %2 ORDER BY ord, createdAt, id")
                            .arg(kTaskColumns, includeDeleted ? QString() : QStringLiteral("AND deleted = 0")));
  if (!st) return st.error();
  st.value().bind(1, projectId);
  QList<Task> rows;
  Error err;
  while (st.value().step(&err)) rows.append(readTask(st.value()));
  if (!err.code.isEmpty()) return err;

  // Depth-first WBS order: parent, then its children by order.
  QHash<QString, QList<qsizetype>> children;
  QSet<QString> present;
  for (const Task& t : rows) present.insert(t.id);
  for (qsizetype i = 0; i < rows.size(); ++i) {
    const QString key = present.contains(rows[i].parent) ? rows[i].parent : QString();
    children[key].append(i);
  }
  QList<Task> out;
  out.reserve(rows.size());
  std::function<void(const QString&)> visit = [&](const QString& parent) {
    for (qsizetype i : children.value(parent)) {
      out.append(rows[i]);
      visit(rows[i].id);
    }
  };
  visit(QString());
  return out;
}

Result<Task> SqliteStore::updateTask(const Task& in, qint64 expectedRev, const WriteContext& ctx) {
  Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  auto cur = task(in.id);
  if (!cur) return cur.error();
  if (cur->rev != expectedRev) return Error::revMismatch(expectedRev, cur->rev);
  if (!in.project.isEmpty() && in.project != cur->project) {
    return Error::validation(QStringLiteral("a task cannot move to another project"),
                             QJsonObject{{QStringLiteral("field"), QStringLiteral("project")}});
  }

  Task t = in;
  t.project = cur->project;
  t.createdAt = cur->createdAt;
  t.updatedAt = cur->updatedAt;
  t.deleted = false;
  t.rev = cur->rev;
  if (auto e = toValidationError(validateTask(t))) return *e;
  if (t.parent != cur->parent) {
    if (auto s = checkParent(t); !s) return s.error();
  }
  auto n = db_.prepare(QStringLiteral("SELECT COUNT(*) FROM task WHERE parent = ?1 AND deleted = 0"));
  if (!n) return n.error();
  n.value().bind(1, t.id);
  n.value().step();
  const bool isSummary = n.value().int64(0) > 0;
  if (isSummary && t.type == QLatin1String("milestone") && cur->type != QLatin1String("milestone")) {
    return Error::validation(QStringLiteral("a task with children cannot become a milestone"),
                             QJsonObject{{QStringLiteral("field"), QStringLiteral("type")}});
  }
  // A summary's progress fields are derived (SPEC.md §5.3): refuse a change.
  if (isSummary) {
    if (auto e = toValidationError(checkSummaryEdit(*cur, t))) return *e;
  }
  const FieldDiff d = diffFields(toJson(*cur), toJson(t));
  if (d.empty()) return *cur;

  t.rev = cur->rev + 1;
  t.updatedAt = nextTimestamp();
  if (auto s = writeTask(t, false); !s) return s.error();
  if (auto s = appendChange(kTask, t.id, d.before, d.after, ctx, t.updatedAt); !s) return s.error();
  if (auto s = rollUp(t.project, ctx); !s) return s.error();
  if (auto s = tx.commit(); !s) return s.error();
  return task(t.id);
}

Status SqliteStore::tombstoneTask(const Task& cur, const WriteContext& ctx, const QString& ts) {
  Task t = cur;
  t.deleted = true;
  t.rev = cur.rev + 1;
  t.updatedAt = ts;
  if (auto s = writeTask(t, false); !s) return s;
  const FieldDiff d = diffFields(toJson(cur), toJson(t));
  return appendChange(kTask, t.id, d.before, d.after, ctx, ts);
}

Result<Task> SqliteStore::deleteTask(const QString& id, qint64 expectedRev, const WriteContext& ctx) {
  Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  auto cur = task(id);
  if (!cur) return cur.error();
  if (cur->rev != expectedRev) return Error::revMismatch(expectedRev, cur->rev);

  // The task and its live subtree, deepest first.
  auto all = tasks(cur->project);
  if (!all) return all.error();
  QList<Task> subtree;
  QSet<QString> inSubtree{id};
  for (const Task& t : all.value()) {  // depth-first order: parents come first
    if (t.id == id || inSubtree.contains(t.parent)) {
      inSubtree.insert(t.id);
      subtree.append(t);
    }
  }
  const QString ts = nextTimestamp();
  for (auto it = subtree.crbegin(); it != subtree.crend(); ++it) {
    if (auto s = tombstoneTask(*it, ctx, ts); !s) return s.error();
  }
  if (auto s = rollUp(cur->project, ctx); !s) return s.error();
  if (auto s = tx.commit(); !s) return s.error();
  return task(id, true);
}

// ---- move and restore -------------------------------------------------------

Status SqliteStore::writeRevision(const Task& cur, Task t, const WriteContext& ctx, const QString& ts) {
  t.rev = cur.rev + 1;
  t.updatedAt = ts;
  if (auto s = writeTask(t, false); !s) return s;
  const FieldDiff d = diffFields(toJson(cur), toJson(t));
  return appendChange(kTask, t.id, d.before, d.after, ctx, ts);
}

Result<QList<Task>> SqliteStore::planMove(const QString& id, const QString& parent, std::optional<int> position) {
  auto cur = task(id);
  if (!cur) return cur.error();
  auto all = tasks(cur->project);
  if (!all) return all.error();
  Task moved;  // copied from the project's list (keeps clang-analyzer's view of Result simple)
  for (const Task& t : all.value()) {
    if (t.id == id) moved = t;
  }
  moved.parent = parent;
  if (moved.parent != cur->parent) {
    if (auto s = checkParent(moved); !s) return s.error();
  }
  QList<Task> siblings;  // the destination's live children in order, without the task
  qsizetype currentIndex = -1;
  for (const Task& t : all.value()) {
    if (t.parent != moved.parent) continue;
    if (t.id == id) {
      currentIndex = siblings.size();
      continue;
    }
    siblings.append(t);
  }
  const qsizetype n = siblings.size();
  const qsizetype at = position ? *position : n;
  if (at < 0 || at > n) {
    return Error::validation(QStringLiteral("position must be between 0 and %1").arg(n),
                             QJsonObject{{QStringLiteral("field"), QStringLiteral("position")},
                                         {QStringLiteral("max"), static_cast<qint64>(n)}});
  }
  // Same parent, same place: nothing to write (no revisions, no renumbering).
  if (moved.parent == cur->parent && currentIndex == at) return QList<Task>{};

  siblings.insert(at, moved);
  QList<Task> writes;
  for (qsizetype i = 0; i < siblings.size(); ++i) {
    Task t = siblings[i];
    const qint64 ord = i + 1;
    if (t.id == id) {
      t.order = ord;
      if (t.parent != cur->parent || t.order != cur->order) writes.prepend(t);
    } else if (t.order != ord) {
      t.order = ord;
      writes.append(t);
    }
  }
  return writes;
}

Result<Task> SqliteStore::moveTask(const QString& id, const QString& parent, std::optional<int> position,
                                   qint64 expectedRev, const WriteContext& ctx) {
  Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  auto cur = task(id);
  if (!cur) return cur.error();
  if (cur->rev != expectedRev) return Error::revMismatch(expectedRev, cur->rev);
  auto plan = planMove(id, parent, position);
  if (!plan) return plan.error();
  if (plan->isEmpty()) return cur.value();
  const QString ts = nextTimestamp();
  for (const Task& t : plan.value()) {
    auto now = task(t.id);
    if (!now) return now.error();
    if (auto s = writeRevision(now.value(), t, ctx, ts); !s) return s.error();
  }
  if (auto s = rollUp(cur->project, ctx); !s) return s.error();
  if (auto s = tx.commit(); !s) return s.error();
  return task(id);
}

Result<QString> SqliteStore::tombstoneTs(const QString& id) {
  auto st = db_.prepare(QStringLiteral(
      "SELECT ts, after FROM changes WHERE entityKind = ?1 AND entityId = ?2 ORDER BY seq DESC LIMIT 1"));
  if (!st) return st.error();
  st.value().bind(1, kTask).bind(2, id);
  Error err;
  if (!st.value().step(&err)) {
    if (!err.code.isEmpty()) return err;
    return QString();
  }
  const QJsonObject after = QJsonDocument::fromJson(st.value().text(1).toUtf8()).object();
  if (after.value(QStringLiteral("deleted")).toBool()) return st.value().text(0);
  return QString();
}

Result<QList<Task>> SqliteStore::planRestore(const QString& id) {
  auto cur = task(id, true);
  if (!cur) return cur.error();
  if (!cur->deleted) {
    return Error::conflict(QStringLiteral("task %1 is not deleted").arg(id),
                           QJsonObject{{QStringLiteral("reason"), QStringLiteral("not_deleted")}});
  }
  auto proj = project(cur->project, true);
  if (!proj) return proj.error();
  if (proj->deleted) {
    return Error::conflict(QStringLiteral("project %1 is deleted, so its tasks can't be restored").arg(proj->id),
                           QJsonObject{{QStringLiteral("reason"), QStringLiteral("project_deleted")},
                                       {QStringLiteral("project"), proj->id}});
  }
  if (!cur->parent.isEmpty()) {
    auto parent = task(cur->parent, true);
    if (!parent) return parent.error();
    if (parent->deleted) {
      return Error::conflict(QStringLiteral("the parent task %1 is deleted: restore it first").arg(parent->id),
                             QJsonObject{{QStringLiteral("reason"), QStringLiteral("parent_deleted")},
                                         {QStringLiteral("parent"), parent->id}});
    }
    if (parent->type == QLatin1String("milestone")) {
      return Error::conflict(QStringLiteral("the parent task %1 is now a milestone, which can't have children")
                                 .arg(parent->id),
                             QJsonObject{{QStringLiteral("reason"), QStringLiteral("parent_milestone")},
                                         {QStringLiteral("parent"), parent->id}});
    }
  }
  auto rootTs = tombstoneTs(id);
  if (!rootTs) return rootTs.error();

  // The deleted descendants tombstoned by the same delete (one timestamp per
  // delete request); a subtree deleted earlier keeps its own and stays deleted.
  auto all = tasks(cur->project, true);
  if (!all) return all.error();
  QList<Task> out{cur.value()};
  if (rootTs->isEmpty()) return out;
  QSet<QString> restored{id};
  for (const Task& t : all.value()) {  // depth-first: parents before children
    if (!t.deleted || t.id == id || !restored.contains(t.parent)) continue;
    auto ts = tombstoneTs(t.id);
    if (!ts) return ts.error();
    if (ts.value() == rootTs.value()) {
      restored.insert(t.id);
      out.append(t);
    }
  }
  return out;
}

Result<Task> SqliteStore::restoreTask(const QString& id, qint64 expectedRev, const WriteContext& ctx) {
  Transaction tx(db_);
  if (!tx.ok()) return tx.error();
  auto cur = task(id, true);
  if (!cur) return cur.error();
  if (cur->rev != expectedRev) return Error::revMismatch(expectedRev, cur->rev);
  auto plan = planRestore(id);
  if (!plan) return plan.error();
  const QString ts = nextTimestamp();
  for (const Task& t : plan.value()) {
    Task live = t;
    live.deleted = false;
    if (auto s = writeRevision(t, live, ctx, ts); !s) return s.error();
  }
  if (auto s = rollUp(cur->project, ctx); !s) return s.error();
  if (auto s = tx.commit(); !s) return s.error();
  return task(id);
}

// ---- sync feed --------------------------------------------------------------

namespace {

Result<QList<ChangeRecord>> readChanges(Statement& st) {
  QList<ChangeRecord> out;
  Error err;
  while (st.step(&err)) {
    ChangeRecord c;
    c.seq = st.int64(0);
    c.ts = st.text(1);
    c.actor = st.text(2);
    c.entityKind = st.text(3);
    c.entityId = st.text(4);
    c.before = QJsonDocument::fromJson(st.text(5).toUtf8()).object();
    c.after = QJsonDocument::fromJson(st.text(6).toUtf8()).object();
    c.requestId = st.text(7);
    out.append(c);
  }
  if (!err.code.isEmpty()) return err;
  return out;
}

}  // namespace

Result<QList<ChangeRecord>> SqliteStore::changesSince(qint64 since, int limit) {
  limit = std::clamp(limit, 1, 500);
  auto st = db_.prepare(QStringLiteral(
      "SELECT seq, ts, actor, entityKind, entityId, before, after, requestId FROM changes "
      "WHERE seq > ?1 ORDER BY seq LIMIT ?2"));
  if (!st) return st.error();
  st.value().bind(1, since).bind(2, limit);
  return readChanges(st.value());
}

Result<QList<ChangeRecord>> SqliteStore::changesFor(const QString& entityKind, const QString& entityId,
                                                    qint64 since, int limit) {
  limit = std::clamp(limit, 1, 500);
  auto st = db_.prepare(QStringLiteral(
      "SELECT seq, ts, actor, entityKind, entityId, before, after, requestId FROM changes "
      "WHERE entityKind = ?1 AND entityId = ?2 AND seq > ?3 ORDER BY seq LIMIT ?4"));
  if (!st) return st.error();
  st.value().bind(1, entityKind).bind(2, entityId).bind(3, since).bind(4, limit);
  return readChanges(st.value());
}

}  // namespace zproject::store
