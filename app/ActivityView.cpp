#include "app/ActivityView.hpp"

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "app/ChangeText.hpp"
#include "app/OutlineModel.hpp"

namespace zproject::app {

namespace {

constexpr int kSeqRole = Qt::UserRole + 1;
constexpr int kTaskRole = Qt::UserRole + 2;

}  // namespace

ActivityView::ActivityView(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("activityView"));
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);

  auto* bar = new QWidget(this);
  bar->setObjectName(QStringLiteral("activityBar"));
  auto* h = new QHBoxLayout(bar);
  h->setContentsMargins(10, 6, 10, 6);
  heading_ = new QLabel(bar);
  heading_->setObjectName(QStringLiteral("activityHeading"));
  h->addWidget(heading_, 1);
  auto* whoLabel = new QLabel(tr("&Who:"), bar);
  h->addWidget(whoLabel);
  actorBox_ = new QComboBox(bar);
  actorBox_->setObjectName(QStringLiteral("activityActor"));
  actorBox_->setAccessibleName(tr("Show changes by"));
  actorBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  whoLabel->setBuddy(actorBox_);
  h->addWidget(actorBox_);
  auto* whatLabel = new QLabel(tr("W&hat:"), bar);
  h->addWidget(whatLabel);
  kindBox_ = new QComboBox(bar);
  kindBox_->setObjectName(QStringLiteral("activityKind"));
  kindBox_->setAccessibleName(tr("Show changes to"));
  kindBox_->addItem(tr("All changes"), QString());
  kindBox_->addItem(tr("Tasks"), QStringLiteral("task"));
  kindBox_->addItem(tr("Project details"), QStringLiteral("project"));
  whatLabel->setBuddy(kindBox_);
  h->addWidget(kindBox_);
  connect(actorBox_, &QComboBox::currentIndexChanged, this, [this] { fill(); });
  connect(kindBox_, &QComboBox::currentIndexChanged, this, [this] { fill(); });
  v->addWidget(bar);

  table_ = new QTableWidget(0, ColumnCount, this);
  table_->setObjectName(QStringLiteral("activityTable"));
  table_->setAccessibleName(tr("Activity"));
  table_->setHorizontalHeaderLabels({tr("When"), tr("Who"), tr("Item"), tr("Change")});
  table_->verticalHeader()->hide();
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setTabKeyNavigation(false);
  table_->setShowGrid(false);
  table_->setWordWrap(false);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->installEventFilter(this);
  connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { open(); });
  v->addWidget(table_, 1);

  note_ = new QLabel(this);
  note_->setObjectName(QStringLiteral("activityNote"));
  note_->setContentsMargins(10, 4, 10, 4);
  note_->setWordWrap(true);
  v->addWidget(note_);

  setFocusProxy(table_);
  setProject({}, {});
}

void ActivityView::setProject(const QString& id, const QString& name) {
  projectId_ = id;
  projectName_ = name;
  const QString text = tr("Activity — %1").arg(name);
  heading_->setText(QStringLiteral("<b>%1</b>").arg(text.toHtmlEscaped()));
  heading_->setAccessibleName(text);
}

void ActivityView::setNotice(const QString& text) {
  notice_ = text;
  fill();
}

void ActivityView::reload(const QList<client::ChangeRecord>& records, const QList<QJsonObject>& tasks,
                          const QJsonArray& wbs, const QString& ownTokenLabel) {
  records_ = records;
  ownLabel_ = ownTokenLabel;
  live_.clear();
  titles_.clear();
  for (auto it = records_.crbegin(); it != records_.crend(); ++it) {  // oldest first
    if (it->entityKind != QLatin1String("task")) continue;
    const QString t = it->after.value(QStringLiteral("title")).toString();
    if (!t.isEmpty()) titles_.insert(it->entityId, t);
  }
  OutlineModel model;
  model.setTasks(tasks, wbs);
  for (const QString& id : model.depthFirstIds()) {
    live_.insert(id, {model.wbsCode(id), model.task(id).value(QStringLiteral("title")).toString()});
  }

  // The actors in these records, "This computer" and system first, then the
  // rest by name; the current choice is kept even if it has no rows now.
  const QString keep = actorBox_->currentData().toString();
  QStringList actors;
  for (const client::ChangeRecord& c : records_) {
    if (!actors.contains(c.actor)) actors.append(c.actor);
  }
  if (!keep.isEmpty() && !actors.contains(keep)) actors.append(keep);
  auto rank = [this](const QString& a) {
    if (changeActorName(a, ownLabel_) == tr("This computer")) return 0;
    if (a.isEmpty() || a == QLatin1String("system")) return 1;
    return 2;
  };
  std::stable_sort(actors.begin(), actors.end(), [&](const QString& a, const QString& b) {
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    return a < b;
  });
  {
    const QSignalBlocker block(actorBox_);
    actorBox_->clear();
    actorBox_->addItem(tr("Everyone"), QString());
    for (const QString& a : actors) {
      actorBox_->addItem(changeActorName(a, ownLabel_), a);
      actorBox_->setItemData(actorBox_->count() - 1, changeActorLabel(a), Qt::ToolTipRole);
    }
    const int i = actorBox_->findData(keep);
    actorBox_->setCurrentIndex(i >= 0 ? i : 0);
  }
  fill();
}

