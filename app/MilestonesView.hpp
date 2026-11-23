#pragma once

#include <QColor>
#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace zproject::app {

// One project's tasks, as the cache has them, for the milestones view.
struct MilestoneSource {
  QString projectId;
  QString projectName;
  QList<QJsonObject> tasks;
  QJsonArray wbs;
  QString projectColor;  // the project's #rrggbb, or empty
};

// One milestone (a task of type "milestone") with what the view shows.
struct MilestoneRow {
  QString projectId;
  QString projectName;
  QColor projectColor;  // invalid: the project has none
  QString taskId;
  QString title;
  QString wbs;          // "1.4", numbered as the outline numbers it
  QString parentTitle;  // the summary it sits under; empty at top level
  QDate date;           // finish (or start); invalid: no date yet
  QDate deadline;       // invalid: none
  QString status;       // todo, in_progress, done
  bool done = false;
  // Days from today to the date: > 0 to go, 0 today, < 0 late. Only
  // meaningful when the date is valid and the milestone is not done.
  qint64 days = 0;
  bool late = false;            // not done and the date has passed
  bool afterDeadline = false;   // the date is after its deadline
};

// The milestones of the given projects (plan §3.5, SPEC.md §7.1), dated ones
// first by date, then those with no date; ties by project and WBS code.
QList<MilestoneRow> milestoneRows(const QList<MilestoneSource>& sources, const QDate& today);
// "In 3 days", "Today", "2 days late", "Done", "No date".
QString milestoneDueText(const MilestoneRow& row);
// The row in words, for screen readers: "Milestone 1.4 Handover, Kitchen
// remodel, 20 Nov 2026, 2 days late, after its deadline of 18 Nov 2026".
QString milestoneAccessibleText(const MilestoneRow& row, bool withProject);

// View → Milestones (Ctrl + Shift + M): the selected project's milestones,
// or every project's with "All projects" ticked. Showing them all, a
// milestone's diamond and its Project cell take the project's colour (a
// swatch outlined in the theme's text colour; the name is still there in
// words, so colour is never the only cue). Read-only; Enter (or a
// double-click) opens the milestone in the outline, Show in Gantt opens it
// on the timeline.
class MilestonesView : public QWidget {
  Q_OBJECT
 public:
  enum Column { Title, Project, Wbs, Date, Deadline, Status, Due, Parent, ColumnCount };

  explicit MilestonesView(QWidget* parent = nullptr);

  void setToday(const QDate& today);
  // The theme's text colour, for the outline round project colours.
  void setOutlineColor(const QColor& c);
  QDate today() const { return today_; }
  // The selected project, named in the heading when not showing them all.
  void setProject(const QString& id, const QString& name);
  QString projectId() const { return projectId_; }
  bool allProjects() const;
  void setAllProjects(bool all);
  // New data from the cache; keeps the selection.
  void reload(const QList<MilestoneSource>& sources);

  const QList<MilestoneRow>& rows() const { return rows_; }
  QString currentTaskId() const;
  QString currentRowProjectId() const;
  bool selectTask(const QString& id);

  QTableWidget* table() const { return table_; }
  QCheckBox* allProjectsBox() const { return all_; }
  QLabel* heading() const { return heading_; }
  QLabel* note() const { return note_; }
  QPushButton* outlineButton() const { return outlineButton_; }
  QPushButton* ganttButton() const { return ganttButton_; }

 signals:
  // Show this milestone in the outline (gantt false) or the Gantt.
  void openTask(const QString& projectId, const QString& taskId, bool gantt);
  // The "All projects" box changed: the window reloads with other sources.
  void allProjectsChanged(bool all);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void updateHeading();
  void updateButtons();
  void open(bool gantt);
  void decorate();  // project colours on the rows (all projects only)

  QDate today_;
  QColor outline_ = Qt::black;
  QString projectId_;
  QString projectName_;
  QList<MilestoneRow> rows_;
  QLabel* heading_ = nullptr;
  QCheckBox* all_ = nullptr;
  QTableWidget* table_ = nullptr;
  QLabel* note_ = nullptr;
  QPushButton* outlineButton_ = nullptr;
  QPushButton* ganttButton_ = nullptr;
};

}  // namespace zproject::app
