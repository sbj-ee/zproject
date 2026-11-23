#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

#include "client/ApiError.hpp"

class QWidget;

namespace zproject::app {

class AppSession;
class ConflictDialog;
class OutlineModel;
class PreviewDialog;

// The outline's writes (plan §3.2, §4.3-§4.5): insert, indent, outdent,
// move up/down, delete and restore, convert to milestone, and inline edits.
// Each one reads the task from the server first (its rev is the If-Match;
// the cache has none), runs a dry run, shows the preview when the write is
// destructive or ripples to other tasks (summaries rolling up; renumbered
// siblings don't count), then commits. A 412 on an edit opens the conflict
// dialog; on a structural change it refreshes and says so (no blind retry).
// One write at a time.
class TaskActions : public QObject {
  Q_OBJECT
 public:
  using SessionFn = std::function<AppSession*()>;
  TaskActions(QWidget* window, SessionFn session, OutlineModel* model, QObject* parent = nullptr);

  // Enter / Ins: a new task below `currentId` (same parent), or the last
  // top-level task when nothing is selected.
  void insertBelow(const QString& projectId, const QString& currentId);
  void indent(const QString& id);    // under the previous sibling, last
  void outdent(const QString& id);   // after its parent
  void moveUp(const QString& id);
  void moveDown(const QString& id);
  void deleteTask(const QString& id);
  // Restores the task deleted most recently in this window (and the
  // subtasks the same delete took).
  void restoreLast();
  // Task ⇄ milestone (duration 0); not for a summary.
  void toggleMilestone(const QString& id);
  void edit(const QString& id, const QString& field, const QJsonValue& value);

  // What the outline allows for `id` now (menus and keys).
  bool canIndent(const QString& id) const;
  bool canOutdent(const QString& id) const;
  bool canMoveUp(const QString& id) const;
  bool canMoveDown(const QString& id) const;
  bool canToggleMilestone(const QString& id) const;

  struct Deleted {
    QString id;
    QString label;  // "1.2 Design" when it was deleted
  };
  const QList<Deleted>& deleted() const { return deleted_; }
  bool busy() const { return busy_; }
  QString lastMessage() const { return lastMessage_; }
  PreviewDialog* preview() const { return preview_.data(); }
  ConflictDialog* conflict() const { return conflict_.data(); }

 signals:
  // A write reached the server: select `selectId` once the refresh has it
  // (and start editing its title when `editTitle`).
  void changed(const QString& selectId, bool editTitle);
  void deletedChanged();
  void notice(const QString& text, bool error);

 private:
  struct Op {
    QString what;           // "Indent 1.2 Design": for messages
    QString done;           // the message once it worked
    QByteArray method;
    QString path;
    QJsonObject body;
    bool hasBody = false;
    QString taskId;         // read first for its rev; empty for a create
    qint64 rev = 0;
    QJsonObject base;       // the server's copy the write starts from
    bool destructive = false;
    bool edit = false;      // a field edit: a 412 opens the conflict dialog
    QString previewTitle;
    QString previewIntro;
    QString confirmText;
    QString selectAfter;
    bool editTitle = false;
    // A second write once this one worked (insert: move into place).
    std::function<void(const QJsonObject& created)> then;
  };
  void start(Op op);
  void dryRun(Op op, const QString& note = {});
  void commit(const Op& op, const QString& confirmId);
  void finish(const Op& op);
  void onRevMismatch(const Op& op);
  void showConflict(const Op& op, const QJsonObject& theirs);
  void fail(const client::ApiError& e);
  void say(const QString& text, bool error);
  QString label(const QString& id) const;
  void setBusy(bool busy) { busy_ = busy; }

  QWidget* window_;
  SessionFn session_;
  OutlineModel* model_;
  QPointer<PreviewDialog> preview_;
  QPointer<ConflictDialog> conflict_;
  QList<Deleted> deleted_;
  bool busy_ = false;
  QString lastMessage_;
};

}  // namespace zproject::app
