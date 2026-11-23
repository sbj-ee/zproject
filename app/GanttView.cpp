#include "app/GanttView.hpp"

#include <QAccessibleWidget>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScrollBar>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QToolTip>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

#include "app/OutlineModel.hpp"

namespace zproject::app {
namespace {

using M = OutlineModel;

const QColor kLate(0xd2, 0x3b, 0x3b);

QDate isoDate(const QString& s) { return s.isEmpty() ? QDate() : QDate::fromString(s, Qt::ISODate); }

QString dayMonth(const QDate& d) {
  return QStringLiteral("%1 %2").arg(d.day()).arg(QLocale().monthName(d.month(), QLocale::ShortFormat));
}

// "9–11 Sep", "28 Sep – 2 Oct", "18 Dec".
QString dateRange(const QDate& a, const QDate& b) {
  if (!b.isValid() || a == b) return dayMonth(a);
  if (a.year() == b.year() && a.month() == b.month()) {
    return QStringLiteral("%1–%2 %3").arg(a.day()).arg(b.day()).arg(QLocale().monthName(a.month(), QLocale::ShortFormat));
  }
  return QStringLiteral("%1 – %2").arg(dayMonth(a), dayMonth(b));
}

// The table shows "unscheduled" (italic, muted) where a task has no dates.
class UnscheduledDelegate : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

 protected:
  void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
    QStyledItemDelegate::initStyleOption(option, index);
    if (option == nullptr || !option->text.isEmpty()) return;
    option->text = QObject::tr("unscheduled");
    option->features |= QStyleOptionViewItem::HasDisplay;
    option->font.setItalic(true);
    option->palette.setColor(QPalette::Text, option->palette.color(QPalette::PlaceholderText));
  }
};

// One bar for screen readers: its name and description, its rectangle.
class GanttBarAccessible : public QAccessibleInterface {
 public:
  GanttBarAccessible(GanttChart* chart, QString id) : chart_(chart), id_(std::move(id)) {}
  bool isValid() const override { return chart_ != nullptr && chart_->hasBar(id_); }
  QObject* object() const override { return nullptr; }
  QWindow* window() const override {
    GanttChart* c = chart_.data();
    return c != nullptr && c->window() != nullptr ? c->window()->windowHandle() : nullptr;
  }
  QAccessibleInterface* parent() const override { return QAccessible::queryAccessibleInterface(chart_.data()); }
  QAccessibleInterface* child(int) const override { return nullptr; }
  int childCount() const override { return 0; }
  int indexOfChild(const QAccessibleInterface*) const override { return -1; }
  QAccessibleInterface* childAt(int, int) const override { return nullptr; }
  QString text(QAccessible::Text t) const override {
    GanttChart* c = chart_.data();
    if (c == nullptr) return {};
    if (t == QAccessible::Name) return c->barName(id_);
    if (t == QAccessible::Description) return c->barDescription(id_);
    return {};
  }
  void setText(QAccessible::Text, const QString&) override {}
  QRect rect() const override {
    GanttChart* c = chart_.data();
    if (c == nullptr) return {};
    const QRect r = c->barRect(id_);
    return QRect(c->mapToGlobal(r.topLeft()), r.size());
  }
  QAccessible::Role role() const override { return QAccessible::ListItem; }
  QAccessible::State state() const override {
    QAccessible::State s;
    s.selectable = true;
    s.readOnly = true;
    if (!isValid()) s.invisible = true;
    return s;
  }
  QString id() const { return id_; }

 private:
  QPointer<GanttChart> chart_;
  QString id_;
};

// The timeline as a list of its bars (the rows in view).
class GanttChartAccessible : public QAccessibleWidget {
 public:
  explicit GanttChartAccessible(QWidget* w) : QAccessibleWidget(w, QAccessible::List) {}
  int childCount() const override { return static_cast<int>(chart()->visibleBars().size()); }
  QAccessibleInterface* child(int index) const override {
    const QList<GanttChart::Bar> bars = chart()->visibleBars();
    if (index < 0 || index >= bars.size()) return nullptr;
    return bar(bars.at(index).id);
  }
  int indexOfChild(const QAccessibleInterface* child) const override {
    const auto* b = dynamic_cast<const GanttBarAccessible*>(child);
    if (b == nullptr) return -1;
    const QList<GanttChart::Bar> bars = chart()->visibleBars();
    for (int i = 0; i < bars.size(); ++i) {
      if (bars.at(i).id == b->id()) return i;
    }
    return -1;
  }
  QAccessibleInterface* childAt(int x, int y) const override {
    const QPoint p = chart()->mapFromGlobal(QPoint(x, y));
    // Anywhere on a bar's row, scrolled into view or not.
    for (const GanttChart::Bar& b : chart()->visibleBars()) {
      if (p.y() >= b.row.top() && p.y() <= b.row.bottom()) return bar(b.id);
    }
    return nullptr;
  }
  QString text(QAccessible::Text t) const override {
    if (t == QAccessible::Name) return QObject::tr("Gantt timeline");
    if (t == QAccessible::Description) {
      return QObject::tr("Read-only timeline, %1 scale").arg(GanttChart::scaleName(chart()->scale()));
    }
    return QAccessibleWidget::text(t);
  }

