#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <optional>

#include "core/Model.hpp"
#include "core/WorkCalendar.hpp"

// Project exports (SPEC.md §7.9): zproject JSON, CSV, OpenDocument
// spreadsheet (.ods) and Microsoft Project XML (MSPDI). Pure Qt Core, so
// zprojectd serves them (GET /v1/projects/{id}/export) and the desktop and
// CLI share the same output. PDF and PNG need Qt Gui and are made by the
// desktop app.
//
// Every writer takes the same snapshot and is deterministic for it (the
// timestamp is `exportedAt`, never the clock). Titles and notes are
// untrusted (agents write them): XML output drops characters XML 1.0
// can't carry, and CSV guards text cells that a spreadsheet would run as a
// formula (fuzz/fuzz_export.cpp).
namespace zproject {

enum class ExportFormat { Json, Csv, Ods, Mspdi };

// "json", "csv", "ods", "mspdi" (case-insensitive); nullopt otherwise.
std::optional<ExportFormat> parseExportFormat(const QString& name);
QString exportFormatName(ExportFormat f);
QByteArray exportContentType(ExportFormat f);
// Without the dot: json, csv, ods, xml.
QString exportExtension(ExportFormat f);
// A safe ASCII file name from the project's name: "Office move" ->
// "Office-move.ods"; "project.<ext>" when nothing usable is left.
QString exportFileName(const QString& projectName, ExportFormat f);

inline constexpr int kExportSchemaVersion = 1;

// What an export contains: one project's live rows.
struct ExportData {
  Project project;
  QList<Task> tasks;               // any order; deleted rows are ignored
  QList<Dependency> dependencies;  // deleted rows and links to missing tasks are ignored
  // Every live calendar with its live exceptions, by id (for JSON and the
  // MSPDI calendars). Missing ones fall back to Standard.
  QHash<QString, CalendarDef> calendars;
  QString exportedAt;  // UTC ISO 8601, e.g. 2026-11-24T15:04:05.000Z
  QString generator;   // e.g. "zprojectd 0.22.0"
};

// One task in outline order, with the derived columns the tables and the
// desktop's printout share.
struct ExportRow {
  Task task;
  QString wbs;           // "1.2.3"
  int level = 1;         // 1 = top level
  bool summary = false;  // has a live child
  QString start;         // a summary: the earliest start below it if its own is empty
  QString finish;        // a summary: the latest finish below it if its own is empty
  QString predecessors;  // "3FS+2d, 4" by WBS code
};

// Live tasks depth-first: children follow their parent, siblings by
// `order`. A task whose parent is missing or deleted is top level; a
// parent cycle (never stored, but the input is not trusted) is broken.
QList<ExportRow> exportRows(const ExportData& data);

// One spreadsheet cell, typed so ODS keeps numbers and dates.
struct ExportCell {
  enum class Kind { Empty, Text, Number, Date, Percent };
  Kind kind = Kind::Empty;
  QString text;      // Text; Date as YYYY-MM-DD
  double number = 0; // Number; Percent as 0..100
};

struct ExportTable {
  QString name;  // the ODS sheet name
  QStringList header;
  QList<QList<ExportCell>> rows;
};

// The Tasks table (the CSV, and the first ODS sheet), the Dependencies
// table and the Project details table (the other ODS sheets).
ExportTable tasksTable(const QList<ExportRow>& rows);
ExportTable dependenciesTable(const ExportData& data, const QList<ExportRow>& rows);
ExportTable projectTable(const ExportData& data);

QByteArray exportJson(const ExportData& data);
QByteArray exportCsv(const ExportData& data);
QByteArray exportOds(const ExportData& data);
// The .ods package's content.xml on its own (for tests and the fuzzer,
// which check it without unzipping).
QByteArray odsContentXml(const ExportData& data);
QByteArray exportMspdi(const ExportData& data);
QByteArray exportProject(const ExportData& data, ExportFormat f);

// Reads an exportJson() document back (the desktop prints from it). A
// validation error unless it is a "zproject-export" object of a schema
// version this build reads. Rows are decoded as the API decodes them;
// nothing else is checked.
Result<ExportData> exportDataFromJson(const QByteArray& json);

// The text with every character XML 1.0 forbids removed (C0 controls other
// than tab, newline and carriage return; U+FFFE, U+FFFF; lone surrogates).
QString xmlSafe(const QString& s);

}  // namespace zproject
