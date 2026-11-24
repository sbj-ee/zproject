#include "core/Export.hpp"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cmath>

#include "core/LinkNotation.hpp"
#include "core/Rollup.hpp"
#include "core/Zip.hpp"

namespace zproject {
namespace {

QString fmtNumber(double d) {
  if (!std::isfinite(d)) return QStringLiteral("0");
  if (d == std::floor(d) && std::fabs(d) < 1e15) return QString::number(static_cast<qint64>(d));
  return QString::number(d, 'g', 12);
}

ExportCell text(const QString& s) {
  if (s.isEmpty()) return {};
  return {ExportCell::Kind::Text, s, 0};
}
ExportCell number(double d) { return {ExportCell::Kind::Number, {}, d}; }
ExportCell date(const QString& s) {
  if (s.isEmpty()) return {};
  // A stored date is always valid; anything else is kept as text.
  return {isValidDate(s) ? ExportCell::Kind::Date : ExportCell::Kind::Text, s, 0};
}

QString calendarName(const ExportData& data, const QString& id) {
  if (id.isEmpty()) return {};
  const auto it = data.calendars.constFind(id);
  return it == data.calendars.constEnd() || it->name.isEmpty() ? id : it->name;
}

}  // namespace

// ------------------------------------------------------------ formats

std::optional<ExportFormat> parseExportFormat(const QString& name) {
  const QString n = name.trimmed().toLower();
  if (n == QLatin1String("json")) return ExportFormat::Json;
  if (n == QLatin1String("csv")) return ExportFormat::Csv;
  if (n == QLatin1String("ods")) return ExportFormat::Ods;
  if (n == QLatin1String("mspdi")) return ExportFormat::Mspdi;
  return std::nullopt;
}

QString exportFormatName(ExportFormat f) {
  switch (f) {
    case ExportFormat::Json: return QStringLiteral("json");
    case ExportFormat::Csv: return QStringLiteral("csv");
    case ExportFormat::Ods: return QStringLiteral("ods");
    case ExportFormat::Mspdi: return QStringLiteral("mspdi");
  }
  return {};
}

QByteArray exportContentType(ExportFormat f) {
  switch (f) {
    case ExportFormat::Json: return "application/json";
    case ExportFormat::Csv: return "text/csv; charset=utf-8; header=present";
    case ExportFormat::Ods: return "application/vnd.oasis.opendocument.spreadsheet";
    case ExportFormat::Mspdi: return "application/xml";
  }
  return "application/octet-stream";
}

QString exportExtension(ExportFormat f) {
  switch (f) {
    case ExportFormat::Json: return QStringLiteral("json");
    case ExportFormat::Csv: return QStringLiteral("csv");
    case ExportFormat::Ods: return QStringLiteral("ods");
    case ExportFormat::Mspdi: return QStringLiteral("xml");
  }
  return {};
}

QString exportFileName(const QString& projectName, ExportFormat f) {
  // Keep ASCII letters, digits, '.', '_' and '-'; everything else becomes
  // one '-'. No leading dots or dashes (no hidden files, no options).
  QString out;
  bool dash = false;
  for (const QChar c : projectName) {
    const char16_t u = c.unicode();
    const bool keep = (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9') ||
                      u == u'.' || u == u'_' || u == u'-';
    if (keep) {
      out += c;
      dash = false;
    } else if (!dash && !out.isEmpty()) {
      out += QLatin1Char('-');
      dash = true;
    }
  }
  while (!out.isEmpty() && (out.endsWith(QLatin1Char('-')) || out.endsWith(QLatin1Char('.')))) out.chop(1);
  while (!out.isEmpty() && (out.startsWith(QLatin1Char('-')) || out.startsWith(QLatin1Char('.')))) out.remove(0, 1);
  out.truncate(80);
  if (out.isEmpty()) out = QStringLiteral("project");
  return out + QLatin1Char('.') + exportExtension(f);
}

QString xmlSafe(const QString& s) {
  QString out;
  out.reserve(s.size());
  for (qsizetype i = 0; i < s.size(); ++i) {
    const QChar c = s.at(i);
    const char16_t u = c.unicode();
    if (c.isHighSurrogate()) {
      if (i + 1 < s.size() && s.at(i + 1).isLowSurrogate()) {
        out += c;
        out += s.at(++i);
      }
      continue;
    }
    if (c.isLowSurrogate()) continue;
    if (u < 0x20 && u != u'\t' && u != u'\n' && u != u'\r') continue;
    if (u == 0xFFFE || u == 0xFFFF) continue;
    out += c;
  }
  return out;
}

// ------------------------------------------------------------ rows

QList<ExportRow> exportRows(const ExportData& data) {
  QList<const Task*> live;
  QHash<QString, const Task*> byId;
  for (const Task& t : data.tasks) {
    if (t.deleted || t.id.isEmpty() || byId.contains(t.id)) continue;
    live.append(&t);
    byId.insert(t.id, &t);
  }
  auto sibling = [](const Task* a, const Task* b) { return a->order != b->order ? a->order < b->order : a->id < b->id; };
  std::stable_sort(live.begin(), live.end(), sibling);

  QHash<QString, QList<const Task*>> children;
  QList<const Task*> roots;
  for (const Task* t : live) {
    if (!t->parent.isEmpty() && t->parent != t->id && byId.contains(t->parent)) {
      children[t->parent].append(t);
    } else {
      roots.append(t);
    }
  }

  QList<ExportRow> rows;
  rows.reserve(live.size());
  QSet<QString> visited;
  int topCount = 0;
  struct Frame {
    const Task* task;
    int level;
    QString wbs;
  };
  // Iterative depth-first walk: a deep outline must not exhaust the stack.
  auto walk = [&](const Task* root) {
    if (visited.contains(root->id)) return;
    visited.insert(root->id);
    QList<Frame> stack{{root, 1, QString::number(++topCount)}};
    while (!stack.isEmpty()) {
      const Frame f = stack.takeLast();
      ExportRow r;
      r.task = *f.task;
      r.level = f.level;
      r.wbs = f.wbs;
      r.start = f.task->start;
      r.finish = f.task->finish;
      QList<Frame> kids;
      int n = 0;
      for (const Task* c : children.value(f.task->id)) {
        if (visited.contains(c->id)) continue;  // a cycle; already placed
        visited.insert(c->id);
        kids.append({c, f.level + 1, f.wbs + QLatin1Char('.') + QString::number(++n)});
      }
      r.summary = n > 0;
      rows.append(r);
      for (auto it = kids.crbegin(); it != kids.crend(); ++it) stack.append(*it);
    }
  };
  for (const Task* t : roots) walk(t);
  // Tasks only reachable through a parent cycle: break it at the first one.
  for (const Task* t : live) walk(t);

  // Summary spans, bottom-up: a row's descendants follow it.
  QList<QString> minStart(rows.size());
  QList<QString> maxFinish(rows.size());
  QList<int> parentRow(rows.size(), -1);
  {
    QList<int> open;  // row index per level
    for (int i = 0; i < rows.size(); ++i) {
      const int lvl = rows[i].level;
      while (open.size() >= lvl) open.removeLast();
      parentRow[i] = open.isEmpty() ? -1 : open.last();
      open.append(i);
    }
  }
  for (int i = static_cast<int>(rows.size()) - 1; i >= 0; --i) {
    ExportRow& r = rows[i];
    if (r.summary) {
      if (r.start.isEmpty()) r.start = minStart[i];
      if (r.finish.isEmpty()) r.finish = maxFinish[i];
    }
    const int p = parentRow[i];
    if (p < 0) continue;
    if (!r.start.isEmpty() && (minStart[p].isEmpty() || r.start < minStart[p])) minStart[p] = r.start;
    if (!r.finish.isEmpty() && (maxFinish[p].isEmpty() || r.finish > maxFinish[p])) maxFinish[p] = r.finish;
  }

  // Predecessors by WBS code, in the predecessors' row order.
  QHash<QString, int> rowOf;
  for (int i = 0; i < rows.size(); ++i) rowOf.insert(rows[i].task.id, i);
  QList<const Dependency*> deps;
  for (const Dependency& d : data.dependencies) {
    if (!d.deleted && rowOf.contains(d.predecessor) && rowOf.contains(d.successor)) deps.append(&d);
  }
  std::stable_sort(deps.begin(), deps.end(), [&](const Dependency* a, const Dependency* b) {
    return rowOf.value(a->predecessor) < rowOf.value(b->predecessor);
  });
  QHash<int, QStringList> preds;
  for (const Dependency* d : deps) {
    preds[rowOf.value(d->successor)].append(formatLink(rows[rowOf.value(d->predecessor)].wbs, d->type, d->lag));
  }
  for (auto it = preds.constBegin(); it != preds.constEnd(); ++it) {
    rows[it.key()].predecessors = it.value().join(QStringLiteral(", "));
  }
  return rows;
}

// ------------------------------------------------------------ tables

ExportTable tasksTable(const QList<ExportRow>& rows) {
  ExportTable t;
  t.name = QStringLiteral("Tasks");
  t.header = {QStringLiteral("WBS"),           QStringLiteral("Level"),         QStringLiteral("Title"),
              QStringLiteral("Type"),          QStringLiteral("Mode"),          QStringLiteral("Duration (days)"),
              QStringLiteral("Start"),         QStringLiteral("Finish"),        QStringLiteral("Predecessors"),
              QStringLiteral("% Complete"),    QStringLiteral("Status"),        QStringLiteral("Priority"),
              QStringLiteral("Constraint"),    QStringLiteral("Constraint date"), QStringLiteral("Deadline"),
              QStringLiteral("Actual start"),  QStringLiteral("Actual finish"), QStringLiteral("Fixed cost"),
              QStringLiteral("Notes"),         QStringLiteral("ID")};
  for (const ExportRow& r : rows) {
    const Task& k = r.task;
    QList<ExportCell> c;
    c << text(r.wbs) << number(r.level) << text(k.title)
      << text(r.summary ? QStringLiteral("summary") : k.type) << text(k.mode)
      << (r.summary ? ExportCell{} : number(k.duration)) << date(r.start) << date(r.finish) << text(r.predecessors)
      << ExportCell{ExportCell::Kind::Percent, {}, static_cast<double>(k.percentComplete)} << text(k.status)
      << number(k.priority) << text(k.constraintType) << date(k.constraintDate) << date(k.deadline)
      << date(k.actualStart) << date(k.actualFinish) << number(k.fixedCost) << text(k.notes) << text(k.id);
    t.rows.append(c);
  }
  return t;
}

ExportTable dependenciesTable(const ExportData& data, const QList<ExportRow>& rows) {
  ExportTable t;
  t.name = QStringLiteral("Dependencies");
  t.header = {QStringLiteral("Predecessor WBS"), QStringLiteral("Predecessor"), QStringLiteral("Successor WBS"),
              QStringLiteral("Successor"),       QStringLiteral("Type"),        QStringLiteral("Lag (days)"),
              QStringLiteral("ID")};
  QHash<QString, int> rowOf;
  for (int i = 0; i < rows.size(); ++i) rowOf.insert(rows[i].task.id, i);
  QList<const Dependency*> deps;
  for (const Dependency& d : data.dependencies) {
    if (!d.deleted && rowOf.contains(d.predecessor) && rowOf.contains(d.successor)) deps.append(&d);
  }
  std::stable_sort(deps.begin(), deps.end(), [&](const Dependency* a, const Dependency* b) {
    const int sa = rowOf.value(a->successor);
    const int sb = rowOf.value(b->successor);
    return sa != sb ? sa < sb : rowOf.value(a->predecessor) < rowOf.value(b->predecessor);
  });
  for (const Dependency* d : deps) {
    const ExportRow& p = rows[rowOf.value(d->predecessor)];
    const ExportRow& s = rows[rowOf.value(d->successor)];
    t.rows.append({text(p.wbs), text(p.task.title), text(s.wbs), text(s.task.title), text(d->type), number(d->lag),
                   text(d->id)});
  }
  return t;
}

ExportTable projectTable(const ExportData& data) {
  const Project& p = data.project;
  ExportTable t;
  t.name = QStringLiteral("Project");
  t.header = {QStringLiteral("Field"), QStringLiteral("Value")};
  auto row = [&](const QString& k, const ExportCell& v) { t.rows.append({text(k), v}); };
  row(QStringLiteral("Name"), text(p.name));
  row(QStringLiteral("Status"), text(p.status));
  row(QStringLiteral("RAG"), text(p.rag));
  row(QStringLiteral("Priority"), number(p.priority));
  row(QStringLiteral("Start"), date(p.start));
  row(QStringLiteral("Target finish"), date(p.finish));
  row(QStringLiteral("Owner"), text(p.owner));
  row(QStringLiteral("Sponsor"), text(p.sponsor));
  row(QStringLiteral("Budget"), p.budget ? number(*p.budget) : ExportCell{});
  row(QStringLiteral("Calendar"),
      text(calendarName(data, p.calendar.isEmpty() ? QString(kStandardCalendarId) : p.calendar)));
  row(QStringLiteral("Description"), text(p.description));
  row(QStringLiteral("Objectives"), text(p.objectives));
  row(QStringLiteral("Scope"), text(p.scope));
  row(QStringLiteral("Out of scope"), text(p.outOfScope));
  row(QStringLiteral("Success criteria"), text(p.successCriteria));
  row(QStringLiteral("Exported at"), text(data.exportedAt));
  row(QStringLiteral("Exported by"), text(data.generator));
  row(QStringLiteral("ID"), text(p.id));
  return t;
}

// ------------------------------------------------------------ JSON

QByteArray exportJson(const ExportData& data) {
  const QList<ExportRow> rows = exportRows(data);
  QSet<QString> taskIds;
  QJsonArray tasks;
  for (const ExportRow& r : rows) {
    taskIds.insert(r.task.id);
    QJsonObject o = toJson(r.task);
    o.insert(QStringLiteral("wbs"), r.wbs);
    tasks.append(o);
  }
  QJsonArray deps;
  for (const Dependency& d : data.dependencies) {
    if (!d.deleted && taskIds.contains(d.predecessor) && taskIds.contains(d.successor)) deps.append(toJson(d));
  }
  QStringList calIds = data.calendars.keys();
  std::sort(calIds.begin(), calIds.end());
  QJsonArray cals;
  for (const QString& id : calIds) {
    const CalendarDef& c = data.calendars.value(id);
    if (c.deleted) continue;
    QJsonObject o = toJson(c);
    QJsonArray ex;
    for (const CalendarException& e : c.exceptions) {
      if (!e.deleted) ex.append(toJson(e));
    }
    o.insert(QStringLiteral("exceptions"), ex);
    cals.append(o);
  }
  const QJsonObject root{{QStringLiteral("format"), QStringLiteral("zproject-export")},
                         {QStringLiteral("schemaVersion"), kExportSchemaVersion},
                         {QStringLiteral("exportedAt"), data.exportedAt},
                         {QStringLiteral("generator"), data.generator},
                         {QStringLiteral("project"), toJson(data.project)},
                         {QStringLiteral("tasks"), tasks},
                         {QStringLiteral("dependencies"), deps},
                         {QStringLiteral("calendars"), cals}};
  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// ------------------------------------------------------------ CSV

namespace {

// RFC 4180 field. Text that a spreadsheet would read as a formula (it
// starts with = + - @, a tab or a carriage return) gets a leading
// apostrophe, as OWASP recommends for CSV injection.
QString csvField(const ExportCell& c) {
  QString s;
  switch (c.kind) {
    case ExportCell::Kind::Empty: return {};
    case ExportCell::Kind::Number:
    case ExportCell::Kind::Percent: s = fmtNumber(c.number); break;
    case ExportCell::Kind::Date: s = c.text; break;
    case ExportCell::Kind::Text:
      s = c.text;
      if (!s.isEmpty()) {
        const QChar f = s.front();
        if (f == u'=' || f == u'+' || f == u'-' || f == u'@' || f == u'\t' || f == u'\r') s.prepend(QLatin1Char('\''));
      }
      break;
  }
  static const QRegularExpression needsQuotes(QStringLiteral("[\",\\r\\n]|^\\s|\\s$"));
  if (s.contains(needsQuotes)) {
    s.replace(QLatin1Char('"'), QLatin1String("\"\""));
    s = QLatin1Char('"') + s + QLatin1Char('"');
  }
  return s;
}

}  // namespace

QByteArray exportCsv(const ExportData& data) {
  const ExportTable t = tasksTable(exportRows(data));
  QString out;
  QStringList line;
  for (const QString& h : t.header) line << csvField(text(h));
  out += line.join(QLatin1Char(',')) + QLatin1String("\r\n");
  for (const auto& r : t.rows) {
    line.clear();
    for (const ExportCell& c : r) line << csvField(c);
    out += line.join(QLatin1Char(',')) + QLatin1String("\r\n");
  }
  return out.toUtf8();
}

// ------------------------------------------------------------ ODS

namespace {

const QString kNsOffice = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
const QString kNsStyle = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:style:1.0");
const QString kNsText = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:text:1.0");
const QString kNsTable = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:table:1.0");
const QString kNsFo = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");
const QString kNsNumber = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0");
const QString kNsMeta = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:meta:1.0");
const QString kNsDc = QStringLiteral("http://purl.org/dc/elements/1.1/");
const QString kNsManifest = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:manifest:1.0");
const QString kOdsMime = QStringLiteral("application/vnd.oasis.opendocument.spreadsheet");

// One line of text as ODF wants it: runs of spaces as <text:s/>, tabs as
// <text:tab/> (a plain run would collapse to one space).
void writeOdfLine(QXmlStreamWriter& w, const QString& line) {
  w.writeStartElement(kNsText, QStringLiteral("p"));
  QString run;
  for (qsizetype i = 0; i < line.size(); ++i) {
    const QChar c = line.at(i);
    if (c == u'\t') {
      w.writeCharacters(run);
      run.clear();
      w.writeEmptyElement(kNsText, QStringLiteral("tab"));
    } else if (c == u' ' && (run.isEmpty() || run.endsWith(QLatin1Char(' ')) || i + 1 == line.size())) {
      qsizetype n = 1;
      while (i + n < line.size() && line.at(i + n) == u' ') ++n;
      w.writeCharacters(run);
      run.clear();
      w.writeEmptyElement(kNsText, QStringLiteral("s"));
      if (n > 1) w.writeAttribute(kNsText, QStringLiteral("c"), QString::number(n));
      i += n - 1;
    } else {
      run += c;
    }
  }
  w.writeCharacters(run);
  w.writeEndElement();
}

void writeOdfText(QXmlStreamWriter& w, const QString& s) {
  QString t = xmlSafe(s);
  t.replace(QLatin1String("\r\n"), QLatin1String("\n"));
  t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
  for (const QString& line : t.split(QLatin1Char('\n'))) writeOdfLine(w, line);
}

void writeCell(QXmlStreamWriter& w, const ExportCell& c, const QString& style = {}) {
  if (c.kind == ExportCell::Kind::Empty) {
    w.writeEmptyElement(kNsTable, QStringLiteral("table-cell"));
    if (!style.isEmpty()) w.writeAttribute(kNsTable, QStringLiteral("style-name"), style);
    return;
  }
  w.writeStartElement(kNsTable, QStringLiteral("table-cell"));
  switch (c.kind) {
    case ExportCell::Kind::Number:
      w.writeAttribute(kNsOffice, QStringLiteral("value-type"), QStringLiteral("float"));
      w.writeAttribute(kNsOffice, QStringLiteral("value"), fmtNumber(c.number));
      if (!style.isEmpty()) w.writeAttribute(kNsTable, QStringLiteral("style-name"), style);
      writeOdfText(w, fmtNumber(c.number));
      break;
    case ExportCell::Kind::Percent:
      w.writeAttribute(kNsOffice, QStringLiteral("value-type"), QStringLiteral("percentage"));
      w.writeAttribute(kNsOffice, QStringLiteral("value"), fmtNumber(c.number / 100.0));
      w.writeAttribute(kNsTable, QStringLiteral("style-name"), QStringLiteral("cePct"));
      writeOdfText(w, fmtNumber(c.number) + QLatin1Char('%'));
      break;
    case ExportCell::Kind::Date:
      w.writeAttribute(kNsOffice, QStringLiteral("value-type"), QStringLiteral("date"));
      w.writeAttribute(kNsOffice, QStringLiteral("date-value"), c.text);
      w.writeAttribute(kNsTable, QStringLiteral("style-name"), QStringLiteral("ceDate"));
      writeOdfText(w, c.text);
      break;
    case ExportCell::Kind::Text:
    case ExportCell::Kind::Empty:
      w.writeAttribute(kNsOffice, QStringLiteral("value-type"), QStringLiteral("string"));
      if (!style.isEmpty()) w.writeAttribute(kNsTable, QStringLiteral("style-name"), style);
      writeOdfText(w, c.text);
      break;
  }
  w.writeEndElement();
}

// Column width in cm from the header and the longest value (capped).
double columnWidthCm(const ExportTable& t, qsizetype col) {
  qsizetype chars = t.header.value(col).size();
  for (const auto& r : t.rows) {
    if (col >= r.size()) continue;
    const ExportCell& c = r[col];
    qsizetype n = 0;
    if (c.kind == ExportCell::Kind::Text) {
      n = c.text.indexOf(QLatin1Char('\n')) < 0 ? c.text.size() : c.text.indexOf(QLatin1Char('\n'));
    } else if (c.kind == ExportCell::Kind::Date) {
      n = 10;
    } else if (c.kind != ExportCell::Kind::Empty) {
      n = fmtNumber(c.number).size() + 1;
    }
    chars = std::max(chars, n);
  }
  return std::clamp(0.22 * static_cast<double>(chars) + 0.4, 1.4, 10.0);
}

QByteArray odsContent(const QList<ExportTable>& tables) {
  QByteArray buf;
  QXmlStreamWriter w(&buf);
  w.writeStartDocument();
  w.writeNamespace(kNsOffice, QStringLiteral("office"));
  w.writeNamespace(kNsStyle, QStringLiteral("style"));
  w.writeNamespace(kNsText, QStringLiteral("text"));
  w.writeNamespace(kNsTable, QStringLiteral("table"));
  w.writeNamespace(kNsFo, QStringLiteral("fo"));
  w.writeNamespace(kNsNumber, QStringLiteral("number"));
  w.writeStartElement(kNsOffice, QStringLiteral("document-content"));
  w.writeAttribute(kNsOffice, QStringLiteral("version"), QStringLiteral("1.2"));

  w.writeStartElement(kNsOffice, QStringLiteral("automatic-styles"));
  // YYYY-MM-DD dates and whole percentages.
  w.writeStartElement(kNsNumber, QStringLiteral("date-style"));
  w.writeAttribute(kNsStyle, QStringLiteral("name"), QStringLiteral("NDate"));
  for (const char* part : {"year", "-", "month", "-", "day"}) {
    if (part[0] == '-') {
      w.writeTextElement(kNsNumber, QStringLiteral("text"), QStringLiteral("-"));
    } else {
      w.writeEmptyElement(kNsNumber, QString::fromLatin1(part));
      w.writeAttribute(kNsNumber, QStringLiteral("style"), QStringLiteral("long"));
    }
  }
  w.writeEndElement();
  w.writeStartElement(kNsNumber, QStringLiteral("percentage-style"));
  w.writeAttribute(kNsStyle, QStringLiteral("name"), QStringLiteral("NPct"));
  w.writeEmptyElement(kNsNumber, QStringLiteral("number"));
  w.writeAttribute(kNsNumber, QStringLiteral("decimal-places"), QStringLiteral("0"));
  w.writeAttribute(kNsNumber, QStringLiteral("min-integer-digits"), QStringLiteral("1"));
  w.writeTextElement(kNsNumber, QStringLiteral("text"), QStringLiteral("%"));
  w.writeEndElement();
  auto cellStyle = [&](const QString& name, const QString& dataStyle, bool bold, bool wrap) {
    w.writeStartElement(kNsStyle, QStringLiteral("style"));
    w.writeAttribute(kNsStyle, QStringLiteral("name"), name);
    w.writeAttribute(kNsStyle, QStringLiteral("family"), QStringLiteral("table-cell"));
    if (!dataStyle.isEmpty()) w.writeAttribute(kNsStyle, QStringLiteral("data-style-name"), dataStyle);
    if (wrap) {
      w.writeEmptyElement(kNsStyle, QStringLiteral("table-cell-properties"));
      w.writeAttribute(kNsFo, QStringLiteral("wrap-option"), QStringLiteral("wrap"));
      w.writeAttribute(kNsStyle, QStringLiteral("vertical-align"), QStringLiteral("top"));
    }
    if (bold) {
      w.writeEmptyElement(kNsStyle, QStringLiteral("text-properties"));
      w.writeAttribute(kNsFo, QStringLiteral("font-weight"), QStringLiteral("bold"));
    }
    w.writeEndElement();
  };
  cellStyle(QStringLiteral("ceHead"), {}, true, false);
  cellStyle(QStringLiteral("ceDate"), QStringLiteral("NDate"), false, false);
  cellStyle(QStringLiteral("cePct"), QStringLiteral("NPct"), false, false);
  cellStyle(QStringLiteral("ceWrap"), {}, false, true);
  for (qsizetype ti = 0; ti < tables.size(); ++ti) {
    for (qsizetype ci = 0; ci < tables[ti].header.size(); ++ci) {
      w.writeStartElement(kNsStyle, QStringLiteral("style"));
      w.writeAttribute(kNsStyle, QStringLiteral("name"), QStringLiteral("co%1_%2").arg(ti).arg(ci));
      w.writeAttribute(kNsStyle, QStringLiteral("family"), QStringLiteral("table-column"));
      w.writeEmptyElement(kNsStyle, QStringLiteral("table-column-properties"));
      w.writeAttribute(kNsStyle, QStringLiteral("column-width"),
                       QString::number(columnWidthCm(tables[ti], ci), 'f', 2) + QStringLiteral("cm"));
      w.writeEndElement();
    }
  }
  w.writeEndElement();  // automatic-styles

  w.writeStartElement(kNsOffice, QStringLiteral("body"));
  w.writeStartElement(kNsOffice, QStringLiteral("spreadsheet"));
  for (qsizetype ti = 0; ti < tables.size(); ++ti) {
    const ExportTable& t = tables[ti];
    w.writeStartElement(kNsTable, QStringLiteral("table"));
    w.writeAttribute(kNsTable, QStringLiteral("name"), t.name);
    for (qsizetype ci = 0; ci < t.header.size(); ++ci) {
      w.writeEmptyElement(kNsTable, QStringLiteral("table-column"));
      w.writeAttribute(kNsTable, QStringLiteral("style-name"), QStringLiteral("co%1_%2").arg(ti).arg(ci));
    }
    // The header repeats on every printed page.
    w.writeStartElement(kNsTable, QStringLiteral("table-header-rows"));
    w.writeStartElement(kNsTable, QStringLiteral("table-row"));
    for (const QString& h : t.header) writeCell(w, text(h), QStringLiteral("ceHead"));
    w.writeEndElement();
    w.writeEndElement();
    for (const auto& r : t.rows) {
      w.writeStartElement(kNsTable, QStringLiteral("table-row"));
      for (qsizetype ci = 0; ci < t.header.size(); ++ci) {
        const ExportCell c = r.value(ci);
        const bool multiline = c.kind == ExportCell::Kind::Text && c.text.contains(QLatin1Char('\n'));
        writeCell(w, c, multiline ? QStringLiteral("ceWrap") : QString());
      }
      w.writeEndElement();
    }
    w.writeEndElement();  // table
  }
  w.writeEndElement();  // spreadsheet
  w.writeEndElement();  // body
  w.writeEndElement();  // document-content
  w.writeEndDocument();
  return buf;
}

QByteArray odsStyles() {
  QByteArray buf;
  QXmlStreamWriter w(&buf);
  w.writeStartDocument();
  w.writeNamespace(kNsOffice, QStringLiteral("office"));
  w.writeStartElement(kNsOffice, QStringLiteral("document-styles"));
  w.writeAttribute(kNsOffice, QStringLiteral("version"), QStringLiteral("1.2"));
  w.writeEndElement();
  w.writeEndDocument();
  return buf;
}

QByteArray odsMeta(const ExportData& data) {
  QByteArray buf;
  QXmlStreamWriter w(&buf);
  w.writeStartDocument();
  w.writeNamespace(kNsOffice, QStringLiteral("office"));
  w.writeNamespace(kNsMeta, QStringLiteral("meta"));
  w.writeNamespace(kNsDc, QStringLiteral("dc"));
  w.writeStartElement(kNsOffice, QStringLiteral("document-meta"));
  w.writeAttribute(kNsOffice, QStringLiteral("version"), QStringLiteral("1.2"));
  w.writeStartElement(kNsOffice, QStringLiteral("meta"));
  if (!data.generator.isEmpty()) w.writeTextElement(kNsMeta, QStringLiteral("generator"), xmlSafe(data.generator));
  w.writeTextElement(kNsDc, QStringLiteral("title"), xmlSafe(data.project.name));
  const QDateTime at = QDateTime::fromString(data.exportedAt, Qt::ISODateWithMs);
  if (at.isValid()) {
    w.writeTextElement(kNsMeta, QStringLiteral("creation-date"), at.toUTC().toString(Qt::ISODate));
  }
  w.writeEndElement();
  w.writeEndElement();
  w.writeEndDocument();
  return buf;
}

QByteArray odsManifest() {
  QByteArray buf;
  QXmlStreamWriter w(&buf);
  w.writeStartDocument();
  w.writeNamespace(kNsManifest, QStringLiteral("manifest"));
  w.writeStartElement(kNsManifest, QStringLiteral("manifest"));
  w.writeAttribute(kNsManifest, QStringLiteral("version"), QStringLiteral("1.2"));
  auto entry = [&](const QString& path, const QString& type) {
    w.writeEmptyElement(kNsManifest, QStringLiteral("file-entry"));
    w.writeAttribute(kNsManifest, QStringLiteral("full-path"), path);
    if (path == QLatin1String("/")) w.writeAttribute(kNsManifest, QStringLiteral("version"), QStringLiteral("1.2"));
    w.writeAttribute(kNsManifest, QStringLiteral("media-type"), type);
  };
  entry(QStringLiteral("/"), kOdsMime);
  entry(QStringLiteral("content.xml"), QStringLiteral("text/xml"));
  entry(QStringLiteral("styles.xml"), QStringLiteral("text/xml"));
  entry(QStringLiteral("meta.xml"), QStringLiteral("text/xml"));
  w.writeEndElement();
  w.writeEndDocument();
  return buf;
}

}  // namespace

QByteArray odsContentXml(const ExportData& data) {
  const QList<ExportRow> rows = exportRows(data);
  return odsContent({tasksTable(rows), dependenciesTable(data, rows), projectTable(data)});
}

QByteArray exportOds(const ExportData& data) {
  ZipWriter zip(QDateTime::fromString(data.exportedAt, Qt::ISODateWithMs));
  // ODF: "mimetype" first, stored, so tools can sniff the type.
  zip.add(QStringLiteral("mimetype"), kOdsMime.toLatin1(), false);
  zip.add(QStringLiteral("content.xml"), odsContentXml(data));
  zip.add(QStringLiteral("styles.xml"), odsStyles());
  zip.add(QStringLiteral("meta.xml"), odsMeta(data));
  zip.add(QStringLiteral("META-INF/manifest.xml"), odsManifest());
  return zip.finish();
}

// ------------------------------------------------------------ MSPDI

namespace {

const QString kNsMspdi = QStringLiteral("http://schemas.microsoft.com/project");

// A day's working hours as MS Project working times: from 08:00, with a
// lunch hour at 12:00 once the day is longer than four hours. Days too
// long for that start at midnight.
QList<QPair<int, int>> workingTimes(double hours) {
  const int m = std::clamp(static_cast<int>(std::lround(hours * 60)), 0, 24 * 60);
  if (m == 0) return {};
  if (m <= 240) return {{8 * 60, 8 * 60 + m}};
  if (m <= 240 + 11 * 60) return {{8 * 60, 12 * 60}, {13 * 60, 13 * 60 + (m - 240)}};
  return {{0, m}};
}

QString hhmm(int minutes) {
  if (minutes >= 24 * 60) minutes = 0;  // MS Project writes midnight at the end of a day as 00:00:00
  return QStringLiteral("%1:%2:00").arg(minutes / 60, 2, 10, QLatin1Char('0')).arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

QString pt(qint64 minutes) {
  return QStringLiteral("PT%1H%2M0S").arg(minutes / 60).arg(minutes % 60);
}

struct MsCalendar {
  int uid = 1;
  QString name;
  WorkWeek week;
  double hoursPerDay = 8;
  // Days that differ from the week, as ranges of equal days.
  struct Range {
    QDate from;
    QDate to;
    bool working = false;
    double hours = 0;
  };
  QList<Range> exceptions;
};

MsCalendar buildCalendar(const ExportData& data, const QString& id, int uid) {
  MsCalendar m;
  m.uid = uid;
  auto resolved = WorkCalendar::resolve(id, data.calendars);
  const WorkCalendar wc = resolved ? resolved.value() : WorkCalendar();
  m.week = wc.workWeek();
  m.hoursPerDay = wc.hoursPerDay();
  m.name = calendarName(data, id.isEmpty() ? QString(kStandardCalendarId) : id);
  if (m.name.isEmpty() || m.name == QLatin1String(kStandardCalendarId)) m.name = QStringLiteral("Standard");
  if (!resolved) return m;

  // The base chain, root first, so the calendar's own exceptions win.
  QStringList chain;
  QString cur = id.isEmpty() ? QString(kStandardCalendarId) : id;
  while (!cur.isEmpty() && !chain.contains(cur) && chain.size() <= kMaxCalendarDepth &&
         data.calendars.contains(cur)) {
    chain.prepend(cur);
    cur = data.calendars.value(cur).base;
  }
  struct Day {
    std::optional<bool> working;
    std::optional<double> hours;
  };
  QMap<QDate, Day> days;
  for (const QString& cid : chain) {
    for (const CalendarException& e : data.calendars.value(cid).exceptions) {
      if (e.deleted || !e.from.isValid() || !e.to.isValid() || e.to < e.from || e.from.daysTo(e.to) > kMaxExceptionDays) {
        continue;
      }
      for (QDate d = e.from; d <= e.to; d = d.addDays(1)) {
        Day& day = days[d];
        if (e.kind == QLatin1String("custom_hours")) {
          day.hours = e.hours;
        } else {
          day.working = false;
          day.hours.reset();
        }
      }
    }
  }
  for (auto it = days.constBegin(); it != days.constEnd(); ++it) {
    const int dow = it.key().dayOfWeek() - 1;
    const bool weekWorking = m.week.working[static_cast<size_t>(dow)];
    const double weekHours = m.week.hours[static_cast<size_t>(dow)];
    const bool working = it->working.value_or(weekWorking) && weekWorking;
    const double hours = working ? it->hours.value_or(weekHours) : 0;
    if (working == weekWorking && (!working || hours == weekHours)) continue;
    if (!m.exceptions.isEmpty()) {
      MsCalendar::Range& last = m.exceptions.last();
      if (last.to.addDays(1) == it.key() && last.working == working && last.hours == hours) {
        last.to = it.key();
        continue;
      }
    }
    m.exceptions.append({it.key(), it.key(), working, hours});
  }
  return m;
}

void writeWorkingTimes(QXmlStreamWriter& w, double hours) {
  const auto times = workingTimes(hours);
  if (times.isEmpty()) return;
  w.writeStartElement(kNsMspdi, QStringLiteral("WorkingTimes"));
  for (const auto& [from, to] : times) {
    w.writeStartElement(kNsMspdi, QStringLiteral("WorkingTime"));
    w.writeTextElement(kNsMspdi, QStringLiteral("FromTime"), hhmm(from));
    w.writeTextElement(kNsMspdi, QStringLiteral("ToTime"), hhmm(to));
    w.writeEndElement();
  }
  w.writeEndElement();
}

void writeCalendar(QXmlStreamWriter& w, const MsCalendar& c) {
  w.writeStartElement(kNsMspdi, QStringLiteral("Calendar"));
  w.writeTextElement(kNsMspdi, QStringLiteral("UID"), QString::number(c.uid));
  w.writeTextElement(kNsMspdi, QStringLiteral("Name"), xmlSafe(c.name));
  w.writeTextElement(kNsMspdi, QStringLiteral("IsBaseCalendar"), QStringLiteral("1"));
  w.writeTextElement(kNsMspdi, QStringLiteral("BaseCalendarUID"), QStringLiteral("-1"));
  w.writeStartElement(kNsMspdi, QStringLiteral("WeekDays"));
  // MSPDI DayType: 1 = Sunday ... 7 = Saturday; WorkWeek is Monday first.
  for (int dayType = 1; dayType <= 7; ++dayType) {
    const auto i = static_cast<size_t>((dayType + 5) % 7);
    const bool working = c.week.working[i] && c.week.hours[i] > 0;
    w.writeStartElement(kNsMspdi, QStringLiteral("WeekDay"));
    w.writeTextElement(kNsMspdi, QStringLiteral("DayType"), QString::number(dayType));
    w.writeTextElement(kNsMspdi, QStringLiteral("DayWorking"), working ? QStringLiteral("1") : QStringLiteral("0"));
    if (working) writeWorkingTimes(w, c.week.hours[i]);
    w.writeEndElement();
  }
  // Exceptions in the WeekDay form (DayType 0), which every MSPDI reader
  // understands.
  for (const MsCalendar::Range& r : c.exceptions) {
    w.writeStartElement(kNsMspdi, QStringLiteral("WeekDay"));
    w.writeTextElement(kNsMspdi, QStringLiteral("DayType"), QStringLiteral("0"));
    w.writeTextElement(kNsMspdi, QStringLiteral("DayWorking"), r.working ? QStringLiteral("1") : QStringLiteral("0"));
    w.writeStartElement(kNsMspdi, QStringLiteral("TimePeriod"));
    w.writeTextElement(kNsMspdi, QStringLiteral("FromDate"), r.from.toString(Qt::ISODate) + QStringLiteral("T00:00:00"));
    w.writeTextElement(kNsMspdi, QStringLiteral("ToDate"), r.to.toString(Qt::ISODate) + QStringLiteral("T23:59:00"));
    w.writeEndElement();
    if (r.working) writeWorkingTimes(w, r.hours);
    w.writeEndElement();
  }
  w.writeEndElement();  // WeekDays
  w.writeEndElement();  // Calendar
}

int msPriority(int p) {
  switch (p) {
    case 1: return 800;
    case 3: return 300;
    case 4: return 100;
    default: return 500;
  }
}

int msConstraint(const QString& c) {
  if (c == QLatin1String("SNET")) return 4;
  if (c == QLatin1String("FNLT")) return 7;
  return 0;  // ASAP
}

int msLinkType(const QString& t) {
  if (t == QLatin1String("FF")) return 0;
  if (t == QLatin1String("SS")) return 3;
  return 1;  // FS
}

}  // namespace

QByteArray exportMspdi(const ExportData& data) {
  const Project& p = data.project;
  const QList<ExportRow> rows = exportRows(data);

  // Calendars: Standard is UID 1; the project's and the tasks' others follow
  // in id order. Each is written flattened (its own week and every
  // exception of its base chain), because MS Project has no base calendars
  // of base calendars.
  QStringList calIds{QString(kStandardCalendarId)};
  {
    QStringList others;
    if (!p.calendar.isEmpty()) others << p.calendar;
    for (const ExportRow& r : rows) {
      if (!r.task.calendar.isEmpty()) others << r.task.calendar;
    }
    others.removeDuplicates();
    others.removeAll(QString(kStandardCalendarId));
    std::sort(others.begin(), others.end());
    calIds << others;
  }
  QHash<QString, int> calUid;
  QList<MsCalendar> cals;
  for (qsizetype i = 0; i < calIds.size(); ++i) {
    calUid.insert(calIds[i], static_cast<int>(i) + 1);
    cals.append(buildCalendar(data, calIds[i], static_cast<int>(i) + 1));
  }
  const QString projectCal = p.calendar.isEmpty() ? QString(kStandardCalendarId) : p.calendar;
  const MsCalendar& pc = cals[calUid.value(projectCal, 1) - 1];
  const int minutesPerDay = std::clamp(static_cast<int>(std::lround(pc.hoursPerDay * 60)), 1, 24 * 60);
  double weekHours = 0;
  for (size_t i = 0; i < 7; ++i) weekHours += pc.week.working[i] ? pc.week.hours[i] : 0;
  const int minutesPerWeek = std::max(1, static_cast<int>(std::lround(weekHours * 60)));
  const auto dayTimes = workingTimes(pc.hoursPerDay);
  const QString startTime = hhmm(dayTimes.isEmpty() ? 8 * 60 : dayTimes.first().first);
  const QString finishTime = hhmm(dayTimes.isEmpty() ? 17 * 60 : dayTimes.last().second);
  auto at = [](const QString& d, const QString& time) { return d + QLatin1Char('T') + time; };
  auto days = [&](double d) { return pt(static_cast<qint64>(std::llround(std::max(0.0, d) * minutesPerDay))); };

  QString projStart = p.start;
  QString projFinish = p.finish;
  for (const ExportRow& r : rows) {
    if (isValidDate(r.start) && (!isValidDate(projStart) || r.start < projStart)) projStart = r.start;
    if (isValidDate(r.finish) && (!isValidDate(projFinish) || r.finish > projFinish)) projFinish = r.finish;
  }
  const QDateTime exported = QDateTime::fromString(data.exportedAt, Qt::ISODateWithMs);
  const QString now = exported.isValid() ? exported.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")) : QString();

  QByteArray buf;
  QXmlStreamWriter w(&buf);
  w.setAutoFormatting(true);
  w.setAutoFormattingIndent(1);
  w.writeStartDocument(QStringLiteral("1.0"), true);
  w.writeDefaultNamespace(kNsMspdi);
  w.writeStartElement(kNsMspdi, QStringLiteral("Project"));
  // Element order follows the MSPDI schema's sequence.
  w.writeTextElement(kNsMspdi, QStringLiteral("SaveVersion"), QStringLiteral("14"));
  w.writeTextElement(kNsMspdi, QStringLiteral("Name"), xmlSafe(exportFileName(p.name, ExportFormat::Mspdi)));
  w.writeTextElement(kNsMspdi, QStringLiteral("Title"), xmlSafe(p.name));
  if (!p.owner.isEmpty()) w.writeTextElement(kNsMspdi, QStringLiteral("Manager"), xmlSafe(p.owner));
  if (!data.generator.isEmpty()) w.writeTextElement(kNsMspdi, QStringLiteral("Author"), xmlSafe(data.generator));
  if (!now.isEmpty()) {
    w.writeTextElement(kNsMspdi, QStringLiteral("CreationDate"), now);
    w.writeTextElement(kNsMspdi, QStringLiteral("LastSaved"), now);
  }
  w.writeTextElement(kNsMspdi, QStringLiteral("ScheduleFromStart"), QStringLiteral("1"));
  if (isValidDate(projStart)) w.writeTextElement(kNsMspdi, QStringLiteral("StartDate"), at(projStart, startTime));
  if (isValidDate(projFinish)) w.writeTextElement(kNsMspdi, QStringLiteral("FinishDate"), at(projFinish, finishTime));
  w.writeTextElement(kNsMspdi, QStringLiteral("CalendarUID"), QString::number(pc.uid));
  w.writeTextElement(kNsMspdi, QStringLiteral("DefaultStartTime"), startTime);
  w.writeTextElement(kNsMspdi, QStringLiteral("DefaultFinishTime"), finishTime);
  w.writeTextElement(kNsMspdi, QStringLiteral("MinutesPerDay"), QString::number(minutesPerDay));
  w.writeTextElement(kNsMspdi, QStringLiteral("MinutesPerWeek"), QString::number(minutesPerWeek));
  w.writeTextElement(kNsMspdi, QStringLiteral("DaysPerMonth"), QStringLiteral("20"));
  w.writeTextElement(kNsMspdi, QStringLiteral("DefaultTaskType"), QStringLiteral("1"));  // fixed duration
  w.writeTextElement(kNsMspdi, QStringLiteral("DurationFormat"), QStringLiteral("7"));   // days
  w.writeTextElement(kNsMspdi, QStringLiteral("WeekStartDay"), QStringLiteral("1"));     // Monday

  w.writeStartElement(kNsMspdi, QStringLiteral("Calendars"));
  for (const MsCalendar& c : cals) writeCalendar(w, c);
  w.writeEndElement();

  QHash<QString, int> uidOf;
  for (int i = 0; i < rows.size(); ++i) uidOf.insert(rows[i].task.id, i + 1);
  QHash<QString, QList<const Dependency*>> predsOf;
  for (const Dependency& d : data.dependencies) {
    if (!d.deleted && uidOf.contains(d.predecessor) && uidOf.contains(d.successor)) predsOf[d.successor].append(&d);
  }

  w.writeStartElement(kNsMspdi, QStringLiteral("Tasks"));
  // Task 0: the project summary task.
  w.writeStartElement(kNsMspdi, QStringLiteral("Task"));
  w.writeTextElement(kNsMspdi, QStringLiteral("UID"), QStringLiteral("0"));
  w.writeTextElement(kNsMspdi, QStringLiteral("ID"), QStringLiteral("0"));
  w.writeTextElement(kNsMspdi, QStringLiteral("Name"), xmlSafe(p.name));
  w.writeTextElement(kNsMspdi, QStringLiteral("Type"), QStringLiteral("1"));
  w.writeTextElement(kNsMspdi, QStringLiteral("IsNull"), QStringLiteral("0"));
  w.writeTextElement(kNsMspdi, QStringLiteral("WBS"), QStringLiteral("0"));
  w.writeTextElement(kNsMspdi, QStringLiteral("OutlineNumber"), QStringLiteral("0"));
  w.writeTextElement(kNsMspdi, QStringLiteral("OutlineLevel"), QStringLiteral("0"));
  w.writeTextElement(kNsMspdi, QStringLiteral("Priority"), QString::number(msPriority(p.priority)));
  if (isValidDate(projStart)) w.writeTextElement(kNsMspdi, QStringLiteral("Start"), at(projStart, startTime));
  if (isValidDate(projFinish)) w.writeTextElement(kNsMspdi, QStringLiteral("Finish"), at(projFinish, finishTime));
  w.writeTextElement(kNsMspdi, QStringLiteral("Summary"), QStringLiteral("1"));
  if (!p.description.isEmpty()) w.writeTextElement(kNsMspdi, QStringLiteral("Notes"), xmlSafe(p.description));
  w.writeEndElement();

  for (int i = 0; i < rows.size(); ++i) {
    const ExportRow& r = rows[i];
    const Task& t = r.task;
    const bool milestone = !r.summary && t.type == QLatin1String("milestone");
    const bool manual = !r.summary && t.mode == QLatin1String("manual");
    QString start = isValidDate(r.start) ? r.start : QString();
    QString finish = isValidDate(r.finish) ? r.finish : QString();
    if (milestone) {
      const QString d = !finish.isEmpty() ? finish : start;
      start = finish = d;
    }
    w.writeStartElement(kNsMspdi, QStringLiteral("Task"));
    w.writeTextElement(kNsMspdi, QStringLiteral("UID"), QString::number(i + 1));
    w.writeTextElement(kNsMspdi, QStringLiteral("ID"), QString::number(i + 1));
    w.writeTextElement(kNsMspdi, QStringLiteral("Name"), xmlSafe(t.title));
    w.writeTextElement(kNsMspdi, QStringLiteral("Active"), QStringLiteral("1"));
    w.writeTextElement(kNsMspdi, QStringLiteral("Manual"), manual ? QStringLiteral("1") : QStringLiteral("0"));
    w.writeTextElement(kNsMspdi, QStringLiteral("Type"), QStringLiteral("1"));
    w.writeTextElement(kNsMspdi, QStringLiteral("IsNull"), QStringLiteral("0"));
    const QDateTime created = QDateTime::fromString(t.createdAt, Qt::ISODateWithMs);
    if (created.isValid()) {
      w.writeTextElement(kNsMspdi, QStringLiteral("CreateDate"),
                         created.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")));
    }
    w.writeTextElement(kNsMspdi, QStringLiteral("WBS"), r.wbs);
    w.writeTextElement(kNsMspdi, QStringLiteral("OutlineNumber"), r.wbs);
    w.writeTextElement(kNsMspdi, QStringLiteral("OutlineLevel"), QString::number(r.level));
    w.writeTextElement(kNsMspdi, QStringLiteral("Priority"), QString::number(msPriority(t.priority)));
    if (!start.isEmpty()) w.writeTextElement(kNsMspdi, QStringLiteral("Start"), at(start, milestone ? finishTime : startTime));
    if (!finish.isEmpty()) w.writeTextElement(kNsMspdi, QStringLiteral("Finish"), at(finish, finishTime));
    if (!r.summary) {
      const QString dur = milestone ? pt(0) : days(t.duration);
      w.writeTextElement(kNsMspdi, QStringLiteral("Duration"), dur);
      if (manual) {
        if (!start.isEmpty()) w.writeTextElement(kNsMspdi, QStringLiteral("ManualStart"), at(start, startTime));
        if (!finish.isEmpty()) w.writeTextElement(kNsMspdi, QStringLiteral("ManualFinish"), at(finish, finishTime));
        w.writeTextElement(kNsMspdi, QStringLiteral("ManualDuration"), dur);
      }
      w.writeTextElement(kNsMspdi, QStringLiteral("DurationFormat"), QStringLiteral("7"));
    }
    w.writeTextElement(kNsMspdi, QStringLiteral("Milestone"), milestone ? QStringLiteral("1") : QStringLiteral("0"));
    w.writeTextElement(kNsMspdi, QStringLiteral("Summary"), r.summary ? QStringLiteral("1") : QStringLiteral("0"));
    if (t.fixedCost != 0 && std::isfinite(t.fixedCost)) {
      w.writeTextElement(kNsMspdi, QStringLiteral("FixedCost"), fmtNumber(t.fixedCost));
    }
    w.writeTextElement(kNsMspdi, QStringLiteral("PercentComplete"),
                       QString::number(std::clamp(t.percentComplete, 0, 100)));
    if (isValidDate(t.actualStart)) {
      w.writeTextElement(kNsMspdi, QStringLiteral("ActualStart"), at(t.actualStart, startTime));
    }
    if (isValidDate(t.actualFinish)) {
      w.writeTextElement(kNsMspdi, QStringLiteral("ActualFinish"), at(t.actualFinish, finishTime));
    }
    const int constraint = isValidDate(t.constraintDate) ? msConstraint(t.constraintType) : 0;
    w.writeTextElement(kNsMspdi, QStringLiteral("ConstraintType"), QString::number(constraint));
    w.writeTextElement(kNsMspdi, QStringLiteral("CalendarUID"),
                       t.calendar.isEmpty() ? QStringLiteral("-1") : QString::number(calUid.value(t.calendar, -1)));
    if (constraint == 4) {
      w.writeTextElement(kNsMspdi, QStringLiteral("ConstraintDate"), at(t.constraintDate, startTime));
    } else if (constraint == 7) {
      w.writeTextElement(kNsMspdi, QStringLiteral("ConstraintDate"), at(t.constraintDate, finishTime));
    }
    if (isValidDate(t.deadline)) w.writeTextElement(kNsMspdi, QStringLiteral("Deadline"), at(t.deadline, finishTime));
    if (!t.notes.isEmpty()) w.writeTextElement(kNsMspdi, QStringLiteral("Notes"), xmlSafe(t.notes));
    for (const Dependency* d : predsOf.value(t.id)) {
      w.writeStartElement(kNsMspdi, QStringLiteral("PredecessorLink"));
      w.writeTextElement(kNsMspdi, QStringLiteral("PredecessorUID"), QString::number(uidOf.value(d->predecessor)));
      w.writeTextElement(kNsMspdi, QStringLiteral("Type"), QString::number(msLinkType(d->type)));
      w.writeTextElement(kNsMspdi, QStringLiteral("CrossProject"), QStringLiteral("0"));
      // LinkLag is in tenths of a minute.
      const qint64 lag = static_cast<qint64>(std::clamp(d->lag, -kMaxLagDays, kMaxLagDays)) * minutesPerDay * 10;
      w.writeTextElement(kNsMspdi, QStringLiteral("LinkLag"), QString::number(lag));
      w.writeTextElement(kNsMspdi, QStringLiteral("LagFormat"), QStringLiteral("7"));
      w.writeEndElement();
    }
    w.writeEndElement();  // Task
  }
  w.writeEndElement();  // Tasks
  w.writeEmptyElement(kNsMspdi, QStringLiteral("Resources"));
  w.writeEmptyElement(kNsMspdi, QStringLiteral("Assignments"));
  w.writeEndElement();  // Project
  w.writeEndDocument();
  return buf;
}

Result<ExportData> exportDataFromJson(const QByteArray& json) {
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    return Error::validation(QStringLiteral("not a JSON object"));
  }
  const QJsonObject o = doc.object();
  if (o.value(QStringLiteral("format")).toString() != QLatin1String("zproject-export")) {
    return Error::validation(QStringLiteral("not a zproject export"));
  }
  const int version = o.value(QStringLiteral("schemaVersion")).toInt();
  if (version < 1 || version > kExportSchemaVersion) {
    return Error::validation(QStringLiteral("export schema version %1 is not supported").arg(version));
  }
  ExportData d;
  d.project = projectFromJson(o.value(QStringLiteral("project")).toObject());
  for (const QJsonValue& v : o.value(QStringLiteral("tasks")).toArray()) d.tasks.append(taskFromJson(v.toObject()));
  for (const QJsonValue& v : o.value(QStringLiteral("dependencies")).toArray()) {
    d.dependencies.append(dependencyFromJson(v.toObject()));
  }
  for (const QJsonValue& v : o.value(QStringLiteral("calendars")).toArray()) {
    CalendarDef c = calendarFromJson(v.toObject());
    for (const QJsonValue& e : v.toObject().value(QStringLiteral("exceptions")).toArray()) {
      c.exceptions.append(calendarExceptionFromJson(e.toObject()));
    }
    d.calendars.insert(c.id, c);
  }
  d.exportedAt = o.value(QStringLiteral("exportedAt")).toString();
  d.generator = o.value(QStringLiteral("generator")).toString();
  return d;
}

QByteArray exportProject(const ExportData& data, ExportFormat f) {
  switch (f) {
    case ExportFormat::Json: return exportJson(data);
    case ExportFormat::Csv: return exportCsv(data);
    case ExportFormat::Ods: return exportOds(data);
    case ExportFormat::Mspdi: return exportMspdi(data);
  }
  return {};
}

}  // namespace zproject