 private:
  GanttChart* chart() const { return static_cast<GanttChart*>(widget()); }
  QAccessibleInterface* bar(const QString& id) const {
    QHash<QString, QAccessible::Id>& ids = chart()->accessibleIds();
    if (const auto it = ids.constFind(id); it != ids.constEnd()) {
      if (QAccessibleInterface* known = QAccessible::accessibleInterface(it.value())) return known;
    }
    auto* iface = new GanttBarAccessible(chart(), id);
    ids.insert(id, QAccessible::registerAccessibleInterface(iface));
    return iface;
  }
};

QAccessibleInterface* ganttAccessibleFactory(const QString& className, QObject* object) {
  if (className == QLatin1String("zproject::app::GanttChart") && object != nullptr && object->isWidgetType()) {
    return new GanttChartAccessible(static_cast<QWidget*>(object));
  }
  return nullptr;
}

}  // namespace

// ---- GanttChart ----

GanttChart::GanttChart(QTreeView* table, OutlineModel* model, QWidget* parent)
    : QWidget(parent), table_(table), model_(model), c_(Theme::colors(QStringLiteral("light"))) {
  static const bool installed = [] {
    QAccessible::installFactory(ganttAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed);
  setObjectName(QStringLiteral("ganttChart"));
  setAccessibleName(tr("Gantt timeline"));
  setMouseTracking(true);
  setMinimumWidth(200);
  // Clicks select in the table, which keeps the keyboard focus.
  setFocusPolicy(Qt::ClickFocus);
  setFocusProxy(table_);
  today_ = QDate::currentDate();
  hbar_ = new QScrollBar(Qt::Horizontal, this);
  hbar_->setObjectName(QStringLiteral("ganttHorizontalBar"));
  hbar_->setAccessibleName(tr("Scroll the timeline"));
  connect(hbar_, &QScrollBar::valueChanged, this, [this] { update(); });
  // The table's own vertical bar is hidden; this one, at our right edge,
  // scrolls both.
  vbar_ = new QScrollBar(Qt::Vertical, this);
  vbar_->setObjectName(QStringLiteral("ganttVerticalBar"));
  vbar_->setAccessibleName(tr("Scroll the tasks"));
  QScrollBar* tv = table_->verticalScrollBar();
  auto follow = [this, tv] {
    const QSignalBlocker block(vbar_);
    vbar_->setRange(tv->minimum(), tv->maximum());
    vbar_->setPageStep(tv->pageStep());
    vbar_->setSingleStep(tv->singleStep());
    vbar_->setValue(tv->value());
    vbar_->setVisible(tv->maximum() > tv->minimum());
    updateScrollBar();
  };
  connect(tv, &QScrollBar::rangeChanged, this, follow);
  connect(tv, &QScrollBar::valueChanged, this, [this](int v) {
    const QSignalBlocker block(vbar_);
    vbar_->setValue(v);
    update();
  });
  connect(vbar_, &QScrollBar::valueChanged, tv, &QScrollBar::setValue);
  follow();
  connect(table_, &QTreeView::expanded, this, [this] { update(); });
  connect(table_, &QTreeView::collapsed, this, [this] { update(); });
  updateRange();
}

GanttChart::~GanttChart() { dropAccessibleChildren(); }

void GanttChart::dropAccessibleChildren() {
  for (const QAccessible::Id id : std::as_const(a11y_)) QAccessible::deleteAccessibleInterface(id);
  a11y_.clear();
}

QString GanttChart::scaleName(Scale s) {
  switch (s) {
    case Scale::Day: return tr("Day");
    case Scale::Week: return tr("Week");
    case Scale::Month: return tr("Month");
    case Scale::Quarter: return tr("Quarter");
  }
  return {};
}

void GanttChart::setColors(const ThemeColors& c) {
  c_ = c;
  update();
}

void GanttChart::setToday(const QDate& today) {
  today_ = today;
  updateRange();
}

double GanttChart::pxPerDay() const {
  switch (scale_) {
    case Scale::Day: return 28.0;
    case Scale::Week: return 12.0;
    case Scale::Month: return 4.0;
    case Scale::Quarter: return 1.5;
  }
  return 12.0;
}

void GanttChart::setScale(Scale s) {
  if (s == scale_) return;
  // Keep the date in the middle where it is.
  const QDate mid = dateAt(width() / 2);
  scale_ = s;
  updateScrollBar();
  const double target = mid.isValid() ? static_cast<double>(first_.daysTo(mid)) * pxPerDay() - width() / 2.0 : 0.0;
  hbar_->setValue(static_cast<int>(std::lround(target)));
  update();
}

void GanttChart::zoomIn() {
  if (scale_ == Scale::Quarter) setScale(Scale::Month);
  else if (scale_ == Scale::Month) setScale(Scale::Week);
  else if (scale_ == Scale::Week) setScale(Scale::Day);
}

void GanttChart::zoomOut() {
  if (scale_ == Scale::Day) setScale(Scale::Week);
  else if (scale_ == Scale::Week) setScale(Scale::Month);
  else if (scale_ == Scale::Month) setScale(Scale::Quarter);
}

void GanttChart::scrollToDate(const QDate& date) {
  if (!date.isValid()) return;
  const double x = static_cast<double>(first_.daysTo(date)) * pxPerDay() - width() / 3.0;
  hbar_->setValue(static_cast<int>(std::lround(x)));
}

void GanttChart::updateRange() {
  dropAccessibleChildren();
  QDate lo;
  QDate hi;
  auto take = [&lo, &hi](const QDate& d) {
    if (!d.isValid()) return;
    if (!lo.isValid() || d < lo) lo = d;
    if (!hi.isValid() || d > hi) hi = d;
  };
  for (const QString& id : model_->depthFirstIds()) {
    take(isoDate(model_->indexOf(id, M::Start).data().toString()));
    take(isoDate(model_->indexOf(id, M::Finish).data().toString()));
    take(isoDate(model_->task(id).value(QStringLiteral("deadline")).toString()));
  }
  take(today_);
  if (!lo.isValid()) lo = hi = QDate::currentDate();
  // A week before (from a Monday) to a month after.
  lo = lo.addDays(-7);
  first_ = lo.addDays(1 - lo.dayOfWeek());
  last_ = hi.addDays(31);
  updateScrollBar();
  update();
}

int GanttChart::contentWidth() const {
  return static_cast<int>(std::ceil(static_cast<double>(first_.daysTo(last_) + 1) * pxPerDay()));
}

int GanttChart::rowsWidth() const { return width() - (vbar_ != nullptr && vbar_->isVisible() ? vbar_->sizeHint().width() : 0); }

void GanttChart::updateScrollBar() {
  const int h = hbar_->sizeHint().height();
  const int w = rowsWidth();
  hbar_->setGeometry(0, height() - h, w, h);
  if (vbar_ != nullptr) {
    const int top = rowsTop();
    vbar_->setGeometry(w, top, width() - w, std::max(0, height() - h - top));
  }
  hbar_->setRange(0, std::max(0, contentWidth() - w));
  hbar_->setPageStep(std::max(1, w));
  hbar_->setSingleStep(std::max(1, static_cast<int>(pxPerDay())));
}

void GanttChart::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  updateScrollBar();
}

