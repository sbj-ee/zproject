#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QToolButton;
class QTreeView;

namespace zproject::app {

class OutlineFilter;
class OutlineModel;

// The WBS outline page (plan §3.2, SPEC.md §7.1, §8.3): the task tree with
// its WBS codes, a status filter, a find bar (Ctrl-F), and inline editing
// of title, status, % complete and priority (a summary's status and % are
// rolled up and locked). Structural keys (Ins/Enter, Tab, Shift-Tab,
// Alt-↑/↓, Del, F2) are the window's actions, bound to the tree.
class OutlineView : public QWidget {
  Q_OBJECT
 public:
  explicit OutlineView(QWidget* parent = nullptr);

  void setProject(const QString& id, const QString& name);
  QString projectId() const { return projectId_; }
  // New data from the cache: keeps what is collapsed, the selection and the
  // filter; selects (and edits) the pending task once it is there.
  void reload(const QList<QJsonObject>& tasks, const QJsonArray& wbs);
  void setEditable(bool on);
  // After a write: select this task when the refresh brings it.
  void setPending(const QString& id, bool editTitle);

  QString currentTaskId() const;
  bool selectTask(const QString& id);
  void editTitle(const QString& id);
  void setStatusFilter(const QString& status);

  // Find (Ctrl-F, F3, Shift-F3, Esc).
  void showFind();
  void hideFind();
  void findNext(bool backwards = false);
  QStringList findMatches() const { return matches_; }

  void showMessage(const QString& text, bool error);

  QTreeView* tree() const { return tree_; }
  OutlineModel* model() const { return model_; }
  OutlineFilter* filter() const { return filter_; }
  QComboBox* statusFilterBox() const { return statusBox_; }
  QLineEdit* findField() const { return findField_; }
  QLabel* findStatus() const { return findStatus_; }
  QFrame* findBar() const { return findBar_; }
  QLabel* message() const { return message_; }
  QLabel* emptyLabel() const { return empty_; }
  QLabel* title() const { return title_; }

 signals:
  void currentTaskChanged(const QString& id);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void updateMatches();
  void updateCounts();
  void restoreExpansion();

  QString projectId_;
  QLabel* title_ = nullptr;
  QLabel* counts_ = nullptr;
  QComboBox* statusBox_ = nullptr;
  QFrame* findBar_ = nullptr;
  QLineEdit* findField_ = nullptr;
  QLabel* findStatus_ = nullptr;
  QToolButton* findPrev_ = nullptr;
  QToolButton* findNextButton_ = nullptr;
  QToolButton* findClose_ = nullptr;
  QLabel* message_ = nullptr;
  QLabel* empty_ = nullptr;
  QTreeView* tree_ = nullptr;
  OutlineModel* model_ = nullptr;
  OutlineFilter* filter_ = nullptr;
  QSet<QString> collapsed_;
  QString pending_;
  bool pendingEdit_ = false;
  QStringList matches_;
  bool restoring_ = false;
  QList<QJsonObject> deferredTasks_;
  QJsonArray deferredWbs_;
  bool hasDeferred_ = false;
};

}  // namespace zproject::app
