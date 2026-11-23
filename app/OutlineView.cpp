#include "app/OutlineView.hpp"

#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "app/OutlineModel.hpp"

namespace zproject::app {

namespace {

// Inline editors: a list for status, spin boxes for % and priority, a line
// for the title.
class OutlineDelegate : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    switch (index.column()) {
      case OutlineModel::Status: {
        auto* box = new QComboBox(parent);
        box->setObjectName(QStringLiteral("statusEditor"));
        for (const char* s : {"todo", "in_progress", "done"}) {
          box->addItem(OutlineModel::statusName(QString::fromLatin1(s)), QString::fromLatin1(s));
        }
        box->setAccessibleName(QObject::tr("Status"));
        return box;
      }
      case OutlineModel::Percent: {
        auto* spin = new QSpinBox(parent);
        spin->setObjectName(QStringLiteral("percentEditor"));
        spin->setRange(0, 100);
        spin->setSuffix(QStringLiteral("%"));
        spin->setAccessibleName(QObject::tr("Percent complete"));
        return spin;
      }
      case OutlineModel::Priority: {
        auto* spin = new QSpinBox(parent);
        spin->setObjectName(QStringLiteral("priorityEditor"));
        spin->setRange(1, 4);
        spin->setPrefix(QStringLiteral("P"));
        spin->setAccessibleName(QObject::tr("Priority (1 = highest)"));
        return spin;
      }
      default: {
        QWidget* w = QStyledItemDelegate::createEditor(parent, option, index);
        w->setObjectName(QStringLiteral("titleEditor"));
        w->setAccessibleName(QObject::tr("Title"));
        return w;
      }
    }
  }

  void setEditorData(QWidget* editor, const QModelIndex& index) const override {
    if (auto* box = qobject_cast<QComboBox*>(editor)) {
      box->setCurrentIndex(std::max(0, box->findData(index.data(Qt::EditRole).toString())));
      return;
    }
    if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
      spin->setValue(index.data(Qt::EditRole).toInt());
      return;
    }
    QStyledItemDelegate::setEditorData(editor, index);
  }

  void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
    if (auto* box = qobject_cast<QComboBox*>(editor)) {
      model->setData(index, box->currentData(), Qt::EditRole);
      return;
    }
    if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
      spin->interpretText();
      model->setData(index, spin->value(), Qt::EditRole);
      return;
    }
    QStyledItemDelegate::setModelData(editor, model, index);
  }
};

// The tree, telling whether an inline editor is open.
class OutlineTree : public QTreeView {
 public:
  using QTreeView::QTreeView;
  bool editing() const { return state() == QAbstractItemView::EditingState; }

 protected:
  // Tab and Shift+Tab indent and outdent here, so Ctrl+Tab and Ctrl+Shift+Tab
  // (the Control key on macOS too, where Qt calls it Meta) move the focus on,
  // as in a text editor, and keyboard users can still leave the tree.
  bool event(QEvent* e) override {
    if (e->type() == QEvent::KeyPress) {
      auto* k = static_cast<QKeyEvent*>(e);
      const bool ctrl = (k->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) != 0;
      if (ctrl && (k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab)) {
        const bool back = k->key() == Qt::Key_Backtab || (k->modifiers() & Qt::ShiftModifier) != 0;
        focusNextPrevChild(!back);
        return true;
      }
    }
    return QTreeView::event(e);
  }
};

}  // namespace