double GanttChart::xOf(const QDate& date) const {
  return static_cast<double>(first_.daysTo(date)) * pxPerDay() - hbar_->value();
}

QDate GanttChart::dateAt(int x) const {
  if (!first_.isValid()) return {};
  return first_.addDays(static_cast<qint64>(std::floor((x + hbar_->value()) / pxPerDay())));
}

int GanttChart::rowsTop() const {
  // The table's rows start below its header; ours start at the same height.
  const QPoint p = table_->viewport()->mapTo(window(), QPoint(0, 0));
  return std::max(mapFrom(window(), p).y(), 0);
}

QList<GanttChart::Bar> GanttChart::layoutRows(int* rows) const {
  QList<Bar> out;
  int count = 0;
  const int top = rowsTop();
  const int bottom = height() - hbar_->height();
  const QWidget* vp = table_->viewport();
  const int offset = top;  // viewport y 0 is our `top`
  QModelIndex index = table_->indexAt(QPoint(1, 1));
  const double ppd = pxPerDay();
  while (index.isValid()) {
    const QRect vr = table_->visualRect(index);
    if (vr.top() > vp->height()) break;
    const QRect row(0, vr.top() + offset, rowsWidth(), vr.height());
    if (row.top() > bottom) break;
    ++count;
    const QString id = model_->idAt(index);
    Bar b;
    b.id = id;
    b.row = row;
    b.summary = model_->isSummary(id);
    b.milestone = model_->isMilestone(id) && !b.summary;
    const QJsonObject t = model_->task(id);
    b.manual = t.value(QStringLiteral("mode")).toString() == QLatin1String("manual");
    const QDate s = isoDate(model_->indexOf(id, M::Start).data().toString());
    QDate f = isoDate(model_->indexOf(id, M::Finish).data().toString());
    const QDate deadline = isoDate(t.value(QStringLiteral("deadline")).toString());
    const QDate due = f.isValid() ? f : s;
    b.late = deadline.isValid() && due.isValid() && due > deadline;
    if (s.isValid() || f.isValid()) {
      const QDate from = s.isValid() ? s : f;
      if (!f.isValid() || f < from) f = from;
      const int h = row.height();
      if (b.milestone) {
        const double cx = xOf(from) + ppd / 2.0;
        const int size = std::max(8, h / 2);
        b.rect = QRect(static_cast<int>(cx) - size / 2, row.top() + (h - size) / 2, size, size);
      } else if (b.summary) {
        const int x0 = static_cast<int>(xOf(from));
        const int x1 = static_cast<int>(xOf(f.addDays(1)));
        b.rect = QRect(x0, row.top() + h / 3, std::max(2, x1 - x0), std::max(4, h / 3));
      } else {
        const int x0 = static_cast<int>(xOf(from));
        const int x1 = static_cast<int>(xOf(f.addDays(1)));
        const int bh = std::max(6, h * 11 / 20);
        b.rect = QRect(x0, row.top() + (h - bh) / 2, std::max(3, x1 - x0 - 1), bh);
      }
      out.append(b);
    } else {
      out.append(b);  // unscheduled: a row, no bar
    }
    index = table_->indexBelow(index);
  }
  if (rows != nullptr) *rows = count;
  return out;
}

