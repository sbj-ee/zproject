// Project exports (SPEC.md §7.9; core/Export.hpp): the outline rows, CSV,
// ODS (a real zip, read back here), MSPDI and JSON, plus the zip writer.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QXmlStreamReader>
#include <QtEndian>
#include <QtTest>

#include "core/Export.hpp"
#include "core/Zip.hpp"

using namespace zproject;

namespace {

Task task(const QString& id, const QString& title, qint64 order, const QString& parent = {}) {
  Task t;
  t.id = id;
  t.project = QStringLiteral("p1");
  t.title = title;
  t.order = order;
  t.parent = parent;
  return t;
}

Dependency link(const QString& id, const QString& pred, const QString& succ, const QString& type = QStringLiteral("FS"),
                int lag = 0) {
  Dependency d;
  d.id = id;
  d.project = QStringLiteral("p1");
  d.predecessor = pred;
  d.successor = succ;
  d.type = type;
  d.lag = lag;
  return d;
}

// Design (summary) > Spec, Sign-off (milestone); Build; a deleted task.
ExportData sample() {
  ExportData d;
  d.project.id = QStringLiteral("p1");
  d.project.name = QStringLiteral("Office move");
  d.project.owner = QStringLiteral("Sam");
  d.project.start = QStringLiteral("2026-01-05");
  d.project.description = QStringLiteral("Move to the new office.");
  d.exportedAt = QStringLiteral("2026-11-24T15:04:05.000Z");
  d.generator = QStringLiteral("zprojectd test");

  Task design = task(QStringLiteral("t1"), QStringLiteral("Design"), 1);
  Task spec = task(QStringLiteral("t2"), QStringLiteral("Spec, \"draft\""), 1, QStringLiteral("t1"));
  spec.duration = 3;
  spec.start = QStringLiteral("2026-01-05");
  spec.finish = QStringLiteral("2026-01-07");
  spec.percentComplete = 100;
  spec.status = QStringLiteral("done");
  Task signoff = task(QStringLiteral("t3"), QStringLiteral("=SUM(A1)"), 2, QStringLiteral("t1"));
  signoff.type = QStringLiteral("milestone");
  signoff.duration = 0;
  signoff.start = signoff.finish = QStringLiteral("2026-01-07");
  Task build = task(QStringLiteral("t4"), QStringLiteral("Build\x01 it"), 2);
  build.duration = 2.5;
  build.start = QStringLiteral("2026-01-08");
  build.finish = QStringLiteral("2026-01-12");
  build.percentComplete = 40;
  build.priority = 1;
  build.notes = QStringLiteral("line one\nline  two");
  build.constraintType = QStringLiteral("SNET");
  build.constraintDate = QStringLiteral("2026-01-08");
  build.deadline = QStringLiteral("2026-01-15");
  Task gone = task(QStringLiteral("t5"), QStringLiteral("Deleted"), 3);
  gone.deleted = true;
  // Out of order on purpose: exportRows sorts.
  d.tasks = {build, signoff, gone, spec, design};
  d.dependencies = {link(QStringLiteral("d1"), QStringLiteral("t2"), QStringLiteral("t3")),
                    link(QStringLiteral("d2"), QStringLiteral("t3"), QStringLiteral("t4"), QStringLiteral("SS"), 2),
                    link(QStringLiteral("d3"), QStringLiteral("t5"), QStringLiteral("t4"))};  // to a deleted task

  CalendarDef standard;
  standard.id = QString(kStandardCalendarId);
  standard.name = QStringLiteral("Standard");
  CalendarException xmas;
  xmas.calendar = standard.id;
  xmas.from = QDate(2026, 12, 24);
  xmas.to = QDate(2026, 12, 25);
  xmas.label = QStringLiteral("Christmas");
  CalendarException shortDay;
  shortDay.calendar = standard.id;
  shortDay.from = shortDay.to = QDate(2026, 12, 31);
  shortDay.kind = QStringLiteral("custom_hours");
  shortDay.hours = 4;
  standard.exceptions = {xmas, shortDay};
  d.calendars.insert(standard.id, standard);
  return d;
}

// ---- a tiny zip reader, enough to check ZipWriter's output

quint32 adler32(const QByteArray& data) {
  quint32 a = 1;
  quint32 b = 0;
  for (const char c : data) {
    a = (a + static_cast<quint8>(c)) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16) | a;
}

struct ZipEntry {
  QString name;
  quint16 method = 0;
  quint32 crc = 0;
  quint32 size = 0;
  QByteArray raw;  // as stored in the archive
};

QList<ZipEntry> readZip(const QByteArray& z) {
  auto u16 = [&](qsizetype at) { return qFromLittleEndian<quint16>(z.constData() + at); };
  auto u32 = [&](qsizetype at) { return qFromLittleEndian<quint32>(z.constData() + at); };
  QList<ZipEntry> out;
  const qsizetype eocd = z.size() - 22;
  if (eocd < 0 || u32(eocd) != 0x06054b50U) return out;
  const int count = u16(eocd + 10);
  qsizetype cd = u32(eocd + 16);
  for (int i = 0; i < count; ++i) {
    if (u32(cd) != 0x02014b50U) return {};
    ZipEntry e;
    e.method = u16(cd + 10);
    e.crc = u32(cd + 16);
    const quint32 csize = u32(cd + 20);
    e.size = u32(cd + 24);
    const quint16 nameLen = u16(cd + 28);
    const quint32 local = u32(cd + 42);
    e.name = QString::fromLatin1(z.mid(cd + 46, nameLen));
    if (u32(local) != 0x04034b50U || u32(local + 14) != e.crc) return {};
    const qsizetype dataAt = local + 30 + u16(local + 26) + u16(local + 28);
    e.raw = z.mid(dataAt, csize);
    out.append(e);
    cd += 46 + nameLen + u16(cd + 30) + u16(cd + 32);
  }
  return out;
}

// True when the entry holds exactly `expected`: stored bytes compare
// directly; deflated ones are inflated by zlib (qUncompress), rewrapped
// with the zlib header and the Adler-32 of the expected bytes, which zlib
// checks.
bool holds(const ZipEntry& e, const QByteArray& expected) {
  if (e.size != static_cast<quint32>(expected.size()) || e.crc != crc32(expected)) return false;
  if (e.method == 0) return e.raw == expected;
  if (e.method != 8) return false;
  QByteArray wrapped(4, '\0');
  qToBigEndian<quint32>(e.size, wrapped.data());
  wrapped.append("\x78\x9c", 2);
  wrapped.append(e.raw);
  char adler[4];
  qToBigEndian<quint32>(adler32(expected), adler);
  wrapped.append(adler, 4);
  return qUncompress(wrapped) == expected;
}

}  // namespace

