#include "app/OutlineModel.hpp"

#include <QBrush>
#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <algorithm>

namespace zproject::app {

namespace {

const QString kParent = QStringLiteral("parent");
const QString kOrder = QStringLiteral("order");

// The lock on a summary's rolled-up cells, drawn so it looks the same on
// every platform (no emoji font needed).
QIcon lockIcon() {
  static const QIcon icon = [] {
    QPixmap px(14, 14);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c(0x8a, 0x8f, 0x98);
    p.setPen(QPen(c, 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(4, 1.5, 6, 7), 0, 180 * 16);
    p.drawLine(QPointF(4, 5), QPointF(4, 7));
    p.drawLine(QPointF(10, 5), QPointF(10, 7));
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(2.5, 6.5, 9, 6.5), 1.5, 1.5);
    return QIcon(px);
  }();
  return icon;
}

// Milestones (SPEC.md §7.1) get a diamond, as on the Gantt.
QIcon diamondIcon() {
  static const QIcon icon = [] {
    QPixmap px(14, 14);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath d;
    d.moveTo(7, 1.5);
    d.lineTo(12.5, 7);
    d.lineTo(7, 12.5);
    d.lineTo(1.5, 7);
    d.closeSubpath();
    p.fillPath(d, QColor(0x2f, 0xa8, 0xf0));
    return QIcon(px);
  }();
  return icon;
}

QString str(const QJsonObject& o, const QString& k) {
  const QJsonValue v = o.value(k);
  return v.isString() ? v.toString() : QString();
}

}  // namespace

OutlineModel::OutlineModel(QObject* parent) : QAbstractItemModel(parent) {}
OutlineModel::~OutlineModel() = default;

QString OutlineModel::field(int column) {
  switch (column) {
    case Title: return QStringLiteral("title");
    case Type: return QStringLiteral("type");
    case Status: return QStringLiteral("status");
    case Percent: return QStringLiteral("percentComplete");
    case Duration: return QStringLiteral("duration");
    case Start: return QStringLiteral("start");
    case Finish: return QStringLiteral("finish");
    case Deadline: return QStringLiteral("deadline");
    case Priority: return QStringLiteral("priority");
    default: return {};
  }
}

QString OutlineModel::statusName(const QString& status) {
  if (status == QLatin1String("todo")) return tr("To do");
  if (status == QLatin1String("in_progress")) return tr("In progress");
  if (status == QLatin1String("done")) return tr("Done");
  return status;
}

QString OutlineModel::typeName(const QJsonObject& task, bool summary) {
  if (summary) return tr("Summary");
  if (str(task, QStringLiteral("type")) == QLatin1String("milestone")) return tr("Milestone");
  return tr("Task");
}

void OutlineModel::setTasks(const QList<QJsonObject>& tasks, const QJsonArray& wbs) {
  beginResetModel();
  byId_.clear();
  root_.children.clear();
  nodes_.clear();
  nodes_.reserve(static_cast<size_t>(tasks.size()));
  for (const QJsonObject& t : tasks) {
    const QString id = str(t, QStringLiteral("id"));
    if (id.isEmpty() || byId_.contains(id) || t.value(QStringLiteral("deleted")).toBool()) continue;
    auto n = std::make_unique<Node>();
    n->id = id;
    n->json = t;
    byId_.insert(id, n.get());
    nodes_.push_back(std::move(n));
  }
  // Parents; a task whose parent isn't here (deleted, or not cached yet) is
  // shown at the top level rather than lost.
  for (const auto& n : nodes_) {
    Node* parent = byId_.value(str(n->json, kParent), &root_);
    // A cycle can't come from the server; guard anyway so the tree stays a tree.
    for (Node* a = parent; a != nullptr && a != &root_; a = byId_.value(str(a->json, kParent), nullptr)) {
      if (a == n.get()) {
        parent = &root_;
        break;
      }
    }
    if (parent == nullptr) parent = &root_;
    n->parent = parent;
    parent->children.push_back(n.get());
  }
  // Siblings by `order`, then id (as the server sorts them).
  std::vector<Node*> stack{&root_};
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    std::stable_sort(n->children.begin(), n->children.end(), [](const Node* a, const Node* b) {
      const double oa = a->json.value(kOrder).toDouble();
      const double ob = b->json.value(kOrder).toDouble();
      if (oa != ob) return oa < ob;
      return a->id < b->id;
    });
    for (Node* c : n->children) stack.push_back(c);
  }
  // WBS codes: computed, then the server's if its list is this same tree.
  std::vector<std::pair<Node*, QString>> todo{{&root_, QString()}};
  while (!todo.empty()) {
    auto [n, prefix] = todo.back();
    todo.pop_back();
    for (size_t i = 0; i < n->children.size(); ++i) {
      Node* c = n->children[i];
      c->wbs = prefix.isEmpty() ? QString::number(i + 1) : prefix + QLatin1Char('.') + QString::number(i + 1);
      todo.emplace_back(c, c->wbs);
    }
  }
  bool matches = !wbs.isEmpty() && wbs.size() == static_cast<qsizetype>(byId_.size());
  for (const QJsonValue& v : wbs) {
    const QJsonObject item = v.toObject();
    const QString id = str(item, QStringLiteral("id"));
    const Node* n = byId_.value(id, nullptr);
    const QString parent = str(item, kParent);
    if (n == nullptr || (n->parent == &root_ ? !parent.isEmpty() : n->parent->id != parent) ||
        item.value(QStringLiteral("wbs")).toString() != n->wbs) {
      matches = false;
      break;
    }
  }
  serverWbs_ = matches;
  endResetModel();
}