QList<GanttChart::Bar> GanttChart::visibleBars() const { return layoutRows(nullptr); }

bool GanttChart::hasBar(const QString& id) const {
  const QList<Bar> bars = visibleBars();
  return std::any_of(bars.cbegin(), bars.cend(), [&id](const Bar& b) { return b.id == id; });
}

QRect GanttChart::barRect(const QString& id) const {
  for (const Bar& b : visibleBars()) {
    if (b.id == id) return b.rect.isNull() ? b.row : b.rect;
  }
  return {};
}

QString GanttChart::barName(const QString& id) const {
  if (!model_->contains(id)) return {};
  const QString title = model_->task(id).value(QStringLiteral("title")).toString();
  const QString wbs = model_->wbsCode(id);
  if (model_->isSummary(id)) return tr("Summary %1 %2").arg(wbs, title);
  if (model_->isMilestone(id)) return tr("Milestone %1 %2").arg(wbs, title);
  return tr("Task %1 %2").arg(wbs, title);
}

QString GanttChart::barDescription(const QString& id) const {
  if (!model_->contains(id)) return {};
  const QJsonObject t = model_->task(id);
  const QDate s = isoDate(model_->indexOf(id, M::Start).data().toString());
  const QDate f = isoDate(model_->indexOf(id, M::Finish).data().toString());
  const int pct = qRound(t.value(QStringLiteral("percentComplete")).toDouble());
  QStringList parts;
  const bool summary = model_->isSummary(id);
  if (summary) {
    const int kids = model_->liveChildren(id);
    parts << (kids == 1 ? tr("1 subtask") : tr("%1 subtasks").arg(kids));
  } else if (!model_->isMilestone(id)) {
    parts << model_->indexOf(id, M::Duration).data().toString();
  }
  if (s.isValid() || f.isValid()) {
    parts << dateRange(s.isValid() ? s : f, f);
  } else {
    parts << tr("unscheduled");
  }
  if (!model_->isMilestone(id) || summary) {
    parts << (summary ? tr("%1% complete (rolled up)").arg(pct) : tr("%1% complete").arg(pct));
  } else if (pct >= 100) {
    parts << tr("reached");
  }
  if (!summary && t.value(QStringLiteral("mode")).toString() == QLatin1String("manual")) parts << tr("manual dates");
  const QDate deadline = isoDate(t.value(QStringLiteral("deadline")).toString());
  if (deadline.isValid()) {
    const QDate due = f.isValid() ? f : s;
    parts << (due.isValid() && due > deadline ? tr("deadline %1, missed").arg(dayMonth(deadline))
                                              : tr("deadline %1").arg(dayMonth(deadline)));
  }
  return parts.join(QStringLiteral(", "));
}

QString GanttChart::idAt(const QPoint& p) const {
  for (const Bar& b : visibleBars()) {
    if (b.row.contains(p)) return b.id;
  }
  return {};
}

QRectF GanttChart::todayLabel(const QRect& area) const {
  if (today_ < first_ || today_ > last_) return {};
  const QFontMetrics fm(font());
  const QString label = tr("Today");
  const double x = xOf(today_) + pxPerDay() / 2.0;
  const int mid = area.top() + area.height() / 2;
  const double w = fm.horizontalAdvance(label) + 8.0;
  const double h = std::min<double>(fm.height() + 2, area.bottom() - mid - 2);
  return {x - w / 2.0, mid + (area.bottom() - mid - h) / 2.0, w, h};
}