class TestExport : public QObject {
  Q_OBJECT

 private slots:
  void formats();
  void fileNames();
  void xmlSafeDropsForbiddenCharacters();
  void rowsOutlineAndPredecessors();
  void rowsSurviveBrokenInput();
  void csv();
  void zipWriterRoundTrip();
  void ods();
  void mspdi();
  void json();
  void deterministic();
};

void TestExport::formats() {
  QCOMPARE(parseExportFormat(QStringLiteral("CSV")), std::optional(ExportFormat::Csv));
  QCOMPARE(parseExportFormat(QStringLiteral("mspdi")), std::optional(ExportFormat::Mspdi));
  QCOMPARE(parseExportFormat(QStringLiteral("pdf")), std::nullopt);
  for (ExportFormat f : {ExportFormat::Json, ExportFormat::Csv, ExportFormat::Ods, ExportFormat::Mspdi}) {
    QCOMPARE(parseExportFormat(exportFormatName(f)), std::optional(f));
  }
  QCOMPARE(exportExtension(ExportFormat::Mspdi), QStringLiteral("xml"));
  QCOMPARE(exportContentType(ExportFormat::Ods), QByteArray("application/vnd.oasis.opendocument.spreadsheet"));
}

void TestExport::fileNames() {
  QCOMPARE(exportFileName(QStringLiteral("Office move / 2026"), ExportFormat::Ods), QStringLiteral("Office-move-2026.ods"));
  QCOMPARE(exportFileName(QStringLiteral("../../etc/passwd"), ExportFormat::Csv), QStringLiteral("etc-passwd.csv"));
  QCOMPARE(exportFileName(QStringLiteral("-rf"), ExportFormat::Json), QStringLiteral("rf.json"));
  QCOMPARE(exportFileName(QString(), ExportFormat::Mspdi), QStringLiteral("project.xml"));
  QCOMPARE(exportFileName(QStringLiteral("Ünïcode"), ExportFormat::Csv), QStringLiteral("n-code.csv"));
  QCOMPARE(exportFileName(QString(300, QLatin1Char('a')), ExportFormat::Csv).size(), 84);
}

