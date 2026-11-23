#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace zproject::app {

// The WBS outline of one project (SPEC.md §7.1, plan §3.2): a tree over the
// cache's tasks (parent and sibling `order`). WBS codes are numbered the way
// the server numbers them (depth-first, siblings 1, 2, …) and checked
// against the cached GET /v1/projects/{id}/wbs list; serverWbs() says
// whether they agree (they can lag by one refresh after a move).
//
// A summary (a task with live children, SPEC.md §5.3) is bold, and its
// rolled-up cells (% complete, status) carry a lock and can't be edited.
// The model never changes on its own: setData() only asks for a write
// (editRequested), and the next refresh shows what the server kept.
class OutlineModel : public QAbstractItemModel {
  Q_OBJECT
 public:
  enum Column { Wbs, Title, Type, Status, Percent, Duration, Start, Finish, Deadline, Priority, ColumnCount };
  enum Role {
    IdRole = Qt::UserRole + 1,
    SummaryRole,  // bool
    FieldRole,    // the API field a column shows ("" for WBS)
  };

  explicit OutlineModel(QObject* parent = nullptr);
  ~OutlineModel() override;

  // Rebuilds the tree from live tasks (any order) and the cached WBS items.
  void setTasks(const QList<QJsonObject>& tasks, const QJsonArray& wbs);
  // Editing on or off (offline: off).
  void setEditable(bool on);
  bool editable() const { return editable_; }
  // True when the codes agree with the server's cached WBS list.
  bool serverWbs() const { return serverWbs_; }

  // QAbstractItemModel
  QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
  QModelIndex parent(const QModelIndex& child) const override;
  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

  // Lookups by task id.
  QModelIndex indexOf(const QString& id, int column = Title) const;
  QString idAt(const QModelIndex& index) const;
  bool contains(const QString& id) const { return byId_.contains(id); }
  QJsonObject task(const QString& id) const;
  QString parentId(const QString& id) const;
  QStringList childIds(const QString& parentId) const;  // "" = top level
  int siblingIndex(const QString& id) const;
  bool isSummary(const QString& id) const;
  bool isMilestone(const QString& id) const;
  int liveChildren(const QString& id) const;
  QString wbsCode(const QString& id) const;
  QStringList depthFirstIds() const;
  int taskCount() const { return static_cast<int>(byId_.size()); }

  static QString field(int column);
  static QString statusName(const QString& status);
  static QString typeName(const QJsonObject& task, bool summary);
  // "1.2 Design, summary of 3 tasks, 40% complete (rolled up)": for screen
  // readers.
  QString accessibleText(const QString& id) const;

 signals:
  // An inline edit: PATCH {field: value} on the task (the caller does the
  // dry run, If-Match and preview).
  void editRequested(const QString& id, const QString& field, const QJsonValue& value);

 private:
  struct Node {
    QString id;
    QJsonObject json;
    Node* parent = nullptr;
    std::vector<Node*> children;
    QString wbs;
  };
  Node* nodeAt(const QModelIndex& index) const;
  int rowOf(const Node* n) const;
  QVariant display(const Node* n, int column) const;
  QString dateSpan(const Node* n, bool earliest) const;

  std::vector<std::unique_ptr<Node>> nodes_;
  Node root_;
  QHash<QString, Node*> byId_;
  bool editable_ = true;
  bool serverWbs_ = false;
};

// The outline's status filter (plan §3.2): a row stays when it matches, and
// so do its ancestors (so a match is never shown without its context).
class OutlineFilter : public QSortFilterProxyModel {
  Q_OBJECT
 public:
  // "", "open" (not done), "todo", "in_progress", "done".
  explicit OutlineFilter(QObject* parent = nullptr);
  void setStatusFilter(const QString& status);
  QString statusFilter() const { return status_; }

 protected:
  bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

 private:
  QString status_;
};

}  // namespace zproject::app