void GanttChart::paintHeader(QPainter& p, const QRect& area) const {
  const QRectF today = todayLabel(area);
  p.fillRect(area, c_.panelBg);
  const QDate from = dateAt(0);
  const QDate to = dateAt(width());
  if (!from.isValid() || !to.isValid()) return;
  const int mid = area.top() + area.height() / 2;
  QFont small = font();
  p.setFont(small);
  const QFontMetrics fm(small);
  p.setPen(c_.border);
  p.drawLine(area.left(), mid, area.right(), mid);
  p.drawLine(area.left(), area.bottom(), area.right(), area.bottom());
  // Upper band: months (Day, Week) or years (Month, Quarter).
  const bool years = scale_ == Scale::Month || scale_ == Scale::Quarter;
  QDate d = years ? QDate(from.year(), 1, 1) : QDate(from.year(), from.month(), 1);
  while (d <= to) {
    const QDate next = years ? d.addYears(1) : d.addMonths(1);
    const int x0 = static_cast<int>(xOf(d));
    const int x1 = static_cast<int>(xOf(next));
    p.setPen(c_.border);
    p.drawLine(x0, area.top(), x0, mid);
    const QString label = years ? QString::number(d.year())
                                : QStringLiteral("%1 %2").arg(QLocale().monthName(d.month(), QLocale::LongFormat)).arg(d.year());
    p.setPen(c_.fg);
    const QRect r(std::max(x0, 0) + 4, area.top(), std::max(0, x1 - std::max(x0, 0) - 6), mid - area.top());
    // A sliver of a month at the edge gets no label rather than "A…".
    if (r.width() >= fm.horizontalAdvance(label.left(3)) + fm.horizontalAdvance(QChar(0x2026)))
      p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(label, Qt::ElideRight, r.width()));
    d = next;
  }
  // Lower band: days, week starts, months or quarters.
  QDate u;
  switch (scale_) {
    case Scale::Day: u = from; break;
    case Scale::Week: u = from.addDays(1 - from.dayOfWeek()); break;
    case Scale::Month: u = QDate(from.year(), from.month(), 1); break;
    case Scale::Quarter: u = QDate(from.year(), ((from.month() - 1) / 3) * 3 + 1, 1); break;
  }
  while (u <= to) {
    QDate next;
    QString label;
    switch (scale_) {
      case Scale::Day:
        next = u.addDays(1);
        label = QString::number(u.day());
        break;
      case Scale::Week:
        next = u.addDays(7);
        label = dayMonth(u);
        break;
      case Scale::Month:
        next = u.addMonths(1);
        label = QLocale().monthName(u.month(), QLocale::ShortFormat);
        break;
      case Scale::Quarter:
        next = u.addMonths(3);
        label = QStringLiteral("Q%1").arg((u.month() - 1) / 3 + 1);
        break;
    }
    const int x0 = static_cast<int>(xOf(u));
    const int x1 = static_cast<int>(xOf(next));
    p.setPen(c_.border);
    p.drawLine(x0, mid, x0, area.bottom());
    p.setPen(scale_ == Scale::Day && u.dayOfWeek() >= 6 ? c_.muted : c_.fg);
    const QRect r(x0, mid, x1 - x0, area.bottom() - mid);
    const QRect text = fm.boundingRect(r, Qt::AlignCenter, label);
    if (fm.horizontalAdvance(label) <= r.width() - 2 && !today.intersects(QRectF(text))) {
      p.drawText(r, Qt::AlignCenter, label);
    }
    u = next;
  }
}

void GanttChart::paintBar(QPainter& p, const Bar& b) const {
  if (b.rect.isNull()) return;
  const QJsonObject t = model_->task(b.id);
  const double pct = std::clamp(t.value(QStringLiteral("percentComplete")).toDouble(), 0.0, 100.0);
  const QRectF r(b.rect);
  if (b.milestone) {
    QPainterPath diamond;
    diamond.moveTo(r.center().x(), r.top());
    diamond.lineTo(r.right(), r.center().y());
    diamond.lineTo(r.center().x(), r.bottom());
    diamond.lineTo(r.left(), r.center().y());
    diamond.closeSubpath();
    p.setPen(QPen(c_.fg, 1.2));
    p.setBrush(pct >= 100 ? c_.fg : c_.accent);
    p.drawPath(diamond);
  } else if (b.summary) {
    // A bracket: a bar with a point down at each end.
    p.setPen(Qt::NoPen);
    p.setBrush(c_.fg);
    const double tip = std::min(r.height(), r.width() / 2.0);
    QPainterPath path;
    path.moveTo(r.left(), r.top());
    path.lineTo(r.right(), r.top());
    path.lineTo(r.right(), r.bottom() + tip * 0.6);
    path.lineTo(r.right() - tip, r.bottom());
    path.lineTo(r.left() + tip, r.bottom());
    path.lineTo(r.left(), r.bottom() + tip * 0.6);
    path.closeSubpath();
    p.drawPath(path);
  } else {
    if (b.manual) {
      // Manual dates: outlined.
      QColor fill = c_.accent;
      fill.setAlphaF(0.18);
      p.setBrush(fill);
      p.setPen(QPen(c_.accent, 1.6));
    } else {
      p.setBrush(c_.accent);
      p.setPen(QPen(c_.accent.darker(130), 1.0));
    }
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 2.5, 2.5);
    if (pct > 0) {
      // Progress: a centre band, so it reads without colour.
      const QRectF band(r.left() + 1.5, r.center().y() - r.height() / 6.0, (r.width() - 3.0) * pct / 100.0, r.height() / 3.0);
      p.setPen(Qt::NoPen);
      p.setBrush(b.manual ? c_.accent : c_.fg);
      p.drawRect(band);
    }
  }
  // Deadline flag: filled when the finish is past it, outlined otherwise.
  const QDate deadline = isoDate(t.value(QStringLiteral("deadline")).toString());
  if (deadline.isValid()) {
    const double x = xOf(deadline) + pxPerDay() / 2.0;
    const QRect row = b.row;
    const double top = row.top() + 3.0;
    const double bottom = row.bottom() - 3.0;
    const QColor c = b.late ? kLate : c_.fg;
    p.setPen(QPen(c, 1.4));
    p.drawLine(QPointF(x, top), QPointF(x, bottom));
    QPainterPath flag;
    flag.moveTo(x, top);
    flag.lineTo(x + 8.0, top + 3.5);
    flag.lineTo(x, top + 7.0);
    flag.closeSubpath();
    p.setBrush(b.late ? QBrush(c) : QBrush(Qt::NoBrush));
    p.drawPath(flag);
  }
  // The title to the right of leaf bars and milestones.
  if (!b.summary) {
    p.setPen(c_.muted);
    const QString title = t.value(QStringLiteral("title")).toString();
    int x = b.rect.right() + 6;
    if (deadline.isValid()) {
      // Past the flag when it would sit under the text.
      const int flag = static_cast<int>(xOf(deadline) + pxPerDay() / 2.0);
      if (flag >= b.rect.right() - 2 && flag <= x + p.fontMetrics().horizontalAdvance(title)) x = std::max(x, flag + 12);
    }
    p.drawText(QRect(x, b.row.top(), 400, b.row.height()), Qt::AlignVCenter | Qt::AlignLeft, title);
  }
}