void TestExport::xmlSafeDropsForbiddenCharacters() {
  QString s = QStringLiteral("a\x01" "b\tc\nd");
  s += QChar(0xFFFE);
  s += QChar(0xD800);  // lone high surrogate
  s += QStringLiteral("e");
  s += QChar(0xDC00);  // lone low surrogate
  s += QString::fromUcs4(U"\U0001F600");
  QCOMPARE(xmlSafe(s), QStringLiteral("ab\tc\nde") + QString::fromUcs4(U"\U0001F600"));
}

void TestExport::rowsOutlineAndPredecessors() {
  const QList<ExportRow> rows = exportRows(sample());
  QCOMPARE(rows.size(), 4);
  QStringList wbs;
  QStringList ids;
  for (const ExportRow& r : rows) {
    wbs << r.wbs;
    ids << r.task.id;
  }
  QCOMPARE(wbs, QStringList({"1", "1.1", "1.2", "2"}));
  QCOMPARE(ids, QStringList({"t1", "t2", "t3", "t4"}));
  QCOMPARE(rows[0].level, 1);
  QCOMPARE(rows[1].level, 2);
  QVERIFY(rows[0].summary);
  QVERIFY(!rows[1].summary);
  // The summary's span comes from its children.
  QCOMPARE(rows[0].start, QStringLiteral("2026-01-05"));
  QCOMPARE(rows[0].finish, QStringLiteral("2026-01-07"));
  QCOMPARE(rows[2].predecessors, QStringLiteral("1.1"));
  // The link from the deleted task is gone.
  QCOMPARE(rows[3].predecessors, QStringLiteral("1.2SS+2d"));
}

void TestExport::rowsSurviveBrokenInput() {
  ExportData d;
  // A parent cycle (a <-> b), a self-parent, a missing parent, a duplicate
  // id, and a 5,000-deep chain (no recursion).
  d.tasks << task(QStringLiteral("a"), QStringLiteral("A"), 1, QStringLiteral("b"))
          << task(QStringLiteral("b"), QStringLiteral("B"), 2, QStringLiteral("a"))
          << task(QStringLiteral("s"), QStringLiteral("Self"), 3, QStringLiteral("s"))
          << task(QStringLiteral("o"), QStringLiteral("Orphan"), 4, QStringLiteral("missing"))
          << task(QStringLiteral("o"), QStringLiteral("Duplicate"), 5);
  QString parent;
  for (int i = 0; i < 5000; ++i) {
    const QString id = QStringLiteral("deep%1").arg(i);
    d.tasks << task(id, id, 0, parent);
    parent = id;
  }
  const QList<ExportRow> rows = exportRows(d);
  QCOMPARE(rows.size(), 4 + 5000);
  QSet<QString> seen;
  for (const ExportRow& r : rows) seen.insert(r.task.id);
  QCOMPARE(seen.size(), rows.size());  // each task once
  int deepest = 0;
  for (const ExportRow& r : rows) deepest = std::max(deepest, r.level);
  QCOMPARE(deepest, 5000);
  // The cycle is broken at its first task (by order), after the real roots.
  QCOMPARE(rows[rows.size() - 2].task.id, QStringLiteral("a"));
  QCOMPARE(rows.last().task.id, QStringLiteral("b"));
  QCOMPARE(rows.last().level, 2);
  // Every writer copes too.
  for (ExportFormat f : {ExportFormat::Json, ExportFormat::Csv, ExportFormat::Ods, ExportFormat::Mspdi}) {
    QVERIFY(!exportProject(d, f).isEmpty());
  }
}

