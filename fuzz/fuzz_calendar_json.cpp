// libFuzzer harness: calendar and calendar-exception bodies as the API
// decodes them (calendarFromJson, calendarExceptionFromJson, validation;
// SPEC.md §19 "JSON body decoding"). Whatever validates must resolve,
// answer working-day questions without hanging, and round-trip.
#include <QJsonDocument>
#include <QJsonObject>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "core/WorkCalendar.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(
      QByteArray::fromRawData(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size)), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) return 0;
  const QJsonObject o = doc.object();

  zproject::CalendarDef c = zproject::calendarFromJson(o);
  zproject::CalendarException e = zproject::calendarExceptionFromJson(o);
  const bool calOk = zproject::validateCalendar(c).isEmpty();
  const bool excOk = zproject::validateCalendarException(e).isEmpty();
  if (calOk) {
    c.id = QStringLiteral("0123456789abcdef0123");
    c.base.clear();
    if (excOk) c.exceptions = {e};
    const auto r = zproject::WorkCalendar::resolve(c.id, {{c.id, c}});
    if (!r) std::abort();
    // A valid week has a working day, so these end (and find one).
    const QDate d(2026, 11, 23);
    if (!r->nextWorking(d).isValid() || !r->addWorkingDays(d, 5).isValid()) std::abort();
    const zproject::CalendarDef back = zproject::calendarFromJson(zproject::toJson(c));
    if (back.name != c.name || back.workWeek != c.workWeek || back.hoursPerDay != c.hoursPerDay) std::abort();
  }
  if (excOk) {
    const zproject::CalendarException back = zproject::calendarExceptionFromJson(zproject::toJson(e));
    if (back.from != e.from || back.to != e.to || back.kind != e.kind || back.hours != e.hours) std::abort();
  }
  return 0;
}