void GanttChart::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const int top = rowsTop();
  const int bottom = height() - hbar_->height();
  const QRect rows(0, top, rowsWidth(), bottom - top);
  p.fillRect(rect(), c_.inputBg);
  const double ppd = pxPerDay();
  const QDate from = dateAt(0);
  const QDate to = dateAt(width());
  // Weekends (no project calendars yet).
  if (ppd >= 10.0 && from.isValid()) {  // Day and week scales
    QColor shade = c_.muted;
    shade.setAlphaF(c_.id == QLatin1String("high-contrast") ? 0.22 : 0.10);
    for (QDate d = from; d <= to; d = d.addDays(1)) {
      if (d.dayOfWeek() < 6) continue;
      const double x = xOf(d);
      p.fillRect(QRectF(x, top, ppd, rows.height()), shade);
    }
  }
  int painted = 0;
  const QList<Bar> bars = layoutRows(&painted);
  lastPainted_ = painted;
  const QString current = model_->idAt(table_->currentIndex());
  p.save();
  p.setClipRect(rows);
  for (const Bar& b : bars) {
    if (b.id == current) {
      QColor sel = c_.selBg;
      sel.setAlphaF(0.25);
      p.fillRect(b.row, sel);
    }
    p.setPen(QPen(c_.border, 0.5));
    p.drawLine(b.row.bottomLeft(), b.row.bottomRight());
  }
  for (const Bar& b : bars) paintBar(p, b);
  p.restore();
  p.save();
  p.setClipRect(QRect(0, 0, rowsWidth(), top));
  paintHeader(p, QRect(0, 0, rowsWidth(), top));
  p.restore();
  // Today: a dashed line through the rows and a label in the header.
  if (today_ >= first_ && today_ <= last_) {
    const double x = xOf(today_) + ppd / 2.0;
    QPen pen(kLate, 1.6, Qt::DashLine);
    p.setPen(pen);
    p.drawLine(QPointF(x, top), QPointF(x, bottom));
    const QRectF box = todayLabel(QRect(0, 0, rowsWidth(), top));
    p.setPen(Qt::NoPen);
    p.setBrush(kLate);
    p.drawRoundedRect(box, 3, 3);
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, tr("Today"));
  }
}

void GanttChart::mousePressEvent(QMouseEvent* event) {
  const QString id = idAt(event->position().toPoint());
  if (!id.isEmpty()) emit taskClicked(id);
  QWidget::mousePressEvent(event);
}

void GanttChart::mouseDoubleClickEvent(QMouseEvent* event) {
  const QString id = idAt(event->position().toPoint());
  if (!id.isEmpty()) emit taskActivated(id);
}

void GanttChart::wheelEvent(QWheelEvent* event) {
  const QPoint d = event->angleDelta();
  if (event->modifiers() & Qt::ControlModifier) {
    if (d.y() > 0) zoomIn();
    else if (d.y() < 0) zoomOut();
    event->accept();
    return;
  }
  if (d.x() != 0 || (event->modifiers() & Qt::ShiftModifier)) {
    const int delta = d.x() != 0 ? d.x() : d.y();
    hbar_->setValue(hbar_->value() - delta);
  } else {
    QScrollBar* v = table_->verticalScrollBar();
    v->setValue(v->value() - d.y() / 2);
  }
  event->accept();
}

bool GanttChart::event(QEvent* event) {
  if (event->type() == QEvent::ToolTip) {
    auto* help = static_cast<QHelpEvent*>(event);
    const QString id = idAt(help->pos());
    if (id.isEmpty()) {
      QToolTip::hideText();
    } else {
      QToolTip::showText(help->globalPos(), barText(id), this);
    }
    return true;
  }
  return QWidget::event(event);
}

// ---- GanttView ----

