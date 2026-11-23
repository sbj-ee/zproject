#include "app/ProjectActions.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QLocale>
#include <QPointer>
#include <QWidget>

#include "app/AppSession.hpp"

namespace zproject::app {

namespace {

const QString kProject = QStringLiteral("project");

QString changedList(const client::DryRunPreview::Change& c) {
  QStringList names;
  for (const QString& k : c.after.keys()) {
    if (k == QLatin1String("rev") || k == QLatin1String("updatedAt")) continue;
    names.append(fieldLabel(k));
  }
  return names.join(QStringLiteral(", "));
}

QString localTime(const QString& iso) {
  const QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs);
  if (!t.isValid()) return iso;
  return QLocale().toString(t.toLocalTime(), QLocale::ShortFormat);
}

}  // namespace

ProjectActions::ProjectActions(QWidget* window, SessionFn session, QObject* parent)
    : QObject(parent), window_(window), session_(std::move(session)) {}

void ProjectActions::say(const QString& text, bool error) {
  lastMessage_ = text;
  emit notice(text, error);
}

void ProjectActions::setBusy(bool busy) {
  busy_ = busy;
  if (ProjectEditor* e = editor_.data()) e->setBusy(busy);
}

QString ProjectActions::projectName(const QString& id) const {
  AppSession* s = session_();
  if (s != nullptr && s->cache() != nullptr) {
    if (auto p = s->cache()->project(id)) return p->value(QStringLiteral("name")).toString();
  }
  return id;
}

QString ProjectActions::taskName(const QString& id) const {
  AppSession* s = session_();
  if (s != nullptr && s->cache() != nullptr) {
    if (auto t = s->cache()->task(id)) return t->value(QStringLiteral("title")).toString();
  }
  return id;
}

// ---- open ----

ProjectEditor* ProjectActions::newProject() { return editProject(QString()); }

ProjectEditor* ProjectActions::editProject(const QString& id) {
  AppSession* s = session_();
  if (s == nullptr || !s->editingAllowed()) {
    say(tr("Editing is off until the server answers."), true);
    return nullptr;
  }
  if (ProjectEditor* open = editor_.data()) {
    open->raise();
    open->activateWindow();
    return open;
  }
  base_ = QJsonObject();
  if (!id.isEmpty()) {
    auto p = s->cache()->project(id);
    if (!p) {
      say(tr("That project is no longer in the list."), true);
      return nullptr;
    }
    base_ = *p;
  }
  auto* e = new ProjectEditor(base_, window_);
  editor_ = e;
  e->setAttribute(Qt::WA_DeleteOnClose);
  e->setWindowModality(Qt::WindowModal);
  connect(e, &ProjectEditor::saveRequested, this, [this] { save(); });
  e->open();
  if (!id.isEmpty()) {
    // The cache is built from the change feed, whose records carry no rev,
    // so the edit starts from the server's copy: its rev is the If-Match.
    e->setLoading(true);
    const QPointer<ProjectEditor> guard(e);
    s->api()->get(QStringLiteral("/v1/projects/") + id, {}, [this, guard](const client::ApiResponse& resp) {
      ProjectEditor* ed = guard.data();
      if (ed == nullptr) return;
      const client::ApiError err = client::classifyResponse(resp);
      if (!err.ok()) return fail(err);
      base_ = resp.json;
      ed->reload(resp.json);
    });
  }
  return e;
}

// ---- create and edit ----

void ProjectActions::save() {
  ProjectEditor* e = editor_.data();
  if (e == nullptr || busy_) return;
  const QJsonObject body = e->body();
  Write w;
  w.hasBody = true;
  w.body = body;
  if (e->isNew()) {
    w.method = "POST";
    w.path = QStringLiteral("/v1/projects");
  } else {
    if (body.isEmpty()) {
      e->accept();
      say(tr("Nothing changed."), false);
      return;
    }
    w.method = "PATCH";
    w.projectId = e->projectId();
    w.path = QStringLiteral("/v1/projects/") + w.projectId;
    w.rev = base_.value(QStringLiteral("rev")).toInteger();
  }
  runDryRun(w);
}