OutlineView::OutlineView(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("outlineView"));
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(12, 10, 12, 8);
  v->setSpacing(6);

  auto* head = new QHBoxLayout;
  title_ = new QLabel(this);
  title_->setObjectName(QStringLiteral("outlineTitle"));
  QFont f = title_->font();
  f.setBold(true);
  title_->setFont(f);
  head->addWidget(title_);
  counts_ = new QLabel(this);
  counts_->setObjectName(QStringLiteral("outlineCounts"));
  head->addWidget(counts_, 1);
  auto* filterLabel = new QLabel(tr("Show:"), this);
  head->addWidget(filterLabel);
  statusBox_ = new QComboBox(this);
  statusBox_->setObjectName(QStringLiteral("outlineFilter"));
  statusBox_->setAccessibleName(tr("Show tasks by status"));
  statusBox_->addItem(tr("All tasks"), QString());
  statusBox_->addItem(tr("Not done"), QStringLiteral("open"));
  statusBox_->addItem(tr("To do"), QStringLiteral("todo"));
  statusBox_->addItem(tr("In progress"), QStringLiteral("in_progress"));
  statusBox_->addItem(tr("Done"), QStringLiteral("done"));
  filterLabel->setBuddy(statusBox_);
  head->addWidget(statusBox_);
  v->addLayout(head);

  findBar_ = new QFrame(this);
  findBar_->setObjectName(QStringLiteral("outlineFindBar"));
  auto* fh = new QHBoxLayout(findBar_);
  fh->setContentsMargins(0, 0, 0, 0);
  auto* findLabel = new QLabel(tr("Find:"), findBar_);
  fh->addWidget(findLabel);
  findField_ = new QLineEdit(findBar_);
  findField_->setObjectName(QStringLiteral("outlineFind"));
  findField_->setAccessibleName(tr("Find a task by title or WBS code"));
  findField_->setPlaceholderText(tr("Title or WBS code"));
  findField_->installEventFilter(this);
  findLabel->setBuddy(findField_);
  fh->addWidget(findField_, 1);
  findStatus_ = new QLabel(findBar_);
  findStatus_->setObjectName(QStringLiteral("outlineFindStatus"));
  fh->addWidget(findStatus_);
  findPrev_ = new QToolButton(findBar_);
  findPrev_->setText(tr("Previous"));
  findPrev_->setObjectName(QStringLiteral("outlineFindPrevious"));
  fh->addWidget(findPrev_);
  findNextButton_ = new QToolButton(findBar_);
  findNextButton_->setText(tr("Next"));
  findNextButton_->setObjectName(QStringLiteral("outlineFindNext"));
  fh->addWidget(findNextButton_);
  findClose_ = new QToolButton(findBar_);
  findClose_->setText(tr("Done"));
  findClose_->setObjectName(QStringLiteral("outlineFindClose"));
  fh->addWidget(findClose_);
  findBar_->hide();
  v->addWidget(findBar_);

  message_ = new QLabel(this);
  message_->setObjectName(QStringLiteral("outlineMessage"));
  message_->setWordWrap(true);
  message_->hide();
  v->addWidget(message_);

  model_ = new OutlineModel(this);
  filter_ = new OutlineFilter(this);
  filter_->setSourceModel(model_);
  tree_ = new OutlineTree(this);
  tree_->setObjectName(QStringLiteral("outlineTree"));
  tree_->setAccessibleName(tr("Tasks (WBS outline)"));
  tree_->setModel(filter_);
  tree_->setItemDelegate(new OutlineDelegate(tree_));
  tree_->setUniformRowHeights(true);
  tree_->setAlternatingRowColors(true);
  tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
  tree_->setSelectionMode(QAbstractItemView::SingleSelection);
  tree_->setAllColumnsShowFocus(true);
  tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
  tree_->setExpandsOnDoubleClick(false);
  tree_->header()->setStretchLastSection(false);
  tree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
  tree_->header()->setSectionResizeMode(OutlineModel::Title, QHeaderView::Stretch);
  v->addWidget(tree_, 1);

  empty_ = new QLabel(this);
  empty_->setObjectName(QStringLiteral("outlineEmpty"));
  empty_->setAlignment(Qt::AlignCenter);
  empty_->setWordWrap(true);
  empty_->hide();
  v->addWidget(empty_, 1);

  connect(statusBox_, &QComboBox::currentIndexChanged, this, [this] {
    filter_->setStatusFilter(statusBox_->currentData().toString());
    restoreExpansion();
    updateMatches();
    updateCounts();
  });
  connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex& i) {
    if (!restoring_) collapsed_.insert(i.data(OutlineModel::IdRole).toString());
  });
  connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& i) {
    if (!restoring_) collapsed_.remove(i.data(OutlineModel::IdRole).toString());
  });
  connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this] { emit currentTaskChanged(currentTaskId()); });
  connect(findField_, &QLineEdit::textChanged, this, [this] {
    updateMatches();
    if (!matches_.isEmpty()) {
      selectTask(matches_.first());
      updateMatches();
    }
  });
  connect(findPrev_, &QToolButton::clicked, this, [this] { findNext(true); });
  connect(findNextButton_, &QToolButton::clicked, this, [this] { findNext(false); });
  connect(findClose_, &QToolButton::clicked, this, [this] { hideFind(); });
  // A refresh never pulls the rug from under an open inline editor: it
  // waits until the edit is done (the reload then shows the server's value).
  connect(tree_->itemDelegate(), &QAbstractItemDelegate::closeEditor, this, [this] {
    QMetaObject::invokeMethod(
        this,
        [this] {
          if (!hasDeferred_) return;
          hasDeferred_ = false;
          reload(deferredTasks_, deferredWbs_);
        },
        Qt::QueuedConnection);
  });
}