void OutlineModel::setEditable(bool on) {
  if (editable_ == on) return;
  editable_ = on;
  if (!byId_.isEmpty()) emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
}

OutlineModel::Node* OutlineModel::nodeAt(const QModelIndex& index) const {
  if (!index.isValid()) return const_cast<Node*>(&root_);
  return static_cast<Node*>(index.internalPointer());
}

int OutlineModel::rowOf(const Node* n) const {
  if (n == nullptr || n->parent == nullptr) return 0;
  const auto& sib = n->parent->children;
  return static_cast<int>(std::find(sib.begin(), sib.end(), n) - sib.begin());
}

QModelIndex OutlineModel::index(int row, int column, const QModelIndex& parent) const {
  if (column < 0 || column >= ColumnCount || row < 0) return {};
  if (parent.isValid() && parent.column() != 0) return {};
  const Node* p = nodeAt(parent);
  if (row >= static_cast<int>(p->children.size())) return {};
  return createIndex(row, column, p->children[static_cast<size_t>(row)]);
}

QModelIndex OutlineModel::parent(const QModelIndex& child) const {
  if (!child.isValid()) return {};
  const Node* n = nodeAt(child);
  Node* p = n->parent;
  if (p == nullptr || p == &root_) return {};
  return createIndex(rowOf(p), 0, p);
}

int OutlineModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid() && parent.column() != 0) return 0;
  return static_cast<int>(nodeAt(parent)->children.size());
}

int OutlineModel::columnCount(const QModelIndex&) const { return ColumnCount; }

// The earliest start or latest finish among a summary's descendants (ISO
// dates compare as strings). Display only: the server doesn't roll dates up
// until the scheduling engine arrives (SPEC §5.3, §6).
QString OutlineModel::dateSpan(const Node* n, bool earliest) const {
  const QString key = earliest ? QStringLiteral("start") : QStringLiteral("finish");
  QString best;
  std::vector<const Node*> stack(n->children.begin(), n->children.end());
  while (!stack.empty()) {
    const Node* c = stack.back();
    stack.pop_back();
    if (!c->children.empty()) {
      stack.insert(stack.end(), c->children.begin(), c->children.end());
      continue;
    }
    const QString v = c->json.value(key).toString();
    if (v.isEmpty()) continue;
    if (best.isEmpty() || (earliest ? v < best : v > best)) best = v;
  }
  return best;
}

