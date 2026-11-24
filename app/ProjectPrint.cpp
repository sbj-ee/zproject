#include "app/ProjectPrint.hpp"

#include <QFontMetricsF>
#include <QPagedPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace zproject::app {
namespace {

// Sizes in points.
constexpr double kHeaderHeight = 40;
constexpr double kNoteHeight = 13;
constexpr double kFooterHeight = 16;
constexpr double kColumnHeaderHeight = 24;
constexpr double kRowHeight = 14;
constexpr double kPad = 3;
constexpr double kIndent = 9;
constexpr double kMinGantt = 160;

// Fixed print colors: paper is white whatever the screen theme.
const QColor kText(0x1a, 0x1a, 0x1a);
const QColor kMuted(0x5c, 0x5c, 0x5c);
const QColor kRule(0xb4, 0xb4, 0xb4);
const QColor kGrid(0xe0, 0xe0, 0xe0);
const QColor kHeadFill(0xe9, 0xe9, 0xe9);
const QColor kStripe(0xf5, 0xf6, 0xf8);
const QColor kWeekend(0xee, 0xee, 0xee);
const QColor kBar(0xa9, 0xc4, 0xe8);
const QColor kBarEdge(0x2f, 0x5d, 0x9e);
const QColor kLate(0xc6, 0x28, 0x28);
const QColor kNote(0x9a, 0x5b, 0x00);

QFont font(int px, bool bold = false, bool italic = false) {
  QFont f;
  f.setPixelSize(px);  // pixels of the point-scaled painter = points
  f.setBold(bold);
  f.setItalic(italic);
  return f;
}

QDate date(const QString& s) {
  return isValidDate(s) ? QDate::fromString(s, Qt::ISODate) : QDate();
}

QString number(double d) {
  if (d == std::floor(d)) return QString::number(static_cast<qint64>(d));
  return QString::number(d, 'g', 6);
}

// The first day of the unit containing d.
QDate unitStart(const QDate& d, ProjectPrintout::Unit u) {
  switch (u) {
    case ProjectPrintout::Unit::Day: return d;
    case ProjectPrintout::Unit::Week: return d.addDays(1 - d.dayOfWeek());
    case ProjectPrintout::Unit::Month: return QDate(d.year(), d.month(), 1);
    case ProjectPrintout::Unit::Quarter: return QDate(d.year(), ((d.month() - 1) / 3) * 3 + 1, 1);
  }
  return d;
}

QDate nextUnit(const QDate& d, ProjectPrintout::Unit u) {
  switch (u) {
    case ProjectPrintout::Unit::Day: return d.addDays(1);
    case ProjectPrintout::Unit::Week: return d.addDays(7);
    case ProjectPrintout::Unit::Month: return d.addMonths(1);
    case ProjectPrintout::Unit::Quarter: return d.addMonths(3);
  }
  return d.addDays(1);
}

}  // namespace

ProjectPrintout::ProjectPrintout(const ExportData& data, PrintSettings settings)
    : data_(data), settings_(std::move(settings)), rows_(exportRows(data)) {}

QString ProjectPrintout::cell(const ExportRow& r, int column) const {
  const Task& t = r.task;
  switch (column) {
    case 0: return r.wbs;
    case 1: return t.title;
    case 2: return r.summary ? QString() : number(t.duration) + QStringLiteral(" d");
    case 3: return r.start;
    case 4: return r.finish;
    case 5: return QString::number(t.percentComplete) + QLatin1Char('%');
    case 6: return r.predecessors;
    default: return {};
  }
}

