#pragma once

#include <QDate>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QRect>
#include <QSet>
#include <QString>
#include <QWidget>

#include <QAccessible>

#include "app/Theme.hpp"

class QComboBox;
class QLabel;
class QScrollBar;
class QSplitter;
class QToolButton;
class QTreeView;

namespace zproject::app {

class OutlineModel;

// The timeline half of the Gantt (plan §3.4, SPEC.md §8.2 v1 subset):
// custom-painted, read-only, row-for-row with the task table on its left
// (the same model and the table's own row geometry, so they scroll
// together). Bars come from stored dates: auto tasks solid, manual tasks
// outlined, summaries as brackets over their subtasks' span, milestones as
// diamonds; a centre band shows % complete and a flag marks the deadline
// (filled when the finish is past it). Weekends are shaded and a dashed
// line marks today. Only the rows in view are painted (SPEC.md §17).
class GanttChart : public QWidget {
  Q_OBJECT
 public:
  enum class Scale { Day, Week, Month, Quarter };

  struct Bar {
    QString id;
    QRect rect;       // the bar, diamond or bracket, in widget coordinates
    QRect row;        // the whole row
    bool summary = false;
    bool milestone = false;
    bool manual = false;
    bool late = false;  // finish after the deadline
  };

  GanttChart(QTreeView* table, OutlineModel* model, QWidget* parent = nullptr);
  ~GanttChart() override;
  GanttChart(const GanttChart&) = delete;
  GanttChart& operator=(const GanttChart&) = delete;

  void setColors(const ThemeColors& c);
  void setToday(const QDate& today);
  QDate today() const { return today_; }
  void setScale(Scale s);
  Scale scale() const { return scale_; }
  static QString scaleName(Scale s);
  void zoomIn();   // a finer timescale
  void zoomOut();  // a coarser one
  // Puts `date` a third of the way across.
  void scrollToDate(const QDate& date);
  void scrollToToday() { scrollToDate(today_); }
  QScrollBar* horizontalBar() const { return hbar_; }

  // After the model changes: the date range, then a repaint.
  void updateRange();
  QDate rangeStart() const { return first_; }
  QDate rangeEnd() const { return last_; }
  double pxPerDay() const;
  // The left edge of `date` in widget coordinates (scroll applied).
  double xOf(const QDate& date) const;
  QDate dateAt(int x) const;
  // The top of the rows (below the timescale header).
  int rowsTop() const;

  // The bars of the rows in view, as last laid out.
  QList<Bar> visibleBars() const;
  bool hasBar(const QString& id) const;
  QRect barRect(const QString& id) const;
  // "Task 1.2 Drawings" and "3 days, 9–11 Sep, 40% complete": a bar's
  // accessible name and description (also its tooltip).
  QString barName(const QString& id) const;
  QString barDescription(const QString& id) const;
  QString barText(const QString& id) const { return barName(id) + QStringLiteral(", ") + barDescription(id); }
  // Rows painted by the last paintEvent (only the ones in view).
  int lastPaintedRows() const { return lastPainted_; }
  // Accessibility: the cached child interface ids (dropped on reset).
  QHash<QString, QAccessible::Id>& accessibleIds() const { return a11y_; }
  void dropAccessibleChildren();

 signals:
  void taskClicked(const QString& id);
  void taskActivated(const QString& id);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  bool event(QEvent* event) override;

 private:
  QList<Bar> layoutRows(int* rows) const;
  QString idAt(const QPoint& p) const;
  void updateScrollBar();
  int contentWidth() const;
  int rowsWidth() const;
  void paintHeader(QPainter& p, const QRect& area) const;
  QRectF todayLabel(const QRect& area) const;
  void paintBar(QPainter& p, const Bar& b) const;

  QTreeView* table_;
  OutlineModel* model_;
  QScrollBar* hbar_ = nullptr;
  QScrollBar* vbar_ = nullptr;
  ThemeColors c_;
  QDate today_;
  QDate first_;
  QDate last_;
  Scale scale_ = Scale::Week;
  mutable int lastPainted_ = 0;
  mutable QHash<QString, QAccessible::Id> a11y_;
};

// The Gantt page (View → Gantt): the task table (WBS, title, duration,
// start, finish, % complete; "unscheduled" where a task has no dates) and
// the timeline, with a timescale box (Day, Week, Month, Quarter) and Today.
// Read-only: arrows move the selection, Enter opens the task in the
// inspector, T scrolls to today, Ctrl+Shift+= / − and Ctrl+wheel change
// the timescale.
class GanttView : public QWidget {
  Q_OBJECT
 public:
  explicit GanttView(QWidget* parent = nullptr);

  void setProject(const QString& id, const QString& name);
  QString projectId() const { return projectId_; }
  void reload(const QList<QJsonObject>& tasks, const QJsonArray& wbs);
  void setColors(const ThemeColors& c);
  void setToday(const QDate& today);

  QString currentTaskId() const;
  bool selectTask(const QString& id);

  QTreeView* table() const { return table_; }
  GanttChart* chart() const { return chart_; }
  OutlineModel* model() const { return model_; }
  QComboBox* scaleBox() const { return scaleBox_; }
  QToolButton* todayButton() const { return todayButton_; }
  QLabel* title() const { return title_; }
  QLabel* note() const { return note_; }
  QLabel* emptyLabel() const { return empty_; }
  int unscheduledCount() const { return unscheduled_; }

 signals:
  void currentTaskChanged(const QString& id);
  void openTask(const QString& id);  // Enter or a double-click

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  bool handleKey(QKeyEvent* e);
  void fitTable();
  void syncScaleBox();

  QString projectId_;
  bool firstLoad_ = true;
  bool userSized_ = false;
  QLabel* title_ = nullptr;
  QLabel* note_ = nullptr;
  QLabel* empty_ = nullptr;
  QComboBox* scaleBox_ = nullptr;
  QToolButton* todayButton_ = nullptr;
  QSplitter* split_ = nullptr;
  QTreeView* table_ = nullptr;
  GanttChart* chart_ = nullptr;
  OutlineModel* model_ = nullptr;
  QSet<QString> collapsed_;
  int unscheduled_ = 0;
};

}  // namespace zproject::app