void OutlineView::setProject(const QString& id, const QString& name) {
  if (id != projectId_) {
    collapsed_.clear();
    matches_.clear();
  }
  projectId_ = id;
  title_->setText(name);
  title_->setAccessibleName(tr("Outline of %1").arg(name));
}

void OutlineView::reload(const QList<QJsonObject>& tasks, const QJsonArray& wbs) {
  if (static_cast<OutlineTree*>(tree_)->editing()) {
    deferredTasks_ = tasks;
    deferredWbs_ = wbs;
    hasDeferred_ = true;
    return;
  }
  hasDeferred_ = false;
  const QString current = pending_.isEmpty() ? currentTaskId() : pending_;
  restoring_ = true;
  model_->setTasks(tasks, wbs);
  restoreExpansion();
  restoring_ = false;
  const bool any = model_->taskCount() > 0;
  tree_->setVisible(any);
  empty_->setVisible(!any);
  empty_->setText(tr("No tasks yet.\nEdit → Insert Task (Ins) adds the first one."));
  if (!current.isEmpty() && selectTask(current)) {
    if (current == pending_) {
      if (pendingEdit_) editTitle(current);
      pending_.clear();
      pendingEdit_ = false;
    }
  } else if (any && !tree_->currentIndex().isValid()) {
    tree_->setCurrentIndex(filter_->index(0, OutlineModel::Title));
  }
  updateMatches();
  updateCounts();
}

void OutlineView::restoreExpansion() {
  const bool was = restoring_;
  restoring_ = true;
  tree_->expandAll();
  for (const QString& id : std::as_const(collapsed_)) {
    const QModelIndex i = filter_->mapFromSource(model_->indexOf(id, 0));
    if (i.isValid()) tree_->collapse(i);
  }
  restoring_ = was;
}

void OutlineView::updateCounts() {
  const int n = model_->taskCount();
  QString text = n == 1 ? tr("1 task") : tr("%1 tasks").arg(n);
  if (!filter_->statusFilter().isEmpty()) text += tr(" · filtered: %1").arg(statusBox_->currentText());
  if (!model_->serverWbs() && n > 0) text += tr(" · WBS codes update after the next refresh");
  counts_->setText(text);
}

void OutlineView::setEditable(bool on) {
  model_->setEditable(on);
  tree_->setEditTriggers(on ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                            : QAbstractItemView::NoEditTriggers);
}