void ProjectPrintout::layout(const QSizeF& page) {
  page_ = page;
  // Headings and summary rows are bold: measure them that way.
  const QFontMetricsF fm(font(8));
  const QFontMetricsF bold(font(8, true));
  double wbs = bold.horizontalAdvance(QStringLiteral("WBS"));
  double title = 0;
  double preds = bold.horizontalAdvance(QStringLiteral("Predecessors"));
  QDate lo;
  QDate hi;
  auto widen = [&](const QDate& d) {
    if (!d.isValid()) return;
    if (!lo.isValid() || d < lo) lo = d;
    if (!hi.isValid() || d > hi) hi = d;
  };
  for (const ExportRow& r : rows_) {
    const QFontMetricsF& m = r.summary ? bold : fm;
    wbs = std::max(wbs, m.horizontalAdvance(r.wbs));
    title = std::max(title, (r.level - 1) * kIndent + m.horizontalAdvance(r.task.title));
    preds = std::max(preds, m.horizontalAdvance(r.predecessors));
    widen(date(r.start));
    widen(date(r.finish));
    widen(date(r.task.deadline));
  }
  const double dateW = bold.horizontalAdvance(QStringLiteral("0000-00-00")) + 2 * kPad;
  columns_ = {{QStringLiteral("WBS"), wbs + 2 * kPad, false},
              {QStringLiteral("Task"), std::clamp(title + 2 * kPad, 120.0, 230.0), false},
              {QStringLiteral("Duration"), bold.horizontalAdvance(QStringLiteral("Duration")) + 2 * kPad, true},
              {QStringLiteral("Start"), dateW, false},
              {QStringLiteral("Finish"), dateW, false},
              {QStringLiteral("%"), bold.horizontalAdvance(QStringLiteral("100%")) + 2 * kPad, true},
              {QStringLiteral("Predecessors"), std::clamp(preds + 2 * kPad, 50.0, 110.0), false}};
  tableWidth_ = 0;
  for (const Column& c : columns_) tableWidth_ += c.width;
  ganttWidth_ = lo.isValid() ? page.width() - tableWidth_ : 0;
  if (ganttWidth_ < kMinGantt) {
    // No room or nothing dated: the task column takes the width.
    ganttWidth_ = 0;
    const double others = tableWidth_ - columns_[1].width;
    columns_[1].width = std::max(80.0, page.width() - others);
    tableWidth_ = others + columns_[1].width;
  }
  ganttLeft_ = tableWidth_;

  if (ganttWidth_ > 0) {
    lo = lo.addDays(-1);
    hi = hi.addDays(1);
    const double perDay = ganttWidth_ / static_cast<double>(lo.daysTo(hi) + 1);
    unit_ = perDay >= 14 ? Unit::Day : perDay * 7 >= 30 ? Unit::Week : perDay * 30 >= 26 ? Unit::Month : Unit::Quarter;
    from_ = unitStart(lo, unit_);
    to_ = nextUnit(unitStart(hi, unit_), unit_).addDays(-1);
    perDay_ = ganttWidth_ / static_cast<double>(from_.daysTo(to_) + 1);
  }

  bodyTop_ = kHeaderHeight + (settings_.note.isEmpty() ? 0 : kNoteHeight) + kColumnHeaderHeight;
  const int perPage = std::max(1, static_cast<int>((page.height() - bodyTop_ - kFooterHeight) / kRowHeight));
  pages_.clear();
  for (int i = 0; i < rows_.size(); i += perPage) {
    QList<int> onPage;
    for (int k = i; k < std::min<int>(static_cast<int>(rows_.size()), i + perPage); ++k) onPage.append(k);
    pages_.append(onPage);
  }
  if (pages_.isEmpty()) pages_.append(QList<int>());
}

double ProjectPrintout::xOf(const QDate& d) const {
  return ganttLeft_ + static_cast<double>(from_.daysTo(d)) * perDay_;
}

