#include "app/MilestonesView.hpp"

#include <QCheckBox>
#include <QCollator>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "app/OutlineModel.hpp"

namespace zproject::app {

namespace {

constexpr int kTaskIdRole = Qt::UserRole + 1;
constexpr int kProjectIdRole = Qt::UserRole + 2;

QString str(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }

QString dateText(const QDate& d) { return d.isValid() ? QLocale().toString(d, QStringLiteral("d MMM yyyy")) : QString(); }

// A milestone diamond (or, `square`, a swatch) in a project's colour,
// outlined in the theme's text colour so a pale colour still shows on any
// background; hollow once done.
QIcon swatch(const QColor& fill, const QColor& outline, bool hollow, bool square) {
  QIcon icon;
  for (int size : {16, 32}) {
    QPixmap px(size, size);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal k = size / 16.0;
    p.setPen(QPen(hollow ? fill : outline, (hollow ? 2.0 : 1.0) * k));
    p.setBrush(hollow ? QBrush(Qt::NoBrush) : QBrush(fill));
    if (square) {
      p.drawRoundedRect(QRectF(3 * k, 3 * k, 10 * k, 10 * k), 2 * k, 2 * k);
    } else {
      p.drawPolygon(QPolygonF({QPointF(8 * k, 2 * k), QPointF(14 * k, 8 * k), QPointF(8 * k, 14 * k), QPointF(2 * k, 8 * k)}));
    }
    p.end();
    icon.addPixmap(px);
  }
  return icon;
}

// "1.10" after "1.9": WBS codes compare part by part.
bool wbsLess(const QString& a, const QString& b) {
  const QStringList pa = a.split(QLatin1Char('.'));
  const QStringList pb = b.split(QLatin1Char('.'));
  for (qsizetype i = 0; i < std::min(pa.size(), pb.size()); ++i) {
    const int x = pa[i].toInt();
    const int y = pb[i].toInt();
    if (x != y) return x < y;
  }
  return pa.size() < pb.size();
}

}  // namespace

QList<MilestoneRow> milestoneRows(const QList<MilestoneSource>& sources, const QDate& today) {
  QList<MilestoneRow> out;
  for (const MilestoneSource& s : sources) {
    // The outline's own numbering, so the codes match what the outline shows.
    OutlineModel model;
    model.setTasks(s.tasks, s.wbs);
    for (const QString& id : model.depthFirstIds()) {
      if (!model.isMilestone(id)) continue;
      const QJsonObject t = model.task(id);
      MilestoneRow r;
      r.projectId = s.projectId;
      r.projectName = s.projectName;
      // #rrggbb only (the project's validated colour); QColor(QString) is in Qt 6.4.
      if (s.projectColor.size() == 7 && s.projectColor.startsWith(QLatin1Char('#'))) {
        const QColor c(s.projectColor);
        if (c.isValid()) r.projectColor = c;
      }
      r.taskId = id;
      r.title = str(t, "title");
      r.wbs = model.wbsCode(id);
      const QString parent = model.parentId(id);
      if (!parent.isEmpty()) r.parentTitle = str(model.task(parent), "title");
      r.date = QDate::fromString(str(t, "finish"), Qt::ISODate);
      if (!r.date.isValid()) r.date = QDate::fromString(str(t, "start"), Qt::ISODate);
      r.deadline = QDate::fromString(str(t, "deadline"), Qt::ISODate);
      r.status = str(t, "status");
      r.done = r.status == QLatin1String("done");
      if (r.date.isValid()) {
        r.days = today.daysTo(r.date);
        r.late = !r.done && r.days < 0;
        r.afterDeadline = r.deadline.isValid() && r.date > r.deadline;
      }
      out.append(r);
    }
  }
  QCollator collator;
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  std::stable_sort(out.begin(), out.end(), [&collator](const MilestoneRow& a, const MilestoneRow& b) {
    if (a.date.isValid() != b.date.isValid()) return a.date.isValid();
    if (a.date != b.date) return a.date < b.date;
    if (a.projectName != b.projectName) return collator.compare(a.projectName, b.projectName) < 0;
    return wbsLess(a.wbs, b.wbs);
  });
  return out;
}

QString milestoneDueText(const MilestoneRow& r) {
  if (r.done) return QObject::tr("Done");
  if (!r.date.isValid()) return QObject::tr("No date");
  if (r.days == 0) return QObject::tr("Today");
  if (r.days == 1) return QObject::tr("Tomorrow");
  if (r.days > 1) return QObject::tr("In %1 days").arg(r.days);
  if (r.days == -1) return QObject::tr("1 day late");
  return QObject::tr("%1 days late").arg(-r.days);
}

QString milestoneAccessibleText(const MilestoneRow& r, bool withProject) {
  QStringList parts;
  parts << QObject::tr("Milestone %1 %2").arg(r.wbs, r.title).trimmed();
  if (withProject) parts << r.projectName;
  if (r.date.isValid()) parts << dateText(r.date);
  parts << milestoneDueText(r).toLower();
  if (r.afterDeadline) parts << QObject::tr("after its deadline of %1").arg(dateText(r.deadline));
  return parts.join(QStringLiteral(", "));
}

MilestonesView::MilestonesView(QWidget* parent) : QWidget(parent), today_(QDate::currentDate()) {
  setObjectName(QStringLiteral("milestonesView"));
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);