void TestExport::csv() {
  const QString csv = QString::fromUtf8(exportCsv(sample()));
  QVERIFY(csv.endsWith(QLatin1String("\r\n")));
  const QStringList lines = csv.split(QStringLiteral("\r\n"));
  QVERIFY(lines[0].startsWith(QLatin1String("WBS,Level,Title,Type,Mode,Duration (days),Start,Finish,Predecessors,")));
  // Summary: no duration; dates are the span.
  QVERIFY2(lines[1].startsWith(QLatin1String("1,1,Design,summary,auto,,2026-01-05,2026-01-07,,0,")), qPrintable(lines[1]));
  // Quotes doubled; the field is quoted.
  QVERIFY2(lines[2].startsWith(QLatin1String("1.1,2,\"Spec, \"\"draft\"\"\",task,auto,3,")), qPrintable(lines[2]));
  // A formula-looking title is defused with an apostrophe.
  QVERIFY2(lines[3].startsWith(QLatin1String("1.2,2,'=SUM(A1),milestone,auto,0,2026-01-07,2026-01-07,1.1,")),
           qPrintable(lines[3]));
  // 2.5 days, 40 %, the notes' newline kept inside quotes.
  QVERIFY(csv.contains(QLatin1String(",2.5,2026-01-08,2026-01-12,1.2SS+2d,40,todo,1,SNET,2026-01-08,2026-01-15,,,0,\"line one\nline  two\",t4")));
  QVERIFY(!csv.contains(QLatin1String("Deleted")));
}

void TestExport::zipWriterRoundTrip() {
  QCOMPARE(crc32("123456789"), 0xCBF43926U);  // the standard check value
  ZipWriter z(QDateTime(QDate(2026, 11, 24), QTime(15, 4, 6), QTimeZone::utc()));
  z.add(QStringLiteral("stored"), "hello", false);
  z.add(QStringLiteral("dir/big.txt"), QByteArray(5000, 'x'));
  z.add(QStringLiteral("empty"), QByteArray());
  const QByteArray zip = z.finish();
  const QList<ZipEntry> e = readZip(zip);
  QCOMPARE(e.size(), 3);
  QCOMPARE(e[0].name, QStringLiteral("stored"));
  QCOMPARE(e[0].method, quint16(0));
  QVERIFY(holds(e[0], "hello"));
  QCOMPARE(e[1].name, QStringLiteral("dir/big.txt"));
  QCOMPARE(e[1].method, quint16(8));  // deflated: it got smaller
  QVERIFY(holds(e[1], QByteArray(5000, 'x')));
  QCOMPARE(e[2].method, quint16(0));
  QVERIFY(holds(e[2], QByteArray()));
}