void ProjectActions::runDryRun(const Write& w) {
  AppSession* s = session_();
  if (s == nullptr || s->api() == nullptr) return;
  client::ApiRequest r;
  r.method = w.method;
  r.path = w.path;
  if (w.hasBody) r.body = w.body;
  if (w.rev > 0) r.ifMatch = w.rev;
  r.dryRun = true;
  setBusy(true);
  s->api()->send(r, [this, w](const client::ApiResponse& resp) {
    setBusy(false);
    const client::ApiError e = client::classifyResponse(resp);
    if (e.kind == client::ErrorKind::RevMismatch) return startConflict(w);
    if (!e.ok()) return fail(e);
    auto preview = client::DryRunPreview::parse(resp.json);
    if (!preview) return say(preview.error().message, true);
    if (!preview->needsPreview(false, kProject, w.projectId)) return commit(w, {});
    QList<PreviewDialog::Row> rows;
    for (const auto& c : preview->changes) {
      const bool isProject = c.entityKind == kProject;
      rows.append({c.op, c.entityKind,
                   isProject ? (c.entityId.isEmpty() ? w.body.value(QStringLiteral("name")).toString()
                                                     : projectName(c.entityId))
                             : taskName(c.entityId),
                   changedList(c)});
    }
    QWidget* parent = editor_ ? static_cast<QWidget*>(editor_.data()) : window_;
    auto* d = new PreviewDialog(tr("Review Changes"),
                                tr("Saving changes more than this project. Nothing is written until you confirm."),
                                rows, tr("Save"), parent);
    preview_ = d;
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowModality(Qt::WindowModal);
    connect(d, &QDialog::accepted, this, [this, w] { commit(w, {}); });
    d->open();
  });
}

void ProjectActions::commit(const Write& w, const QString& confirmId) {
  AppSession* s = session_();
  if (s == nullptr || s->api() == nullptr) return;
  client::ApiRequest r;
  r.method = w.method;
  r.path = w.path;
  if (w.hasBody) r.body = w.body;
  if (w.rev > 0) r.ifMatch = w.rev;
  r.confirmId = confirmId.toLatin1();
  setBusy(true);
  s->api()->send(r, [this, w](const client::ApiResponse& resp) {
    setBusy(false);
    const client::ApiError e = client::classifyResponse(resp);
    if (e.kind == client::ErrorKind::RevMismatch) {
      if (w.method == "DELETE") {
        const qint64 current =
            resp.json.value(QStringLiteral("details")).toObject().value(QStringLiteral("current")).toInteger();
        return deleteDryRun(w.projectId, current, tr("The project changed on the server after the preview."));
      }
      return startConflict(w);
    }
    if (e.kind == client::ErrorKind::ConfirmRequired && w.method == "DELETE") {
      return deleteDryRun(w.projectId, w.rev, tr("The confirmation expired or didn't match. Check the changes again."));
    }
    if (!e.ok()) return fail(e);
    const QString id = resp.json.value(QStringLiteral("id")).toString();
    const QString name = resp.json.value(QStringLiteral("name")).toString();
    if (w.method == "DELETE") {
      say(tr("Deleted project %1. It is a tombstone: its history is kept.").arg(name), false);
      emit deleted(id);
    } else {
      if (ProjectEditor* ed = editor_.data()) ed->accept();
      say(w.method == "POST" ? tr("Created project %1.").arg(name) : tr("Saved project %1.").arg(name), false);
      emit saved(id, w.method == "POST");
    }
    if (AppSession* now = session_()) now->refresh();
  });
}

// ---- 412 ----