  auto* bar = new QWidget(this);
  bar->setObjectName(QStringLiteral("milestonesBar"));
  auto* h = new QHBoxLayout(bar);
  h->setContentsMargins(10, 6, 10, 6);
  heading_ = new QLabel(bar);
  heading_->setObjectName(QStringLiteral("milestonesHeading"));
  h->addWidget(heading_, 1);
  all_ = new QCheckBox(tr("&All projects"), bar);
  all_->setObjectName(QStringLiteral("milestonesAllProjects"));
  all_->setToolTip(tr("Show the milestones of every project"));
  connect(all_, &QCheckBox::toggled, this, [this](bool on) {
    table_->setColumnHidden(Project, !on);
    updateHeading();
    emit allProjectsChanged(on);
  });
  h->addWidget(all_);
  outlineButton_ = new QPushButton(tr("Show in &Outline"), bar);
  outlineButton_->setObjectName(QStringLiteral("milestonesShowOutline"));
  connect(outlineButton_, &QPushButton::clicked, this, [this] { open(false); });
  h->addWidget(outlineButton_);
  ganttButton_ = new QPushButton(tr("Show in &Gantt"), bar);
  ganttButton_->setObjectName(QStringLiteral("milestonesShowGantt"));
  connect(ganttButton_, &QPushButton::clicked, this, [this] { open(true); });
  h->addWidget(ganttButton_);
  v->addWidget(bar);

  table_ = new QTableWidget(0, ColumnCount, this);
  table_->setObjectName(QStringLiteral("milestonesTable"));
  table_->setAccessibleName(tr("Milestones"));
  table_->setHorizontalHeaderLabels({tr("Milestone"), tr("Project"), tr("WBS"), tr("Date"), tr("Deadline"),
                                     tr("Status"), tr("Due"), tr("Under")});
  table_->verticalHeader()->hide();
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setTabKeyNavigation(false);
  table_->setShowGrid(false);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setColumnHidden(Project, true);
  table_->installEventFilter(this);
  connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { open(false); });
  connect(table_, &QTableWidget::currentCellChanged, this, [this] { updateButtons(); });
  v->addWidget(table_, 1);

  note_ = new QLabel(this);
  note_->setObjectName(QStringLiteral("milestonesNote"));
  note_->setContentsMargins(10, 4, 10, 4);
  note_->setWordWrap(true);
  v->addWidget(note_);

  setFocusProxy(table_);
  updateHeading();
  updateButtons();
}

void MilestonesView::setToday(const QDate& today) { today_ = today; }

void MilestonesView::setOutlineColor(const QColor& c) {
  outline_ = c;
  decorate();
}

void MilestonesView::decorate() {
  const bool all = allProjects();
  for (int i = 0; i < rows_.size() && i < table_->rowCount(); ++i) {
    const MilestoneRow& r = rows_.at(i);
    QTableWidgetItem* title = table_->item(i, Title);
    QTableWidgetItem* project = table_->item(i, Project);
    if (title == nullptr || project == nullptr) continue;
    const bool coloured = all && r.projectColor.isValid();
    // With a colour the drawn diamond replaces the text one.
    title->setText(coloured ? r.title : (r.done ? QStringLiteral("◇ ") : QStringLiteral("◆ ")) + r.title);
    title->setIcon(coloured ? swatch(r.projectColor, outline_, r.done, false) : QIcon());
    project->setIcon(coloured ? swatch(r.projectColor, outline_, false, true) : QIcon());
    project->setToolTip(coloured ? tr("%1 (colour %2)").arg(r.projectName, r.projectColor.name()) : r.projectName);
  }
}

void MilestonesView::setProject(const QString& id, const QString& name) {
  projectId_ = id;
  projectName_ = name;
  updateHeading();
}

bool MilestonesView::allProjects() const { return all_->isChecked(); }

void MilestonesView::setAllProjects(bool all) { all_->setChecked(all); }

void MilestonesView::updateHeading() {
  const QString text = allProjects() ? tr("Milestones — all projects") : tr("Milestones — %1").arg(projectName_);
  heading_->setText(QStringLiteral("<b>%1</b>").arg(text.toHtmlEscaped()));
  heading_->setAccessibleName(text);
}

