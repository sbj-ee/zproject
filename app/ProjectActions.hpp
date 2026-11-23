#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

#include "app/EditDialogs.hpp"
#include "client/ApiError.hpp"
#include "client/EditFlow.hpp"

class QWidget;

namespace zproject::app {

class AppSession;

// Project create, edit and delete for the desktop (SPEC.md §7.10, §9.3;
// plan §4.3-§4.5). Every write runs ?dryRun=true first:
//   - create and edit apply straight after a clean dry run (Q6) and show the
//     preview only when the server would change something else too;
//   - delete always shows the preview (it is destructive) and sends the dry
//     run's confirmId when the token needs one;
//   - every PATCH and DELETE carries If-Match with the revision the user
//     started from; a 412 opens the shared ConflictDialog.
// Errors follow plan §4.3: field errors stay in the editor, 404 says
// "deleted elsewhere", 403 names the missing scope, offline sends nothing.
class ProjectActions : public QObject {
  Q_OBJECT
 public:
  using SessionFn = std::function<AppSession*()>;
  ProjectActions(QWidget* window, SessionFn session, QObject* parent = nullptr);

  ProjectEditor* newProject();
  ProjectEditor* editProject(const QString& id);
  void deleteProject(const QString& id);

  ProjectEditor* editor() const { return editor_.data(); }
  PreviewDialog* preview() const { return preview_.data(); }
  ConflictDialog* conflict() const { return conflict_.data(); }
  bool busy() const { return busy_; }
  QString lastMessage() const { return lastMessage_; }

 signals:
  // A write reached the server; `id` is the project (new or changed).
  void saved(const QString& id, bool created);
  void deleted(const QString& id);
  // Something the user should read (status bar or a message box).
  void notice(const QString& text, bool error);

 private:
  struct Write {
    QByteArray method;
    QString path;
    QJsonObject body;
    bool hasBody = false;
    qint64 rev = 0;  // If-Match (0: none, for a create)
    QString projectId;
  };
  void save();
  void runDryRun(const Write& w);
  void commit(const Write& w, const QString& confirmId);
  void startConflict(const Write& w);
  void showConflict(const Write& w, const QJsonObject& theirs, const QString& who);
  void deleteDryRun(const QString& id, qint64 rev, const QString& note);
  void fail(const client::ApiError& e);
  void say(const QString& text, bool error);
  QString projectName(const QString& id) const;
  QString taskName(const QString& id) const;
  void setBusy(bool busy);

  QWidget* window_;
  SessionFn session_;
  QPointer<ProjectEditor> editor_;
  QPointer<PreviewDialog> preview_;
  QPointer<ConflictDialog> conflict_;
  QJsonObject base_;  // what the edit started from (the cache's copy, then the server's)
  bool busy_ = false;
  QString lastMessage_;
};

}  // namespace zproject::app
