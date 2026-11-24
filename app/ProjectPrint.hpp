#pragma once

#include <QDate>
#include <QList>
#include <QSizeF>
#include <QString>

#include "core/Export.hpp"

class QPagedPaintDevice;
class QPainter;

namespace zproject::app {

struct PrintSettings {
  QString subtitle;  // top right, e.g. "Printed 2026-11-24 15:04"
  QString note;      // under the header, e.g. the offline notice; empty: none
  QString footer;    // bottom left, e.g. "zproject 0.23.0"
  QDate today;       // the today line; invalid: none
};

// A project's printout (SPEC.md §7.9, §8.2): on every page a header with
// the project's name and dates, a task table (WBS, task, duration, start,
// finish, % complete, predecessors) and, beside it, a Gantt timeline scaled
// to fit the page width (day, week, month or quarter ticks). Rows run on
// over as many pages as needed; the column headings repeat; "Page n of m"
// at the foot. It always prints in light colours, whatever the theme.
//
// Everything is laid out in points (1/72 inch) for the printable area, so
// the same pages go to a printer, a PDF or, in tests, an image.
class ProjectPrintout {
 public:
  enum class Unit { Day, Week, Month, Quarter };

  ProjectPrintout(const ExportData& data, PrintSettings settings);

  // Lays the pages out for a printable area of `page` points.
  void layout(const QSizeF& page);
  int pageCount() const { return static_cast<int>(pages_.size()); }
  // The rows (indexes into rows()) on a page.
  QList<int> rowsOnPage(int page) const { return pages_.value(page); }
  const QList<ExportRow>& rows() const { return rows_; }
  bool hasGantt() const { return ganttWidth_ > 0; }
  Unit unit() const { return unit_; }
  QDate from() const { return from_; }
  QDate to() const { return to_; }

  // Paints one page with the painter in points, (0, 0) at the top left of
  // the printable area.
  void paintPage(QPainter& p, int page) const;

  // Lays out for `device`'s printable area and paints every page on it (a
  // QPrinter or QPdfWriter). False when the device can't be painted on.
  bool print(QPagedPaintDevice* device);

 private:
  struct Column {
    QString title;
    double width = 0;
    bool right = false;  // right-aligned
  };
  double xOf(const QDate& d) const;
  void paintHeader(QPainter& p, int page) const;
  void paintColumnHeader(QPainter& p) const;
  void paintRow(QPainter& p, const ExportRow& r, double y, bool shaded) const;
  void paintTimeline(QPainter& p, double top, double bottom) const;
  void paintBar(QPainter& p, const ExportRow& r, double y) const;
  QString cell(const ExportRow& r, int column) const;

  ExportData data_;
  PrintSettings settings_;
  QList<ExportRow> rows_;
  QSizeF page_;
  QList<Column> columns_;
  double tableWidth_ = 0;
  double ganttLeft_ = 0;
  double ganttWidth_ = 0;
  double bodyTop_ = 0;
  QDate from_;
  QDate to_;
  double perDay_ = 0;
  Unit unit_ = Unit::Week;
  QList<QList<int>> pages_;
};

}  // namespace zproject::app