void MilestonesView::reload(const QList<MilestoneSource>& sources) {
  const QString keep = currentTaskId();
  rows_ = milestoneRows(sources, today_);
  const bool withProject = allProjects();
  table_->setRowCount(0);
  table_->setRowCount(static_cast<int>(rows_.size()));
  QFont bold = table_->font();
  bold.setBold(true);
  int late = 0;
  int undated = 0;
  for (int i = 0; i < rows_.size(); ++i) {
    const MilestoneRow& r = rows_.at(i);
    if (r.late) ++late;
    if (!r.date.isValid()) ++undated;
    auto put = [this, i](int col, const QString& text, const QString& tip = {}) {
      auto* item = new QTableWidgetItem(text);
      item->setToolTip(tip.isEmpty() ? text : tip);
      table_->setItem(i, col, item);
      return item;
    };
    // ◆ is the Gantt's milestone diamond; done ones are hollow.
    QTableWidgetItem* title = put(Title, (r.done ? QStringLiteral("◇ ") : QStringLiteral("◆ ")) + r.title,
                                  milestoneAccessibleText(r, withProject));
    title->setData(kTaskIdRole, r.taskId);
    title->setData(kProjectIdRole, r.projectId);
    title->setData(Qt::AccessibleTextRole, milestoneAccessibleText(r, withProject));
    put(Project, r.projectName);
    put(Wbs, r.wbs);
    put(Date, r.date.isValid() ? dateText(r.date) : tr("no date"));
    QTableWidgetItem* dl = put(Deadline, r.afterDeadline ? QStringLiteral("⚠ ") + dateText(r.deadline) : dateText(r.deadline),
                               r.afterDeadline ? tr("The date is after this deadline") : QString());
    if (r.afterDeadline) {
      dl->setFont(bold);
      dl->setData(Qt::AccessibleTextRole, tr("Deadline %1, missed").arg(dateText(r.deadline)));
    }
    put(Status, OutlineModel::statusName(r.status));
    QTableWidgetItem* due = put(Due, r.late ? QStringLiteral("⚠ ") + milestoneDueText(r) : milestoneDueText(r));
    if (r.late) due->setFont(bold);
    due->setData(Qt::AccessibleTextRole, milestoneDueText(r));
    put(Parent, r.parentTitle);
    if (r.taskId == keep) table_->setCurrentCell(i, Title);
  }
  decorate();
  table_->resizeColumnsToContents();
  if (table_->currentRow() < 0 && !rows_.isEmpty()) table_->setCurrentCell(0, Title);

  QString text;
  if (rows_.isEmpty()) {
    text = allProjects() ? tr("No project has milestones yet.")
                         : tr("This project has no milestones yet. In the outline, Edit → Convert to Milestone makes one.");
  } else {
    text = rows_.size() == 1 ? tr("1 milestone") : tr("%1 milestones").arg(rows_.size());
    if (late > 0) text += late == 1 ? tr(" · 1 late") : tr(" · %1 late").arg(late);
    if (undated > 0) text += undated == 1 ? tr(" · 1 with no date") : tr(" · %1 with no date").arg(undated);
    text += tr(" · Enter opens one in the outline.");
  }
  note_->setText(text);
  note_->setAccessibleName(text);
  updateButtons();
}

QString MilestonesView::currentTaskId() const {
  const int row = table_->currentRow();
  QTableWidgetItem* it = row >= 0 ? table_->item(row, Title) : nullptr;
  return it ? it->data(kTaskIdRole).toString() : QString();
}

QString MilestonesView::currentRowProjectId() const {
  const int row = table_->currentRow();
  QTableWidgetItem* it = row >= 0 ? table_->item(row, Title) : nullptr;
  return it ? it->data(kProjectIdRole).toString() : QString();
}

bool MilestonesView::selectTask(const QString& id) {
  for (int i = 0; i < table_->rowCount(); ++i) {
    if (table_->item(i, Title)->data(kTaskIdRole).toString() == id) {
      table_->setCurrentCell(i, Title);
      return true;
    }
  }
  return false;
}

void MilestonesView::updateButtons() {
  const bool have = !currentTaskId().isEmpty();
  outlineButton_->setEnabled(have);
  ganttButton_->setEnabled(have);
}

void MilestonesView::open(bool gantt) {
  const QString id = currentTaskId();
  if (!id.isEmpty()) emit openTask(currentRowProjectId(), id, gantt);
}

bool MilestonesView::eventFilter(QObject* watched, QEvent* event) {
  if (watched == table_ && event->type() == QEvent::KeyPress) {
    auto* key = static_cast<QKeyEvent*>(event);
    if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && key->modifiers() == Qt::NoModifier) {
      open(false);
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

}  // namespace zproject::app