void ActivityView::setActorFilter(const QString& actor) {
  const int i = actorBox_->findData(actor);
  if (i >= 0) actorBox_->setCurrentIndex(i);
}

void ActivityView::setKindFilter(const QString& kind) {
  const int i = kindBox_->findData(kind);
  if (i >= 0) kindBox_->setCurrentIndex(i);
}

qint64 ActivityView::currentSeq() const {
  const int row = table_->currentRow();
  QTableWidgetItem* it = row >= 0 ? table_->item(row, When) : nullptr;
  return it ? it->data(kSeqRole).toLongLong() : 0;
}

void ActivityView::fill() {
  const qint64 keep = currentSeq();
  const QString actor = actorBox_->currentData().toString();
  const QString kind = kindBox_->currentData().toString();
  shown_.clear();
  for (const client::ChangeRecord& c : records_) {
    if (!actor.isEmpty() && c.actor != actor) continue;
    if (!kind.isEmpty() && c.entityKind != kind) continue;
    shown_.append(c);
  }
  table_->setRowCount(0);
  table_->setRowCount(static_cast<int>(shown_.size()));
  for (int r = 0; r < shown_.size(); ++r) {
    const client::ChangeRecord& c = shown_.at(r);
    const QString when = changeLocalTime(c.ts);
    const QString who = changeActorName(c.actor, ownLabel_);
    QString item;
    QString taskId;
    if (c.entityKind == QLatin1String("project")) {
      item = tr("Project details");
    } else if (c.entityKind == QLatin1String("task")) {
      if (live_.contains(c.entityId)) {
        const TaskName& n = live_.value(c.entityId);
        item = QStringLiteral("%1 %2").arg(n.wbs, n.title).trimmed();
        taskId = c.entityId;
      } else {
        // Deleted since: named by its last title in these records.
        const QString title = titles_.value(c.entityId);
        item = title.isEmpty() ? tr("A deleted task") : tr("%1 (deleted)").arg(title);
      }
    } else {
      item = c.entityKind;
    }
    const QString what = describeChange(c.entityKind, c.before, c.after);
    auto put = [this, r](int col, const QString& text, const QString& tip = {}) {
      auto* i = new QTableWidgetItem(text);
      i->setToolTip(tip.isEmpty() ? text : tip);
      i->setData(Qt::AccessibleTextRole, text);
      table_->setItem(r, col, i);
      return i;
    };
    QTableWidgetItem* first = put(When, when, QStringLiteral("%1 · %2").arg(when, c.ts));
    first->setData(kSeqRole, c.seq);
    first->setData(kTaskRole, taskId);
    // The whole row in words, for screen readers.
    first->setData(Qt::AccessibleTextRole, tr("%1, %2, %3: %4").arg(when, who, item, what));
    put(Who, who, changeActorLabel(c.actor));
    put(Item, item, taskId.isEmpty() ? item : tr("%1 — Enter opens it in the outline").arg(item));
    put(Change, what);
    if (c.seq == keep) table_->setCurrentCell(r, When);
  }
  table_->resizeColumnsToContents();
  if (table_->currentRow() < 0 && !shown_.isEmpty()) table_->setCurrentCell(0, When);

  QString text;
  if (!notice_.isEmpty()) {
    text = notice_;
  } else if (records_.isEmpty()) {
    text = tr("No changes recorded for this project yet.");
  } else if (shown_.isEmpty()) {
    text = tr("No changes match the filters.");
  } else {
    text = shown_.size() == 1 ? tr("1 change") : tr("%1 changes, newest first").arg(QLocale().toString(shown_.size()));
    if (shown_.size() != records_.size()) text += tr(" (of %1)").arg(QLocale().toString(records_.size()));
    if (records_.size() >= kLimit) text += tr(" · only the newest %1 are shown").arg(QLocale().toString(kLimit));
    text += tr(" · Enter on a task opens it in the outline.");
  }
  note_->setText(text);
  note_->setAccessibleName(text);
}

void ActivityView::open() {
  const int row = table_->currentRow();
  QTableWidgetItem* it = row >= 0 ? table_->item(row, When) : nullptr;
  if (it == nullptr) return;
  const QString task = it->data(kTaskRole).toString();
  if (!task.isEmpty()) emit openTask(task);
}

bool ActivityView::eventFilter(QObject* watched, QEvent* event) {
  if (watched == table_ && event->type() == QEvent::KeyPress) {
    auto* key = static_cast<QKeyEvent*>(event);
    if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && key->modifiers() == Qt::NoModifier) {
      open();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

}  // namespace zproject::app