void ProjectActions::startConflict(const Write& w) {
  AppSession* s = session_();
  if (s == nullptr || s->api() == nullptr) return;
  setBusy(true);
  s->api()->get(w.path, {}, [this, w](const client::ApiResponse& resp) {
    const client::ApiError e = client::classifyResponse(resp);
    if (!e.ok()) {
      setBusy(false);
      return fail(e);
    }
    const QJsonObject theirs = resp.json;
    const qint64 rev = theirs.value(QStringLiteral("rev")).toInteger();
    AppSession* now = session_();
    if (now == nullptr || now->api() == nullptr) return setBusy(false);
    // Who changed it: the newest feed record for it, newer than the cache;
    // else the cache's own copy of the feed; else just the time.
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("since"), QString::number(now->cache()->lastSeq()));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("500"));
    now->api()->get(QStringLiteral("/v1/changes"), q, [this, w, theirs, rev](const client::ApiResponse& feed) {
      setBusy(false);
      QString actor;
      QString ts;
      for (const QJsonValue& v : feed.json.value(QStringLiteral("items")).toArray()) {
        const QJsonObject c = v.toObject();
        if (c.value(QStringLiteral("entityKind")).toString() == kProject &&
            c.value(QStringLiteral("entityId")).toString() == w.projectId) {
          actor = c.value(QStringLiteral("actor")).toString();
          ts = c.value(QStringLiteral("ts")).toString();
        }
      }
      if (actor.isEmpty()) {
        if (AppSession* s2 = session_(); s2 != nullptr && s2->cache() != nullptr) {
          for (const client::ChangeRecord& c : s2->cache()->activity(w.projectId, 200)) {
            if (c.entityKind == kProject && c.entityId == w.projectId) {  // newest first
              actor = c.actor;
              ts = c.ts;
              break;
            }
          }
        }
      }
      QString who;
      if (!actor.isEmpty()) {
        who = tr("Changed by %1 at %2 (revision %3)").arg(actor, localTime(ts)).arg(rev);
      } else {
        who = tr("Changed at %1 (revision %2)").arg(localTime(theirs.value(QStringLiteral("updatedAt")).toString())).arg(rev);
      }
      showConflict(w, theirs, who);
    });
  });
}

void ProjectActions::showConflict(const Write& w, const QJsonObject& theirs, const QString& who) {
  QJsonObject mine = base_;
  for (auto it = w.body.constBegin(); it != w.body.constEnd(); ++it) mine.insert(it.key(), it.value());
  const client::ConflictMerge merge(base_, mine, theirs);
  QWidget* parent = editor_ ? static_cast<QWidget*>(editor_.data()) : window_;
  auto* d = new ConflictDialog(tr("project %1").arg(theirs.value(QStringLiteral("name")).toString()), merge, who, parent);
  conflict_ = d;
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setWindowModality(Qt::WindowModal);
  connect(d, &ConflictDialog::chosen, this, [this, w, merge, theirs](ConflictDialog::Choice c) {
    if (c == ConflictDialog::Choice::Cancel) {
      if (ProjectEditor* e = editor_.data()) {
        e->setMessage(tr("Not saved: the project changed on the server. Save again to review the differences."));
      }
      say(tr("Not saved: the project changed on the server."), false);
      return;
    }
    base_ = theirs;  // from now on, edits start from the server's version
    Write next = w;
    next.body = merge.resolution(c == ConflictDialog::Choice::KeepMine);
    next.rev = merge.theirRev();
    if (next.body.isEmpty()) {
      if (ProjectEditor* e = editor_.data()) e->accept();
      say(tr("Kept the server's version; nothing to send."), false);
      if (AppSession* s = session_()) s->refresh();
      return;
    }
    runDryRun(next);  // again: a dry run first, on the new revision
  });
  d->open();
}

// ---- delete ----

void ProjectActions::deleteProject(const QString& id) {
  AppSession* s = session_();
  if (s == nullptr || !s->editingAllowed()) {
    say(tr("Editing is off until the server answers."), true);
    return;
  }
  if (!s->cache()->project(id)) {
    say(tr("That project is no longer in the list."), true);
    return;
  }
  if (busy_) return;
  // The server's rev (the cache has none): the preview and the delete are
  // both bound to it.
  setBusy(true);
  s->api()->get(QStringLiteral("/v1/projects/") + id, {}, [this, id](const client::ApiResponse& resp) {
    setBusy(false);
    const client::ApiError err = client::classifyResponse(resp);
    if (!err.ok()) return fail(err);
    deleteDryRun(id, resp.json.value(QStringLiteral("rev")).toInteger(), QString());
  });
}