QVariant OutlineModel::display(const Node* n, int column) const {
  const QJsonObject& t = n->json;
  const bool summary = !n->children.empty();
  switch (column) {
    case Wbs: return n->wbs;
    case Title: return str(t, QStringLiteral("title"));
    case Type: return typeName(t, summary);
    case Status: return statusName(str(t, QStringLiteral("status")));
    case Percent: return QStringLiteral("%1%").arg(qRound(t.value(QStringLiteral("percentComplete")).toDouble()));
    case Duration: {
      if (summary) return QString();  // not rolled up yet; see dateSpan()
      const double d = t.value(QStringLiteral("duration")).toDouble();
      return d == 1.0 ? tr("1 day") : tr("%1 days").arg(QString::number(d, 'g', 6));
    }
    case Start: return summary ? dateSpan(n, true) : str(t, QStringLiteral("start"));
    case Finish: return summary ? dateSpan(n, false) : str(t, QStringLiteral("finish"));
    case Deadline: return str(t, QStringLiteral("deadline"));
    case Priority: return QStringLiteral("P%1").arg(t.value(QStringLiteral("priority")).toInt(2));
    default: return {};
  }
}

QVariant OutlineModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid()) return {};
  const Node* n = nodeAt(index);
  const bool summary = !n->children.empty();
  const int col = index.column();
  const bool rolledUp = summary && (col == Status || col == Percent);
  switch (role) {
    case Qt::DisplayRole: return display(n, col);
    case Qt::EditRole: {
      const QJsonValue v = n->json.value(field(col));
      if (col == Percent || col == Priority) return v.toInt();
      return v.toVariant();
    }
    case Qt::FontRole:
      if (summary) {
        QFont f;
        f.setBold(true);
        return f;
      }
      return {};
    case Qt::DecorationRole:
      if (rolledUp) return lockIcon();
      if (col == Title && !summary && isMilestone(n->id)) return diamondIcon();
      return {};
    case Qt::ToolTipRole: {
      const int kids = static_cast<int>(n->children.size());
      if (rolledUp) {
        return kids == 1 ? tr("Rolled up from 1 child task") : tr("Rolled up from %1 child tasks").arg(kids);
      }
      if (col == Title) return accessibleText(n->id);
      if (summary && (col == Start || col == Finish)) return tr("%1, from its subtasks").arg(display(n, col).toString());
      return display(n, col);
    }
    case Qt::AccessibleTextRole:
      if (col == Title) return accessibleText(n->id);
      if (rolledUp) return tr("%1, rolled up").arg(display(n, col).toString());
      return display(n, col);
    case Qt::TextAlignmentRole:
      if (col == Percent || col == Duration || col == Priority) return int(Qt::AlignRight | Qt::AlignVCenter);
      return {};
    case IdRole: return n->id;
    case SummaryRole: return summary;
    case FieldRole: return field(col);
    default: return {};
  }
}

QVariant OutlineModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  switch (section) {
    case Wbs: return tr("WBS");
    case Title: return tr("Title");
    case Type: return tr("Type");
    case Status: return tr("Status");
    case Percent: return tr("% Complete");
    case Duration: return tr("Duration");
    case Start: return tr("Start");
    case Finish: return tr("Finish");
    case Deadline: return tr("Deadline");
    case Priority: return tr("Priority");
    default: return {};
  }
}

Qt::ItemFlags OutlineModel::flags(const QModelIndex& index) const {
  if (!index.isValid()) return Qt::NoItemFlags;
  Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
  if (!editable_) return f;
  const Node* n = nodeAt(index);
  const bool summary = !n->children.empty();
  switch (index.column()) {
    case Title:
    case Priority: f |= Qt::ItemIsEditable; break;
    case Status:
    case Percent:
      if (!summary) f |= Qt::ItemIsEditable;  // a summary's are rolled up (SPEC.md §5.3)
      break;
    default: break;  // dates, duration and type: the task editor
  }
  return f;
}

bool OutlineModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (role != Qt::EditRole || !(flags(index) & Qt::ItemIsEditable)) return false;
  const Node* n = nodeAt(index);
  const QString f = field(index.column());
  QJsonValue v;
  switch (index.column()) {
    case Title: {
      const QString title = value.toString().trimmed();
      if (title.isEmpty()) return false;
      v = title;
      break;
    }
    case Status: {
      const QString s = value.toString();
      if (s != QLatin1String("todo") && s != QLatin1String("in_progress") && s != QLatin1String("done")) return false;
      v = s;
      break;
    }
    case Percent: {
      bool ok = false;
      const int p = value.toInt(&ok);
      if (!ok || p < 0 || p > 100) return false;
      v = p;
      break;
    }
    case Priority: {
      bool ok = false;
      const int p = value.toInt(&ok);
      if (!ok || p < 1 || p > 4) return false;
      v = p;
      break;
    }
    default: return false;
  }
  if (n->json.value(f) == v) return false;
  emit editRequested(n->id, f, v);
  return false;  // the server's answer, via the next refresh, changes the model
}