GanttView::GanttView(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("ganttView"));
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(12, 10, 12, 8);
  v->setSpacing(6);
  auto* head = new QHBoxLayout;
  title_ = new QLabel(this);
  title_->setObjectName(QStringLiteral("ganttTitle"));
  QFont tf = title_->font();
  tf.setBold(true);
  title_->setFont(tf);
  head->addWidget(title_);
  note_ = new QLabel(this);
  note_->setObjectName(QStringLiteral("ganttNote"));
  note_->setForegroundRole(QPalette::PlaceholderText);
  head->addWidget(note_, 1);
  auto* scaleLabel = new QLabel(tr("&Timescale:"), this);
  head->addWidget(scaleLabel);
  scaleBox_ = new QComboBox(this);
  scaleBox_->setObjectName(QStringLiteral("ganttScale"));
  scaleBox_->setAccessibleName(tr("Timescale"));
  for (GanttChart::Scale s : {GanttChart::Scale::Day, GanttChart::Scale::Week, GanttChart::Scale::Month,
                              GanttChart::Scale::Quarter}) {
    scaleBox_->addItem(GanttChart::scaleName(s), static_cast<int>(s));
  }
  scaleLabel->setBuddy(scaleBox_);
  head->addWidget(scaleBox_);
  todayButton_ = new QToolButton(this);
  todayButton_->setObjectName(QStringLiteral("ganttToday"));
  todayButton_->setText(tr("Today"));
  todayButton_->setToolTip(tr("Scroll to today (T)"));
  head->addWidget(todayButton_);
  v->addLayout(head);

  empty_ = new QLabel(tr("This project has no tasks yet. Add them in the outline (View → Outline)."), this);
  empty_->setObjectName(QStringLiteral("ganttEmpty"));
  empty_->setForegroundRole(QPalette::PlaceholderText);
  empty_->hide();
  v->addWidget(empty_);

  model_ = new OutlineModel(this);
  model_->setEditable(false);
  split_ = new QSplitter(Qt::Horizontal, this);
  split_->setObjectName(QStringLiteral("ganttSplit"));
  split_->setChildrenCollapsible(false);
  table_ = new QTreeView(split_);
  table_->setObjectName(QStringLiteral("ganttTable"));
  table_->setAccessibleName(tr("Gantt tasks"));
  table_->setModel(model_);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setUniformRowHeights(true);
  table_->setAllColumnsShowFocus(true);
  table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  table_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // the timeline scrolls it
  // Always on, so the table's last row lines up with the timeline's (whose
  // own horizontal bar sits at the same height).
  table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  // WBS, title, duration, start, finish, % complete (moved before any
  // column is hidden: Qt 6.4 places hidden sections wrongly otherwise).
  QHeaderView* hh = table_->header();
  hh->moveSection(hh->visualIndex(M::Percent), hh->count() - 1);
  for (int col : {static_cast<int>(M::Type), static_cast<int>(M::Status), static_cast<int>(M::Deadline),
                  static_cast<int>(M::Priority)}) {
    table_->setColumnHidden(col, true);
  }
  auto* unscheduled = new UnscheduledDelegate(table_);
  table_->setItemDelegateForColumn(M::Start, unscheduled);
  table_->header()->setStretchLastSection(false);
  // Two lines tall, like the timescale beside it (months over weeks).
  hh->setMinimumHeight(2 * QFontMetrics(hh->font()).height() + 8);
  hh->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  table_->installEventFilter(this);
  chart_ = new GanttChart(table_, model_, split_);
  chart_->installEventFilter(this);
  split_->addWidget(table_);
  split_->addWidget(chart_);
  // Once the user drags the handle, the split is theirs (for this project).
  connect(split_, &QSplitter::splitterMoved, this, [this] { userSized_ = true; });
  split_->setStretchFactor(0, 0);
  split_->setStretchFactor(1, 1);
  split_->setSizes({430, 600});
  v->addWidget(split_, 1);

  connect(scaleBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
    chart_->setScale(static_cast<GanttChart::Scale>(scaleBox_->itemData(i).toInt()));
  });
  syncScaleBox();
  connect(todayButton_, &QToolButton::clicked, this, [this] { chart_->scrollToToday(); });
  connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& now) {
    chart_->update();
    emit currentTaskChanged(model_->idAt(now));
  });
  connect(table_, &QTreeView::activated, this, [this](const QModelIndex& i) { emit openTask(model_->idAt(i)); });
  connect(chart_, &GanttChart::taskClicked, this, [this](const QString& id) {
    selectTask(id);
    table_->setFocus(Qt::MouseFocusReason);
  });
  connect(chart_, &GanttChart::taskActivated, this, [this](const QString& id) {
    selectTask(id);
    emit openTask(id);
  });
  connect(table_, &QTreeView::collapsed, this, [this](const QModelIndex& i) { collapsed_.insert(model_->idAt(i)); });
  connect(table_, &QTreeView::expanded, this, [this](const QModelIndex& i) { collapsed_.remove(model_->idAt(i)); });
}

void GanttView::setProject(const QString& id, const QString& name) {
  if (id != projectId_) {
    projectId_ = id;
    collapsed_.clear();
    firstLoad_ = true;
  }
  title_->setText(tr("%1 — Gantt").arg(name));
}

