#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QWidget>

#include "client/ChangeRecord.hpp"

class QComboBox;
class QLabel;
class QTableWidget;

namespace zproject::app {

// View → Activity (Ctrl + Shift + A; plan §3.6, SPEC.md §8.3): what changed
// in the selected project, newest first, from the change records the cache
// keeps (no extra request and no audit:read scope). Filters by who made the
// change and by kind (tasks or the project's details). Read-only; Enter (or
// a double-click) on a task's row opens the task in the outline.
class ActivityView : public QWidget {
  Q_OBJECT
 public:
  enum Column { When, Who, Item, Change, ColumnCount };
  // How many of the newest records the view asks the cache for.
  static constexpr int kLimit = 1000;

  explicit ActivityView(QWidget* parent = nullptr);

  void setProject(const QString& id, const QString& name);
  QString projectId() const { return projectId_; }
  // New data: the project's records (newest first), its live tasks and WBS
  // items, and this device's token label ("zpt_…", for "This computer").
  // Keeps the filters and the selection.
  void reload(const QList<client::ChangeRecord>& records, const QList<QJsonObject>& tasks, const QJsonArray& wbs,
              const QString& ownTokenLabel);
  // Shown instead of the counts, e.g. when this token has no change feed.
  void setNotice(const QString& text);

  // Filters: an actor id ("" = everyone) and a kind ("" = all, "task",
  // "project").
  void setActorFilter(const QString& actor);
  void setKindFilter(const QString& kind);
  // The records shown after filtering, newest first.
  const QList<client::ChangeRecord>& shown() const { return shown_; }
  qint64 currentSeq() const;

  QTableWidget* table() const { return table_; }
  QComboBox* actorBox() const { return actorBox_; }
  QComboBox* kindBox() const { return kindBox_; }
  QLabel* heading() const { return heading_; }
  QLabel* note() const { return note_; }

 signals:
  // Show this task in the outline.
  void openTask(const QString& taskId);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void fill();
  void open();

  QString projectId_;
  QString projectName_;
  QString ownLabel_;
  QString notice_;
  QList<client::ChangeRecord> records_;
  QList<client::ChangeRecord> shown_;
  struct TaskName {
    QString wbs;
    QString title;
  };
  QHash<QString, TaskName> live_;  // live tasks by id
  // The last title each task's records carry: a deleted task is named by
  // it (the delete record itself has no title).
  QHash<QString, QString> titles_;
  QLabel* heading_ = nullptr;
  QComboBox* actorBox_ = nullptr;
  QComboBox* kindBox_ = nullptr;
  QTableWidget* table_ = nullptr;
  QLabel* note_ = nullptr;
};

}  // namespace zproject::app