void ProjectPrintout::paintHeader(QPainter& p, int page) const {
  const Project& pr = data_.project;
  const double w = page_.width();
  p.setPen(kMuted);
  p.setFont(font(8));
  const QFontMetricsF small(font(8));
  const double subW = small.horizontalAdvance(settings_.subtitle) + 2;
  p.drawText(QRectF(w - subW, 0, subW, 16), Qt::AlignRight | Qt::AlignVCenter, settings_.subtitle);
  p.setPen(kText);
  p.setFont(font(14, true));
  const QFontMetricsF big(font(14, true));
  p.drawText(QRectF(0, 0, w - subW - 8, 18), Qt::AlignLeft | Qt::AlignVCenter,
             big.elidedText(pr.name, Qt::ElideRight, w - subW - 8));

  QDate start = date(pr.start);
  QDate finish = date(pr.finish);
  for (const ExportRow& r : rows_) {
    const QDate s = date(r.start);
    const QDate f = date(r.finish);
    if (s.isValid() && (!start.isValid() || s < start)) start = s;
    if (f.isValid() && (!finish.isValid() || f > finish)) finish = f;
  }
  QStringList facts;
  if (start.isValid()) facts << QStringLiteral("Start %1").arg(start.toString(Qt::ISODate));
  if (finish.isValid()) facts << QStringLiteral("Finish %1").arg(finish.toString(Qt::ISODate));
  if (!pr.owner.isEmpty()) facts << QStringLiteral("Owner %1").arg(pr.owner);
  facts << QStringLiteral("Status %1").arg(QString(pr.status).replace(QLatin1Char('_'), QLatin1Char(' ')));
  facts << QStringLiteral("%1 tasks").arg(rows_.size());
  p.setFont(font(9));
  p.setPen(kMuted);
  p.drawText(QRectF(0, 20, w, 14), Qt::AlignLeft | Qt::AlignVCenter,
             QFontMetricsF(font(9)).elidedText(facts.join(QStringLiteral("  ·  ")), Qt::ElideRight, w));
  double y = kHeaderHeight;
  if (!settings_.note.isEmpty()) {
    p.setFont(font(8, false, true));
    p.setPen(kNote);
    p.drawText(QRectF(0, y - 4, w, kNoteHeight), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetricsF(font(8, false, true)).elidedText(settings_.note, Qt::ElideRight, w));
    y += kNoteHeight;
  }
  p.setPen(QPen(kRule, 0.75));
  p.drawLine(QPointF(0, y - 3), QPointF(w, y - 3));

  // Footer.
  const double fy = page_.height() - kFooterHeight + 4;
  p.setFont(font(7));
  p.setPen(kMuted);
  p.drawText(QRectF(0, fy, w / 2, kFooterHeight - 4), Qt::AlignLeft | Qt::AlignVCenter, settings_.footer);
  p.drawText(QRectF(w / 2, fy, w / 2, kFooterHeight - 4), Qt::AlignRight | Qt::AlignVCenter,
             QStringLiteral("Page %1 of %2").arg(page + 1).arg(pageCount()));
}