void TestExport::ods() {
  const QByteArray ods = exportOds(sample());
  // ODF: "mimetype" is the first entry, stored, at offset 30 + 8.
  QCOMPARE(ods.mid(30, 8), QByteArray("mimetype"));
  QCOMPARE(ods.mid(38, 46), QByteArray("application/vnd.oasis.opendocument.spreadsheet"));
  const QList<ZipEntry> e = readZip(ods);
  QStringList names;
  for (const ZipEntry& x : e) names << x.name;
  QCOMPARE(names, QStringList({"mimetype", "content.xml", "styles.xml", "meta.xml", "META-INF/manifest.xml"}));
  QCOMPARE(e[0].method, quint16(0));
  const ExportData d = sample();
  const QByteArray content = odsContentXml(d);
  QVERIFY(holds(e[1], content));

  // content.xml: three sheets; typed cells; the header row repeats.
  QXmlStreamReader r(content);
  QStringList sheets;
  int headerRows = 0;
  QStringList dates;
  QStringList percents;
  QStringList texts;
  bool inP = false;
  QString para;
  while (!r.atEnd()) {
    r.readNext();
    if (r.isStartElement()) {
      const auto a = r.attributes();
      if (r.name() == QLatin1String("table")) sheets << a.value(QStringLiteral("table:name")).toString();
      if (r.name() == QLatin1String("table-header-rows")) ++headerRows;
      if (r.name() == QLatin1String("table-cell")) {
        if (a.value(QStringLiteral("office:value-type")) == QLatin1String("date")) {
          dates << a.value(QStringLiteral("office:date-value")).toString();
        }
        if (a.value(QStringLiteral("office:value-type")) == QLatin1String("percentage")) {
          percents << a.value(QStringLiteral("office:value")).toString();
        }
      }
      if (r.name() == QLatin1String("p")) {
        inP = true;
        para.clear();
      }
      if (inP && r.name() == QLatin1String("s")) {
        para += QString(qMax(1, a.value(QStringLiteral("text:c")).toInt()), QLatin1Char(' '));
      }
    } else if (r.isCharacters() && inP) {
      para += r.text();
    } else if (r.isEndElement() && r.name() == QLatin1String("p")) {
      inP = false;
      texts << para;
    }
  }
  QVERIFY2(!r.hasError(), qPrintable(r.errorString()));
  QCOMPARE(sheets, QStringList({"Tasks", "Dependencies", "Project"}));
  QCOMPARE(headerRows, 3);
  QVERIFY(dates.contains(QStringLiteral("2026-01-08")));
  QCOMPARE(percents, QStringList({"0", "1", "0", "0.4"}));
  // No CSV apostrophe in ODS (a string cell is never a formula); notes keep
  // their lines and both spaces.
  QVERIFY(texts.contains(QStringLiteral("=SUM(A1)")));
  QVERIFY(texts.contains(QStringLiteral("line one")));
  QVERIFY(texts.contains(QStringLiteral("line  two")));
  QVERIFY(texts.contains(QStringLiteral("Build it")));
}

