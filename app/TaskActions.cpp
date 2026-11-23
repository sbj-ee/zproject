#include "app/TaskActions.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QLocale>
#include <QWidget>

#include <algorithm>

#include "app/AppSession.hpp"
#include "app/EditDialogs.hpp"
#include "app/OutlineModel.hpp"
#include "client/EditFlow.hpp"

namespace zproject::app {

namespace {

const QString kTask = QStringLiteral("task");

QString changeDetail(const client::DryRunPreview::Change& c) {
  if (c.op == QLatin1String("delete")) return QObject::tr("deleted");
  if (c.op == QLatin1String("restore")) return QObject::tr("restored");
  if (c.op == QLatin1String("create")) return QObject::tr("created");
  QStringList names;
  for (const QString& k : c.after.keys()) {
    if (k == QLatin1String("rev") || k == QLatin1String("updatedAt")) continue;
    const QString before = client::displayValue(c.before.value(k));
    const QString after = client::displayValue(c.after.value(k));
    if (k == QLatin1String("parent") || k == QLatin1String("order")) {
      names.append(fieldLabel(k));
    } else {
      names.append(QStringLiteral("%1 %2 → %3")
                       .arg(fieldLabel(k), before.isEmpty() ? QStringLiteral("—") : before,
                            after.isEmpty() ? QStringLiteral("—") : after));
    }
  }
  const QString list = names.join(QStringLiteral(", "));
  return c.op == QLatin1String("move") ? QObject::tr("moved (%1)").arg(list) : list;
}

}  // namespace

TaskActions::TaskActions(QWidget* window, SessionFn session, OutlineModel* model, QObject* parent)
    : QObject(parent), window_(window), session_(std::move(session)), model_(model) {}

void TaskActions::say(const QString& text, bool error) {
  lastMessage_ = text;
  emit notice(text, error);
}

QString TaskActions::label(const QString& id) const {
  if (model_->contains(id)) {
    return QStringLiteral("%1 %2").arg(model_->wbsCode(id), model_->task(id).value(QStringLiteral("title")).toString());
  }
  for (const Deleted& d : deleted_) {
    if (d.id == id) return d.label;
  }
  if (AppSession* s = session_(); s != nullptr && s->cache() != nullptr) {
    if (auto t = s->cache()->task(id)) return t->value(QStringLiteral("title")).toString();
  }
  return id;
}

// ---- what is allowed ----

bool TaskActions::canIndent(const QString& id) const {
  const int i = model_->siblingIndex(id);
  if (i <= 0) return false;
  const QString prev = model_->childIds(model_->parentId(id)).value(i - 1);
  return !model_->isMilestone(prev);  // milestones have no children (SPEC.md §5.2)
}
bool TaskActions::canOutdent(const QString& id) const { return model_->contains(id) && !model_->parentId(id).isEmpty(); }
bool TaskActions::canMoveUp(const QString& id) const { return model_->siblingIndex(id) > 0; }
bool TaskActions::canMoveDown(const QString& id) const {
  const int i = model_->siblingIndex(id);
  return i >= 0 && i + 1 < model_->childIds(model_->parentId(id)).size();
}
bool TaskActions::canToggleMilestone(const QString& id) const {
  return model_->contains(id) && !model_->isSummary(id);
}

// ---- the actions ----

void TaskActions::insertBelow(const QString& projectId, const QString& currentId) {
  if (projectId.isEmpty()) return;
  const QString parent = model_->contains(currentId) ? model_->parentId(currentId) : QString();
  const int siblings = static_cast<int>(model_->childIds(parent).size());
  const int position = model_->contains(currentId) ? model_->siblingIndex(currentId) + 1 : siblings;
  Op op;
  op.what = tr("Insert a task");
  op.method = "POST";
  op.path = QStringLiteral("/v1/projects/%1/tasks").arg(projectId);
  op.body = QJsonObject{{QStringLiteral("title"), tr("New task")}};
  if (!parent.isEmpty()) op.body.insert(QStringLiteral("parent"), parent);
  op.hasBody = true;
  op.previewTitle = tr("Insert Task");
  op.previewIntro = tr("Adding a task changes more than the task itself. Nothing is written until you confirm.");
  op.confirmText = tr("Insert");
  op.editTitle = true;
  op.then = [this, position, siblings](const QJsonObject& created) {
    const QString id = created.value(QStringLiteral("id")).toString();
    Op move;
    move.what = tr("Insert a task");
    move.done = tr("Inserted a task. Type its title.");
    move.selectAfter = id;
    move.editTitle = true;
    if (position >= siblings) {  // created last: already in place
      finish(move);
      return;
    }
    move.method = "POST";
    move.path = QStringLiteral("/v1/tasks/%1/move").arg(id);
    move.body = QJsonObject{{QStringLiteral("position"), position}};
    move.hasBody = true;
    move.taskId = id;
    move.rev = created.value(QStringLiteral("rev")).toInteger();
    move.base = created;
    dryRun(move);
  };
  start(op);
}

void TaskActions::indent(const QString& id) {
  if (!canIndent(id)) return;
  const QString prev = model_->childIds(model_->parentId(id)).value(model_->siblingIndex(id) - 1);
  Op op;
  op.what = tr("Indent %1").arg(label(id));
  op.done = tr("Indented %1 under %2.").arg(label(id), label(prev));
  op.method = "POST";
  op.path = QStringLiteral("/v1/tasks/%1/move").arg(id);
  op.body = QJsonObject{{QStringLiteral("parent"), prev}};
  op.hasBody = true;
  op.taskId = id;
  op.previewTitle = tr("Indent Task");
  op.previewIntro = tr("Indenting %1 puts it under %2, which then rolls up its progress. Nothing is written "
                       "until you confirm.").arg(label(id), label(prev));
  op.confirmText = tr("Indent");
  op.selectAfter = id;
  start(op);
}

void TaskActions::outdent(const QString& id) {
  if (!canOutdent(id)) return;
  const QString parent = model_->parentId(id);
  const QString grand = model_->parentId(parent);
  Op op;
  op.what = tr("Outdent %1").arg(label(id));
  op.done = tr("Outdented %1.").arg(label(id));
  op.method = "POST";
  op.path = QStringLiteral("/v1/tasks/%1/move").arg(id);
  op.body = QJsonObject{{QStringLiteral("parent"), grand.isEmpty() ? QJsonValue() : QJsonValue(grand)},
                        {QStringLiteral("position"), model_->siblingIndex(parent) + 1}};
  op.hasBody = true;
  op.taskId = id;
  op.previewTitle = tr("Outdent Task");
  op.previewIntro = tr("Outdenting %1 changes what %2 rolls up. Nothing is written until you confirm.")
                        .arg(label(id), label(parent));
  op.confirmText = tr("Outdent");
  op.selectAfter = id;
  start(op);
}

void TaskActions::moveUp(const QString& id) {
  if (!canMoveUp(id)) return;
  Op op;
  op.what = tr("Move %1 up").arg(label(id));
  op.done = tr("Moved %1 up.").arg(label(id));
  op.method = "POST";
  op.path = QStringLiteral("/v1/tasks/%1/move").arg(id);
  op.body = QJsonObject{{QStringLiteral("position"), model_->siblingIndex(id) - 1}};
  op.hasBody = true;
  op.taskId = id;
  op.selectAfter = id;
  start(op);
}

void TaskActions::moveDown(const QString& id) {
  if (!canMoveDown(id)) return;
  Op op;
  op.what = tr("Move %1 down").arg(label(id));
  op.done = tr("Moved %1 down.").arg(label(id));
  op.method = "POST";
  op.path = QStringLiteral("/v1/tasks/%1/move").arg(id);
  op.body = QJsonObject{{QStringLiteral("position"), model_->siblingIndex(id) + 1}};
  op.hasBody = true;
  op.taskId = id;
  op.selectAfter = id;
  start(op);
}

void TaskActions::deleteTask(const QString& id) {
  if (!model_->contains(id)) return;
  const QString name = label(id);
  // Something to select afterwards: the next sibling, else the previous, else the parent.
  const QStringList sib = model_->childIds(model_->parentId(id));
  const int i = model_->siblingIndex(id);
  const QString next = i + 1 < sib.size() ? sib.value(i + 1) : (i > 0 ? sib.value(i - 1) : model_->parentId(id));
  Op op;
  op.what = tr("Delete %1").arg(name);
  op.done = tr("Deleted %1. Edit → Restore brings it back.").arg(name);
  op.method = "DELETE";
  op.path = QStringLiteral("/v1/tasks/") + id;
  op.taskId = id;
  op.destructive = true;
  op.previewTitle = tr("Delete Task");
  const int kids = model_->liveChildren(id);
  op.previewIntro = (kids == 0 ? tr("Delete %1?").arg(name) : tr("Delete %1 and everything under it?").arg(name)) +
                    QLatin1Char(' ') +
                    tr("Deleted tasks are tombstones: Edit → Restore brings them back, and the history keeps them.");
  op.confirmText = tr("Delete");
  op.selectAfter = next;
  op.then = [this, id, name, op](const QJsonObject&) {
    deleted_.append({id, name});
    emit deletedChanged();
    Op shown = op;
    shown.then = nullptr;
    finish(shown);
  };
  start(op);
}

void TaskActions::restoreLast() {
  if (deleted_.isEmpty()) return;
  const Deleted d = deleted_.last();
  Op op;
  op.what = tr("Restore %1").arg(d.label);
  op.done = tr("Restored %1.").arg(d.label);
  op.method = "POST";
  op.path = QStringLiteral("/v1/tasks/%1/restore").arg(d.id);
  op.destructive = true;  // the server treats restore as destructive (SPEC.md §9.3)
  op.previewTitle = tr("Restore Task");
  op.previewIntro = tr("Restore %1 and the subtasks deleted with it?").arg(d.label);
  op.confirmText = tr("Restore");
  op.selectAfter = d.id;
  op.then = [this, d, op](const QJsonObject&) {
    for (qsizetype i = 0; i < deleted_.size(); ++i) {
      if (deleted_[i].id == d.id) {
        deleted_.removeAt(i);
        break;
      }
    }
    emit deletedChanged();
    Op shown = op;
    shown.then = nullptr;
    finish(shown);
  };
  start(op);
}

void TaskActions::toggleMilestone(const QString& id) {
  if (!canToggleMilestone(id)) return;
  const bool toMilestone = !model_->isMilestone(id);
  Op op;
  op.what = toMilestone ? tr("Convert %1 to a milestone").arg(label(id)) : tr("Convert %1 to a task").arg(label(id));
  op.done = toMilestone ? tr("%1 is a milestone now.").arg(label(id)) : tr("%1 is a task now.").arg(label(id));
  op.method = "PATCH";
  op.path = QStringLiteral("/v1/tasks/") + id;
  // A milestone has duration 0 (SPEC.md §5.2); back to a task: one day.
  op.body = toMilestone ? QJsonObject{{QStringLiteral("type"), QStringLiteral("milestone")}, {QStringLiteral("duration"), 0}}
                        : QJsonObject{{QStringLiteral("type"), QStringLiteral("task")}, {QStringLiteral("duration"), 1}};
  op.hasBody = true;
  op.taskId = id;
  op.edit = true;
  op.previewTitle = tr("Review Changes");
  op.previewIntro = tr("This changes more than %1. Nothing is written until you confirm.").arg(label(id));
  op.confirmText = tr("Save");
  op.selectAfter = id;
  start(op);
}

void TaskActions::edit(const QString& id, const QString& field, const QJsonValue& value) {
  if (!model_->contains(id)) return;
  Op op;
  op.what = tr("Change %1 of %2").arg(fieldLabel(field).toLower(), label(id));
  op.done = tr("Saved %1.").arg(label(id));
  op.method = "PATCH";
  op.path = QStringLiteral("/v1/tasks/") + id;
  op.body = QJsonObject{{field, value}};
  op.hasBody = true;
  op.taskId = id;
  op.edit = true;
  op.previewTitle = tr("Review Changes");
  op.previewIntro = tr("Saving %1 changes more than this task (summaries roll up). Nothing is written until you "
                       "confirm.").arg(label(id));
  op.confirmText = tr("Save");
  op.selectAfter = id;
  start(op);
}

// ---- the write flow ----

void TaskActions::start(Op op) {
  AppSession* s = session_();
  if (s == nullptr || s->api() == nullptr || !s->editingAllowed()) {
    say(tr("Editing is off until the server answers."), true);
    return;
  }
  if (busy_) {
    say(tr("Still saving the last change; try again in a moment."), true);
    return;
  }
  setBusy(true);
  if (op.taskId.isEmpty()) {
    dryRun(std::move(op));
    return;
  }
  // The server's copy: its rev is the If-Match (the cache has no rev).
  s->api()->get(QStringLiteral("/v1/tasks/") + op.taskId, {}, [this, op](const client::ApiResponse& resp) mutable {
    const client::ApiError e = client::classifyResponse(resp);
    if (!e.ok()) {
      setBusy(false);
      return fail(e);
    }
    op.base = resp.json;
    op.rev = resp.json.value(QStringLiteral("rev")).toInteger();
    // Progress needs an actual start (SPEC.md §5.2): default it to the
    // scheduled start, else today, as the user would.
    const QString pct = QStringLiteral("percentComplete");
    const QString actual = QStringLiteral("actualStart");
    if (op.edit && op.body.value(pct).toInt() > 0 && op.base.value(actual).toString().isEmpty() &&
        !op.body.contains(actual)) {
      const QString start = op.base.value(QStringLiteral("start")).toString();
      op.body.insert(actual, start.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : start);
    }
    dryRun(std::move(op));
  });
}

void TaskActions::dryRun(Op op, const QString& note) {
  AppSession* s = session_();
  if (s == nullptr || s->api() == nullptr) return setBusy(false);
  client::ApiRequest r;
  r.method = op.method;
  r.path = op.path;
  if (op.hasBody) r.body = op.body;
  if (op.rev > 0) r.ifMatch = op.rev;
  r.dryRun = true;
  s->api()->send(r, [this, op, note](const client::ApiResponse& resp) {
    const client::ApiError e = client::classifyResponse(resp);
    if (e.kind == client::ErrorKind::RevMismatch) return onRevMismatch(op);
    if (!e.ok()) {
      setBusy(false);
      return fail(e);
    }
    auto preview = client::DryRunPreview::parse(resp.json);
    if (!preview) {
      setBusy(false);
      return say(preview.error().message, true);
    }
    if (preview->changes.isEmpty() && !op.destructive) {
      setBusy(false);
      return say(tr("Nothing changed."), false);
    }
    if (!op.destructive && preview->ripples(kTask, op.taskId) == 0) return commit(op, preview->confirmId);
    QList<PreviewDialog::Row> rows;
    for (const auto& c : preview->changes) {
      const QString name = c.entityId.isEmpty() ? c.after.value(QStringLiteral("title")).toString() : label(c.entityId);
      rows.append({c.op, c.entityKind, name, changeDetail(c)});
    }
    const QString intro = note.isEmpty() ? op.previewIntro : note + QStringLiteral("\n\n") + op.previewIntro;
    auto* d = new PreviewDialog(op.previewTitle, intro, rows, op.confirmText, window_);
    preview_ = d;
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowModality(Qt::WindowModal);
    const QString confirmId = preview->confirmId;
    connect(d, &QDialog::accepted, this, [this, op, confirmId] { commit(op, confirmId); });
    connect(d, &QDialog::rejected, this, [this] {
      setBusy(false);
      say(tr("Nothing changed."), false);
    });
    d->open();
  });
}

void TaskActions::commit(const Op& op, const QString& confirmId) {
  AppSession* s = session_();
  if (s == nullptr || s->api() == nullptr) return setBusy(false);
  client::ApiRequest r;
  r.method = op.method;
  r.path = op.path;
  if (op.hasBody) r.body = op.body;
  if (op.rev > 0) r.ifMatch = op.rev;
  r.confirmId = confirmId.toLatin1();
  s->api()->send(r, [this, op](const client::ApiResponse& resp) {
    const client::ApiError e = client::classifyResponse(resp);
    if (e.kind == client::ErrorKind::RevMismatch) return onRevMismatch(op);
    if (e.kind == client::ErrorKind::ConfirmRequired) {
      return dryRun(op, tr("The confirmation expired or didn't match. Check the changes again."));
    }
    if (!e.ok()) {
      setBusy(false);
      return fail(e);
    }
    if (op.then) return op.then(resp.json);
    finish(op);
  });
}

void TaskActions::finish(const Op& op) {
  setBusy(false);
  say(op.done.isEmpty() ? tr("Saved.") : op.done, false);
  emit changed(op.selectAfter, op.editTitle);
  if (AppSession* s = session_()) s->refresh();
}

// ---- 412 ----

void TaskActions::onRevMismatch(const Op& op) {
  AppSession* s = session_();
  if (!op.edit || op.taskId.isEmpty() || s == nullptr || s->api() == nullptr) {
    // A structural change on a task that just changed: say so, refresh, and
    // let the user look before trying again (never a blind retry).
    setBusy(false);
    say(tr("%1 changed on the server a moment ago, so nothing was done. The outline is refreshed; try again.")
            .arg(label(op.taskId)),
        true);
    if (s != nullptr) s->refresh();
    return;
  }
  s->api()->get(QStringLiteral("/v1/tasks/") + op.taskId, {}, [this, op](const client::ApiResponse& resp) {
    const client::ApiError e = client::classifyResponse(resp);
    if (!e.ok()) {
      setBusy(false);
      return fail(e);
    }
    showConflict(op, resp.json);
  });
}

void TaskActions::showConflict(const Op& op, const QJsonObject& theirs) {
  QJsonObject mine = op.base;
  for (auto it = op.body.constBegin(); it != op.body.constEnd(); ++it) mine.insert(it.key(), it.value());
  const client::ConflictMerge merge(op.base, mine, theirs);
  const QDateTime when = QDateTime::fromString(theirs.value(QStringLiteral("updatedAt")).toString(), Qt::ISODateWithMs);
  const QString who = tr("Changed at %1 (revision %2)")
                          .arg(when.isValid() ? QLocale().toString(when.toLocalTime(), QLocale::ShortFormat)
                                              : theirs.value(QStringLiteral("updatedAt")).toString())
                          .arg(merge.theirRev());
  auto* d = new ConflictDialog(tr("task %1").arg(label(op.taskId)), merge, who, window_);
  conflict_ = d;
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setWindowModality(Qt::WindowModal);
  connect(d, &ConflictDialog::chosen, this, [this, op, merge, theirs](ConflictDialog::Choice c) {
    if (c == ConflictDialog::Choice::Cancel) {
      setBusy(false);
      say(tr("Not saved: %1 changed on the server.").arg(label(op.taskId)), false);
      if (AppSession* s = session_()) s->refresh();
      return;
    }
    Op next = op;
    next.base = theirs;
    next.body = merge.resolution(c == ConflictDialog::Choice::KeepMine);
    next.rev = merge.theirRev();
    if (next.body.isEmpty()) {
      setBusy(false);
      say(tr("Kept the server's version; nothing to send."), false);
      if (AppSession* s = session_()) s->refresh();
      return;
    }
    dryRun(next);  // again: a dry run first, on the new revision
  });
  d->open();
}

// ---- errors (plan §4.3) ----

void TaskActions::fail(const client::ApiError& e) {
  using K = client::ErrorKind;
  QString text;
  AppSession* s = session_();
  switch (e.kind) {
    case K::SummaryRollup:
      text = tr("This task now has subtasks; its progress is rolled up. The outline is refreshed.");
      if (s) s->refresh();
      break;
    case K::Validation: text = tr("The server refused the change: %1").arg(e.message); break;
    case K::ForbiddenScope:
    case K::ForbiddenProject:
      text = tr("%1 This computer's token can't change tasks; nothing was saved.").arg(e.message);
      break;
    case K::NotFound:
      text = tr("This task was deleted elsewhere. The outline is being refreshed.");
      if (s) s->refresh();
      break;
    case K::Conflict: text = tr("Not done: %1").arg(e.message); break;
    case K::RateLimited:
      text = tr("Too many requests: wait %1 s and try again. Nothing was saved.").arg(std::max(1, e.retryAfterSeconds));
      break;
    case K::Unauthenticated:
      text = tr("%1 Nothing was saved.").arg(e.message);
      if (s) s->refresh();
      break;
    case K::Network:
    case K::Timeout:
    case K::Server:
      text = tr("The server can't be reached, so nothing was saved (%1).").arg(e.message);
      if (s) s->refresh();
      break;
    default: text = tr("Not saved: %1").arg(e.message); break;
  }
  say(text, true);
}

}  // namespace zproject::app