void ProjectPrintout::paintColumnHeader(QPainter& p) const {
  const double top = bodyTop_ - kColumnHeaderHeight;
  p.fillRect(QRectF(0, top, page_.width(), kColumnHeaderHeight), kHeadFill);
  p.setFont(font(8, true));
  p.setPen(kText);
  double x = 0;
  for (const Column& c : columns_) {
    p.drawText(QRectF(x + kPad, top + kColumnHeaderHeight / 2, c.width - 2 * kPad, kColumnHeaderHeight / 2),
               (c.right ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
               QFontMetricsF(font(8, true)).elidedText(c.title, Qt::ElideRight, c.width - 2 * kPad));
    x += c.width;
  }
  if (ganttWidth_ <= 0) return;

  // Two bands: the group above (month or year), the ticks below.
  p.save();
  p.setClipRect(QRectF(ganttLeft_, top, ganttWidth_, kColumnHeaderHeight));
  const double mid = top + kColumnHeaderHeight / 2;
  // Months over day and week ticks; years over month and quarter ticks.
  const bool byMonth = unit_ == Unit::Day || unit_ == Unit::Week;
  p.setPen(QPen(kRule, 0.5));
  p.drawLine(QPointF(ganttLeft_, mid), QPointF(ganttLeft_ + ganttWidth_, mid));
  const QFontMetricsF fm(font(7, true));
  p.setFont(font(7, true));
  for (QDate g = byMonth ? QDate(from_.year(), from_.month(), 1) : QDate(from_.year(), 1, 1); g <= to_;
       g = byMonth ? g.addMonths(1) : g.addYears(1)) {
    const QDate next = byMonth ? g.addMonths(1) : g.addYears(1);
    const double x0 = std::max(ganttLeft_, xOf(g));
    const double x1 = std::min(ganttLeft_ + ganttWidth_, xOf(next));
    QString label = byMonth ? g.toString(QStringLiteral("MMMM yyyy")) : QString::number(g.year());
    if (byMonth && fm.horizontalAdvance(label) > x1 - x0 - 4) label = g.toString(QStringLiteral("MMM yy"));
    p.setPen(QPen(kRule, 0.5));
    p.drawLine(QPointF(x0, top), QPointF(x0, mid));
    if (fm.horizontalAdvance(label) <= x1 - x0 - 4) {
      p.setPen(kText);
      p.drawText(QRectF(x0 + 2, top, x1 - x0 - 4, kColumnHeaderHeight / 2), Qt::AlignLeft | Qt::AlignVCenter, label);
    }
  }
  p.setFont(font(7));
  const QFontMetricsF tick(font(7));
  for (QDate d = from_; d <= to_; d = nextUnit(d, unit_)) {
    const double x0 = xOf(d);
    const double x1 = xOf(nextUnit(d, unit_));
    QString label;
    switch (unit_) {
      case Unit::Day: label = QString::number(d.day()); break;
      case Unit::Week: label = d.toString(QStringLiteral("d MMM")); break;
      case Unit::Month: label = d.toString(QStringLiteral("MMM")); break;
      case Unit::Quarter: label = QStringLiteral("Q%1").arg((d.month() - 1) / 3 + 1); break;
    }
    p.setPen(QPen(kRule, 0.5));
    p.drawLine(QPointF(x0, mid), QPointF(x0, top + kColumnHeaderHeight));
    if (tick.horizontalAdvance(label) <= x1 - x0 - 2) {
      p.setPen(kText);
      p.drawText(QRectF(x0 + 1, mid, x1 - x0 - 2, kColumnHeaderHeight / 2), Qt::AlignLeft | Qt::AlignVCenter, label);
    }
  }
  p.restore();
}

void ProjectPrintout::paintTimeline(QPainter& p, double top, double bottom) const {
  if (ganttWidth_ <= 0) return;
  p.save();
  p.setClipRect(QRectF(ganttLeft_, top, ganttWidth_, bottom - top));
  if ((unit_ == Unit::Day || unit_ == Unit::Week) && perDay_ >= 3) {
    for (QDate d = from_; d <= to_; d = d.addDays(1)) {
      if (d.dayOfWeek() >= 6) p.fillRect(QRectF(xOf(d), top, perDay_, bottom - top), kWeekend);
    }
  }
  p.setPen(QPen(kGrid, 0.5));
  for (QDate d = from_; d <= to_; d = nextUnit(d, unit_)) p.drawLine(QPointF(xOf(d), top), QPointF(xOf(d), bottom));
  p.restore();
}

void ProjectPrintout::paintRow(QPainter& p, const ExportRow& r, double y, bool shaded) const {
  if (shaded) p.fillRect(QRectF(0, y, tableWidth_, kRowHeight), kStripe);
  const QFont body = font(8, r.summary);
  const QFontMetricsF fm(body);
  p.setFont(body);
  p.setPen(kText);
  double x = 0;
  for (int c = 0; c < columns_.size(); ++c) {
    const Column& col = columns_[c];
    const double indent = c == 1 ? (r.level - 1) * kIndent : 0;
    const double w = col.width - 2 * kPad - indent;
    if (w > 4) {
      p.drawText(QRectF(x + kPad + indent, y, w, kRowHeight), (col.right ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                 fm.elidedText(cell(r, c), Qt::ElideRight, w));
    }
    x += col.width;
  }
  if (ganttWidth_ > 0) paintBar(p, r, y);
}

void ProjectPrintout::paintBar(QPainter& p, const ExportRow& r, double y) const {
  const Task& t = r.task;
  QDate s = date(r.start);
  QDate f = date(r.finish);
  const bool milestone = !r.summary && t.type == QLatin1String("milestone");
  if (milestone) s = f = f.isValid() ? f : s;
  if (!s.isValid() && f.isValid()) s = f;
  if (s.isValid() && !f.isValid()) f = s;

  p.save();
  p.setClipRect(QRectF(ganttLeft_, y, ganttWidth_, kRowHeight));
  p.setRenderHint(QPainter::Antialiasing, true);
  const double mid = y + kRowHeight / 2;
  if (s.isValid()) {
    const double x0 = xOf(s);
    const double x1 = std::max(xOf(f.addDays(1)), x0 + 1.5);
    if (milestone) {
      const double cx = xOf(s) + perDay_ / 2;
      const double h = 4.5;
      QPolygonF diamond{{cx, mid - h}, {cx + h, mid}, {cx, mid + h}, {cx - h, mid}};
      p.setPen(QPen(kText, 0.75));
      p.setBrush(t.percentComplete >= 100 ? QBrush(Qt::white) : QBrush(kText));
      p.drawPolygon(diamond);
    } else if (r.summary) {
      const double h = 2.5;
      p.setPen(Qt::NoPen);
      p.setBrush(kText);
      p.drawRect(QRectF(x0, mid - h, x1 - x0, h));
      p.drawPolygon(QPolygonF{{x0, mid}, {x0 + 4, mid}, {x0, mid + 4}});
      p.drawPolygon(QPolygonF{{x1, mid}, {x1 - 4, mid}, {x1, mid + 4}});
    } else {
      const double h = kRowHeight * 0.55;
      const QRectF bar(x0, mid - h / 2, x1 - x0, h);
      const bool manual = t.mode == QLatin1String("manual");
      p.setPen(QPen(kBarEdge, 0.75));
      p.setBrush(manual ? QBrush(Qt::white) : QBrush(kBar));
      p.drawRect(bar);
      if (t.percentComplete > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(kBarEdge);
        const double pw = bar.width() * std::clamp(t.percentComplete, 0, 100) / 100.0;
        p.drawRect(QRectF(bar.left(), mid - h / 6, pw, h / 3));
      }
    }
  }
  const QDate deadline = date(t.deadline);
  if (deadline.isValid()) {
    const bool late = f.isValid() && f > deadline;
    const double dx = xOf(deadline.addDays(1));
    p.setPen(Qt::NoPen);
    p.setBrush(late ? kLate : kMuted);
    p.drawPolygon(QPolygonF{{dx - 3, y + 1}, {dx + 3, y + 1}, {dx, y + 5}});
  }
  if (settings_.today.isValid() && settings_.today >= from_ && settings_.today <= to_) {
    const double tx = xOf(settings_.today) + perDay_ / 2;
    p.setPen(QPen(kLate, 0.75, Qt::DashLine));
    p.drawLine(QPointF(tx, y), QPointF(tx, y + kRowHeight));
  }
  p.restore();
}

void ProjectPrintout::paintPage(QPainter& p, int page) const {
  if (page < 0 || page >= pageCount()) return;
  p.save();
  p.fillRect(QRectF(QPointF(0, 0), page_), Qt::white);
  paintHeader(p, page);
  paintColumnHeader(p);
  const QList<int>& rows = pages_[page];
  const double bottom = bodyTop_ + static_cast<double>(rows.size()) * kRowHeight;
  paintTimeline(p, bodyTop_, std::max(bottom, bodyTop_ + kRowHeight));
  if (rows.isEmpty()) {
    p.setFont(font(9, false, true));
    p.setPen(kMuted);
    p.drawText(QRectF(kPad, bodyTop_, page_.width(), kRowHeight * 2), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("This project has no tasks."));
  }
  double y = bodyTop_;
  for (int i : rows) {
    paintRow(p, rows_[i], y, i % 2 == 1);
    y += kRowHeight;
  }
  p.setPen(QPen(kRule, 0.5));
  p.drawLine(QPointF(0, y), QPointF(page_.width(), y));
  if (ganttWidth_ > 0) p.drawLine(QPointF(ganttLeft_, bodyTop_ - kColumnHeaderHeight), QPointF(ganttLeft_, y));
  p.restore();
}

bool ProjectPrintout::print(QPagedPaintDevice* device) {
  QPainter p;
  if (device == nullptr || !p.begin(device)) return false;
  const double sx = device->logicalDpiX() / 72.0;
  const double sy = device->logicalDpiY() / 72.0;
  layout(QSizeF(device->width() / sx, device->height() / sy));
  for (int i = 0; i < pageCount(); ++i) {
    if (i > 0 && !device->newPage()) return false;
    p.save();
    p.scale(sx, sy);
    paintPage(p, i);
    p.restore();
  }
  return p.end();
}

}  // namespace zproject::app