void GanttView::reload(const QList<QJsonObject>& tasks, const QJsonArray& wbs) {
  const QString current = currentTaskId();
  model_->setTasks(tasks, wbs);
  // Qt 6.4 keeps a hidden section's old width in the layout after a model
  // reset; hiding the columns again puts the shown ones back together.
  for (int col : {static_cast<int>(M::Type), static_cast<int>(M::Status), static_cast<int>(M::Deadline),
                  static_cast<int>(M::Priority)}) {
    table_->setColumnHidden(col, false);
    table_->setColumnHidden(col, true);
  }
  table_->expandAll();
  for (const QString& id : std::as_const(collapsed_)) {
    if (model_->contains(id)) table_->collapse(model_->indexOf(id, 0));
  }
  for (int col = 0; col < M::ColumnCount; ++col) {
    if (!table_->isColumnHidden(col)) table_->resizeColumnToContents(col);
  }
  table_->setColumnWidth(M::Title, std::clamp(table_->columnWidth(M::Title), 120, 240));
  unscheduled_ = 0;
  for (const QString& id : model_->depthFirstIds()) {
    if (model_->indexOf(id, M::Start).data().toString().isEmpty() &&
        model_->indexOf(id, M::Finish).data().toString().isEmpty()) {
      ++unscheduled_;
    }
  }
  note_->setText(unscheduled_ == 0 ? tr("Read-only; bars show the stored dates.")
                                   : unscheduled_ == 1 ? tr("Read-only; 1 task has no dates yet.")
                                                       : tr("Read-only; %1 tasks have no dates yet.").arg(unscheduled_));
  empty_->setVisible(model_->taskCount() == 0);
  split_->setVisible(model_->taskCount() > 0);
  chart_->updateRange();
  if (!current.isEmpty() && model_->contains(current)) {
    selectTask(current);
  } else if (model_->rowCount() > 0) {
    table_->setCurrentIndex(model_->index(0, M::Title));
  }
  if (firstLoad_) {
    firstLoad_ = false;
    userSized_ = false;
    fitTable();
    chart_->scrollToToday();
  }
}

// The table as wide as its columns (up to half), the timeline the rest;
// again whenever the view's width changes, until the user drags the handle.
void GanttView::fitTable() {
  const int total = split_->width();
  if (userSized_ || total < 400 || !isVisible()) return;
  const int want = std::min(table_->header()->length() + 2 * table_->frameWidth() + 2, total / 2);
  split_->setSizes({want, total - want});
}

void GanttView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  fitTable();
}

void GanttView::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  fitTable();
}

void GanttView::setColors(const ThemeColors& c) { chart_->setColors(c); }

void GanttView::setToday(const QDate& today) { chart_->setToday(today); }

QString GanttView::currentTaskId() const { return model_->idAt(table_->currentIndex()); }

bool GanttView::selectTask(const QString& id) {
  const QModelIndex i = model_->indexOf(id, M::Title);
  if (!i.isValid()) return false;
  for (QModelIndex p = i.parent(); p.isValid(); p = p.parent()) table_->expand(p);
  table_->setCurrentIndex(i);
  table_->scrollTo(i);
  return true;
}

void GanttView::syncScaleBox() {
  const QSignalBlocker block(scaleBox_);
  scaleBox_->setCurrentIndex(scaleBox_->findData(static_cast<int>(chart_->scale())));
}

bool GanttView::handleKey(QKeyEvent* e) {
  const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
  const int k = e->key();
  if (mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
    if (k == Qt::Key_Equal || k == Qt::Key_Plus) {
      chart_->zoomIn();
      syncScaleBox();
      return true;
    }
    if (k == Qt::Key_Minus || k == Qt::Key_Underscore) {
      chart_->zoomOut();
      syncScaleBox();
      return true;
    }
  }
  if (mods == Qt::NoModifier && k == Qt::Key_T) {
    chart_->scrollToToday();
    return true;
  }
  return false;
}

bool GanttView::eventFilter(QObject* watched, QEvent* event) {
  if (watched == table_ || watched == chart_) {
    if (event->type() == QEvent::ShortcutOverride) {
      auto* e = static_cast<QKeyEvent*>(event);
      const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
      // Ctrl+Shift+= / − are ours here, not the window's text zoom.
      if (mods == (Qt::ControlModifier | Qt::ShiftModifier) &&
          (e->key() == Qt::Key_Equal || e->key() == Qt::Key_Plus || e->key() == Qt::Key_Minus ||
           e->key() == Qt::Key_Underscore)) {
        e->accept();
        return true;
      }
    } else if (event->type() == QEvent::KeyPress) {
      if (handleKey(static_cast<QKeyEvent*>(event))) return true;
    } else if (event->type() == QEvent::Wheel && watched == chart_) {
      // Ctrl+wheel changes the scale; keep the box in step.
      auto* w = static_cast<QWheelEvent*>(event);
      if (w->modifiers() & Qt::ControlModifier) {
        if (w->angleDelta().y() > 0) chart_->zoomIn();
        else if (w->angleDelta().y() < 0) chart_->zoomOut();
        syncScaleBox();
        return true;
      }
    }
  }
  return QWidget::eventFilter(watched, event);
}

}  // namespace zproject::app