void ProjectActions::deleteDryRun(const QString& id, qint64 rev, const QString& note) {
  AppSession* s = session_();
  if (s == nullptr || s->api() == nullptr || busy_) return;
  Write w;
  w.method = "DELETE";
  w.path = QStringLiteral("/v1/projects/") + id;
  w.rev = rev;
  w.projectId = id;
  client::ApiRequest r;
  r.method = w.method;
  r.path = w.path;
  if (rev > 0) r.ifMatch = rev;
  r.dryRun = true;
  setBusy(true);
  s->api()->send(r, [this, w, note](const client::ApiResponse& resp) {
    setBusy(false);
    const client::ApiError e = client::classifyResponse(resp);
    if (e.kind == client::ErrorKind::RevMismatch) {
      say(tr("The project changed on the server since this computer last refreshed. Refreshing; try again."), true);
      if (AppSession* now = session_()) now->refresh();
      return;
    }
    if (!e.ok()) return fail(e);
    auto preview = client::DryRunPreview::parse(resp.json);
    if (!preview) return say(preview.error().message, true);
    QList<PreviewDialog::Row> rows;
    int tasks = 0;
    for (const auto& c : preview->changes) {
      const bool isProject = c.entityKind == kProject;
      if (!isProject) ++tasks;
      rows.append({c.op, c.entityKind, isProject ? projectName(c.entityId) : taskName(c.entityId), tr("deleted")});
    }
    const QString name = projectName(w.projectId);
    QString intro = note.isEmpty() ? QString() : note + QStringLiteral("\n\n");
    intro += tasks == 0 ? tr("Delete project %1? It has no tasks.").arg(name)
                        : (tasks == 1 ? tr("Delete project %1 and its 1 task?").arg(name)
                                      : tr("Delete project %1 and its %2 tasks?").arg(name).arg(tasks));
    intro += QLatin1Char(' ') + tr("Deleted items become tombstones: the history keeps them, and they leave every list.");
    auto* d = new PreviewDialog(tr("Delete Project"), intro, rows, tr("Delete Project"), window_);
    preview_ = d;
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowModality(Qt::WindowModal);
    const QString confirmId = preview->confirmId;
    connect(d, &QDialog::accepted, this, [this, w, confirmId] { commit(w, confirmId); });
    d->open();
  });
}

// ---- errors (plan §4.3) ----

void ProjectActions::fail(const client::ApiError& e) {
  using K = client::ErrorKind;
  ProjectEditor* ed = editor_.data();
  QString text;
  switch (e.kind) {
    case K::Validation:
    case K::SummaryRollup:
      text = tr("The server refused the change: %1").arg(e.message);
      if (ed != nullptr && !e.field.isEmpty()) ed->showFieldError(e.field, e.message);
      break;
    case K::ForbiddenScope:
    case K::ForbiddenProject:
      text = tr("%1 This computer's token can't change projects; nothing was saved.").arg(e.message);
      break;
    case K::NotFound:
      text = tr("This project was deleted elsewhere. The list is being refreshed.");
      if (ed != nullptr) ed->reject();
      ed = nullptr;
      if (AppSession* s = session_()) s->refresh();
      break;
    case K::RateLimited:
      text = tr("Too many requests: wait %1 s and try again. Nothing was saved.").arg(std::max(1, e.retryAfterSeconds));
      break;
    case K::Unauthenticated:
      text = tr("%1 Nothing was saved.").arg(e.message);
      if (AppSession* s = session_()) s->refresh();
      break;
    case K::Network:
    case K::Timeout:
    case K::Server:
      text = tr("The server can't be reached, so nothing was saved (%1).").arg(e.message);
      if (AppSession* s = session_()) s->refresh();
      break;
    default:
      text = tr("Not saved: %1").arg(e.message);
      break;
  }
  if (ed != nullptr) ed->setMessage(text);
  say(text, true);
}

}  // namespace zproject::app