void OutlineView::setPending(const QString& id, bool editTitle) {
  pending_ = id;
  pendingEdit_ = editTitle;
  if (!id.isEmpty() && model_->contains(id) && !editTitle) {
    selectTask(id);
    pending_.clear();
  }
}

QString OutlineView::currentTaskId() const {
  return tree_->currentIndex().data(OutlineModel::IdRole).toString();
}

bool OutlineView::selectTask(const QString& id) {
  QModelIndex i = filter_->mapFromSource(model_->indexOf(id, OutlineModel::Title));
  if (!i.isValid()) return false;
  for (QModelIndex p = i.parent(); p.isValid(); p = p.parent()) tree_->expand(p);
  tree_->setCurrentIndex(i);
  tree_->scrollTo(i);
  return true;
}

void OutlineView::editTitle(const QString& id) {
  if (!model_->editable() || !selectTask(id)) return;
  tree_->setFocus(Qt::OtherFocusReason);
  tree_->edit(tree_->currentIndex().siblingAtColumn(OutlineModel::Title));
}

void OutlineView::setStatusFilter(const QString& status) {
  const int i = statusBox_->findData(status);
  if (i >= 0) statusBox_->setCurrentIndex(i);
}

void OutlineView::showFind() {
  findBar_->show();
  findField_->setFocus(Qt::ShortcutFocusReason);
  findField_->selectAll();
  updateMatches();
}

void OutlineView::hideFind() {
  findBar_->hide();
  tree_->setFocus(Qt::OtherFocusReason);
}

void OutlineView::updateMatches() {
  matches_.clear();
  const QString q = findField_->text().trimmed();
  if (!q.isEmpty()) {
    for (const QString& id : model_->depthFirstIds()) {
      const QString title = model_->task(id).value(QStringLiteral("title")).toString();
      const bool hit = title.contains(q, Qt::CaseInsensitive) || model_->wbsCode(id) == q;
      if (hit && filter_->mapFromSource(model_->indexOf(id, 0)).isValid()) matches_ << id;
    }
  }
  const int at = static_cast<int>(matches_.indexOf(currentTaskId()));
  if (q.isEmpty()) {
    findStatus_->clear();
  } else if (matches_.isEmpty()) {
    findStatus_->setText(tr("No matches"));
  } else {
    findStatus_->setText(at >= 0 ? tr("%1 of %2").arg(at + 1).arg(matches_.size())
                                     : (matches_.size() == 1 ? tr("1 match") : tr("%1 matches").arg(matches_.size())));
  }
  findStatus_->setAccessibleName(findStatus_->text());
}

void OutlineView::findNext(bool backwards) {
  if (matches_.isEmpty()) updateMatches();
  if (matches_.isEmpty()) return;
  const int n = static_cast<int>(matches_.size());
  const int at = static_cast<int>(matches_.indexOf(currentTaskId()));
  int next = 0;
  if (at < 0) {
    next = backwards ? n - 1 : 0;
  } else {
    next = backwards ? (at - 1 + n) % n : (at + 1) % n;
  }
  selectTask(matches_.value(next));
  findStatus_->setText(tr("%1 of %2").arg(next + 1).arg(n));
  findStatus_->setAccessibleName(findStatus_->text());
}

void OutlineView::showMessage(const QString& text, bool error) {
  message_->setText(error ? QStringLiteral("⚠ ") + text : text);
  message_->setProperty("error", error);
  message_->setAccessibleName(message_->text());
  message_->setVisible(!text.isEmpty());
}

bool OutlineView::eventFilter(QObject* watched, QEvent* event) {
  if (watched == findField_ && event->type() == QEvent::KeyPress) {
    auto* k = static_cast<QKeyEvent*>(event);
    if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
      findNext(k->modifiers() & Qt::ShiftModifier);
      return true;
    }
    if (k->key() == Qt::Key_Escape) {
      hideFind();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

}  // namespace zproject::app