void TestExport::mspdi() {
  const QByteArray xml = exportMspdi(sample());
  QXmlStreamReader r(xml);
  QStringList taskNames;
  QStringList outline;
  QList<QPair<QString, QString>> links;  // (type, lag)
  QString calendarName;
  QStringList exceptionDates;
  QString element;
  QString inside;  // Task, PredecessorLink, Calendar, TimePeriod
  QString linkType;
  bool exception = false;
  while (!r.atEnd()) {
    r.readNext();
    if (r.isStartElement()) {
      QCOMPARE(r.namespaceUri().toString(), QStringLiteral("http://schemas.microsoft.com/project"));
      element = r.name().toString();
      if (element == QLatin1String("Task") || element == QLatin1String("PredecessorLink") ||
          element == QLatin1String("Calendar")) {
        inside = element;
      }
      if (element == QLatin1String("WeekDay")) exception = false;
    } else if (r.isCharacters() && !r.isWhitespace()) {
      const QString t = r.text().toString();
      if (inside == QLatin1String("Task") && element == QLatin1String("Name")) taskNames << t;
      if (inside == QLatin1String("Task") && element == QLatin1String("OutlineNumber")) outline << t;
      if (inside == QLatin1String("PredecessorLink") && element == QLatin1String("Type")) linkType = t;
      if (inside == QLatin1String("PredecessorLink") && element == QLatin1String("LinkLag")) links.append({linkType, t});
      if (inside == QLatin1String("Calendar") && element == QLatin1String("Name")) calendarName = t;
      if (element == QLatin1String("DayType") && t == QLatin1String("0")) exception = true;
      if (exception && element == QLatin1String("FromDate")) exceptionDates << t;
    } else if (r.isEndElement()) {
      if (r.name() == QLatin1String("PredecessorLink")) inside = QStringLiteral("Task");
    }
  }
  QVERIFY2(!r.hasError(), qPrintable(r.errorString()));
  // Task 0 is the project; the control character in "Build\x01 it" is gone.
  QCOMPARE(taskNames, QStringList({"Office move", "Design", "Spec, \"draft\"", "=SUM(A1)", "Build it"}));
  QCOMPARE(outline, QStringList({"0", "1", "1.1", "1.2", "2"}));
  // FS (1) with no lag; SS (3) + 2 days = 2 × 480 min × 10.
  QCOMPARE(links, (QList<QPair<QString, QString>>{{"1", "0"}, {"3", "9600"}}));
  QCOMPARE(calendarName, QStringLiteral("Standard"));
  QCOMPARE(exceptionDates, QStringList({"2026-12-24T00:00:00", "2026-12-31T00:00:00"}));
  QVERIFY(xml.contains("<Duration>PT20H0M0S</Duration>"));  // 2.5 days
  QVERIFY(xml.contains("<Milestone>1</Milestone>"));
  QVERIFY(xml.contains("<Priority>800</Priority>"));
  QVERIFY(xml.contains("<ConstraintType>4</ConstraintType>"));
  QVERIFY(xml.contains("<ConstraintDate>2026-01-08T08:00:00</ConstraintDate>"));
  QVERIFY(xml.contains("<Deadline>2026-01-15T17:00:00</Deadline>"));
  QVERIFY(xml.contains("<PercentComplete>40</PercentComplete>"));
}

void TestExport::json() {
  const QJsonObject o = QJsonDocument::fromJson(exportJson(sample())).object();
  QCOMPARE(o.value(QStringLiteral("format")).toString(), QStringLiteral("zproject-export"));
  QCOMPARE(o.value(QStringLiteral("schemaVersion")).toInt(), kExportSchemaVersion);
  QCOMPARE(o.value(QStringLiteral("project")).toObject().value(QStringLiteral("name")).toString(),
           QStringLiteral("Office move"));
  const QJsonArray tasks = o.value(QStringLiteral("tasks")).toArray();
  QCOMPARE(tasks.size(), 4);
  QCOMPARE(tasks[3].toObject().value(QStringLiteral("wbs")).toString(), QStringLiteral("2"));
  // Full fidelity: the task decodes back to the same fields.
  const Task back = taskFromJson(tasks[3].toObject());
  QCOMPARE(back.notes, QStringLiteral("line one\nline  two"));
  QCOMPARE(back.duration, 2.5);
  QCOMPARE(o.value(QStringLiteral("dependencies")).toArray().size(), 2);
  const QJsonArray cals = o.value(QStringLiteral("calendars")).toArray();
  QCOMPARE(cals.size(), 1);
  QCOMPARE(cals[0].toObject().value(QStringLiteral("exceptions")).toArray().size(), 2);
}

void TestExport::deterministic() {
  for (ExportFormat f : {ExportFormat::Json, ExportFormat::Csv, ExportFormat::Ods, ExportFormat::Mspdi}) {
    QCOMPARE(exportProject(sample(), f), exportProject(sample(), f));
  }
}

QTEST_GUILESS_MAIN(TestExport)
#include "tst_export.moc"