QModelIndex OutlineModel::indexOf(const QString& id, int column) const {
  const Node* n = byId_.value(id, nullptr);
  if (n == nullptr) return {};
  return createIndex(rowOf(n), column, const_cast<Node*>(n));
}

QString OutlineModel::idAt(const QModelIndex& index) const {
  return index.isValid() ? nodeAt(index)->id : QString();
}

QJsonObject OutlineModel::task(const QString& id) const {
  const Node* n = byId_.value(id, nullptr);
  return n ? n->json : QJsonObject();
}

QString OutlineModel::parentId(const QString& id) const {
  const Node* n = byId_.value(id, nullptr);
  return n && n->parent && n->parent != &root_ ? n->parent->id : QString();
}

QStringList OutlineModel::childIds(const QString& parentId) const {
  const Node* p = parentId.isEmpty() ? &root_ : byId_.value(parentId, nullptr);
  QStringList out;
  if (p == nullptr) return out;
  for (const Node* c : p->children) out << c->id;
  return out;
}

int OutlineModel::siblingIndex(const QString& id) const {
  const Node* n = byId_.value(id, nullptr);
  return n ? rowOf(n) : -1;
}

bool OutlineModel::isSummary(const QString& id) const {
  const Node* n = byId_.value(id, nullptr);
  return n && !n->children.empty();
}

bool OutlineModel::isMilestone(const QString& id) const {
  const Node* n = byId_.value(id, nullptr);
  return n && str(n->json, QStringLiteral("type")) == QLatin1String("milestone");
}

int OutlineModel::liveChildren(const QString& id) const {
  const Node* n = byId_.value(id, nullptr);
  return n ? static_cast<int>(n->children.size()) : 0;
}

QString OutlineModel::wbsCode(const QString& id) const {
  const Node* n = byId_.value(id, nullptr);
  return n ? n->wbs : QString();
}

QStringList OutlineModel::depthFirstIds() const {
  QStringList out;
  std::vector<const Node*> stack;
  for (auto it = root_.children.rbegin(); it != root_.children.rend(); ++it) stack.push_back(*it);
  while (!stack.empty()) {
    const Node* n = stack.back();
    stack.pop_back();
    out << n->id;
    for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.push_back(*it);
  }
  return out;
}

QString OutlineModel::accessibleText(const QString& id) const {
  const Node* n = byId_.value(id, nullptr);
  if (n == nullptr) return {};
  const QString title = str(n->json, QStringLiteral("title"));
  const int pct = qRound(n->json.value(QStringLiteral("percentComplete")).toDouble());
  const int kids = static_cast<int>(n->children.size());
  if (kids > 0) {
    const QString of = kids == 1 ? tr("summary of 1 task") : tr("summary of %1 tasks").arg(kids);
    return tr("%1 %2, %3, %4% complete (rolled up)").arg(n->wbs, title, of).arg(pct);
  }
  if (isMilestone(id)) return tr("%1 %2, milestone, %3").arg(n->wbs, title, statusName(str(n->json, QStringLiteral("status"))));
  return tr("%1 %2, %3% complete, %4").arg(n->wbs, title).arg(pct).arg(statusName(str(n->json, QStringLiteral("status"))));
}

// ---- filter ----

OutlineFilter::OutlineFilter(QObject* parent) : QSortFilterProxyModel(parent) {
  setRecursiveFilteringEnabled(true);
}

void OutlineFilter::setStatusFilter(const QString& status) {
  if (status_ == status) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
  beginFilterChange();
  status_ = status;
  endFilterChange();
#else
  status_ = status;
  invalidateFilter();
#endif
}

bool OutlineFilter::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
  if (status_.isEmpty()) return true;
  const QModelIndex i = sourceModel()->index(sourceRow, OutlineModel::Status, sourceParent);
  const QString s = sourceModel()->data(i, Qt::EditRole).toString();
  if (status_ == QLatin1String("open")) return s != QLatin1String("done");
  return s == status_;
}

}  // namespace zproject::app
