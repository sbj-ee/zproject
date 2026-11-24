// libFuzzer harness: the project exports (SPEC.md §7.9, §19 "JSON
// import/export"). Titles, notes and the other text are untrusted (agents
// write them), and the outline may be anything: a JSON snapshot
// {project, tasks[], dependencies[], calendars[]} is decoded with the
// API's fromJson functions and run through every writer. The XML (MSPDI
// and the ODS content.xml) must always be well-formed, the JSON must
// parse, and every live task must appear once.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QXmlStreamReader>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "core/Export.hpp"

namespace {

bool wellFormed(const QByteArray& xml) {
  QXmlStreamReader r(xml);
  while (!r.atEnd()) r.readNext();
  return !r.hasError();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 64 * 1024) return 0;
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(
      QByteArray::fromRawData(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size)), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) return 0;
  const QJsonObject o = doc.object();

  zproject::ExportData d;
  d.project = zproject::projectFromJson(o.value(QStringLiteral("project")).toObject());
  for (const QJsonValue& v : o.value(QStringLiteral("tasks")).toArray()) {
    d.tasks.append(zproject::taskFromJson(v.toObject()));
  }
  for (const QJsonValue& v : o.value(QStringLiteral("dependencies")).toArray()) {
    d.dependencies.append(zproject::dependencyFromJson(v.toObject()));
  }
  for (const QJsonValue& v : o.value(QStringLiteral("calendars")).toArray()) {
    zproject::CalendarDef c = zproject::calendarFromJson(v.toObject());
    for (const QJsonValue& e : v.toObject().value(QStringLiteral("exceptions")).toArray()) {
      c.exceptions.append(zproject::calendarExceptionFromJson(e.toObject()));
    }
    d.calendars.insert(c.id, c);
  }
  d.exportedAt = o.value(QStringLiteral("exportedAt")).toString();
  d.generator = o.value(QStringLiteral("generator")).toString();

  const QList<zproject::ExportRow> rows = zproject::exportRows(d);
  QSet<QString> live;
  for (const zproject::Task& t : d.tasks) {
    if (!t.deleted && !t.id.isEmpty()) live.insert(t.id);
  }
  QSet<QString> seen;
  for (const zproject::ExportRow& r : rows) {
    if (r.level < 1 || seen.contains(r.task.id)) std::abort();
    seen.insert(r.task.id);
  }
  if (seen != live) std::abort();

  if (!wellFormed(zproject::exportMspdi(d))) std::abort();
  if (!wellFormed(zproject::odsContentXml(d))) std::abort();
  QJsonParseError jerr{};
  QJsonDocument::fromJson(zproject::exportJson(d), &jerr);
  if (jerr.error != QJsonParseError::NoError) std::abort();
  if (zproject::exportCsv(d).isEmpty()) std::abort();  // the header at least
  if (!zproject::exportOds(d).startsWith("PK\x03\x04")) std::abort();
  return 0;
}
