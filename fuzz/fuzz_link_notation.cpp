// libFuzzer harness: the predecessor notation "3FS+2d" (core/LinkNotation;
// SPEC.md §19). Untrusted input from the CLI and, later, the desktop's link
// editor. Beyond "no crash, no sanitizer report": whatever parses must stay
// within the limits and come back identical from its canonical form.
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "core/LinkNotation.hpp"
#include "core/Model.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const QString text = QString::fromUtf8(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));
  const auto parsed = zproject::parseLinkNotation(text);
  if (!parsed) return 0;
  const QList<zproject::LinkSpec>& links = parsed.value();
  if (links.isEmpty() || links.size() > zproject::kMaxLinksInNotation) std::abort();
  QStringList canonical;
  for (const zproject::LinkSpec& l : links) {
    if (l.lag < -zproject::kMaxLagDays || l.lag > zproject::kMaxLagDays) std::abort();
    if (l.type != QLatin1String("FS") && l.type != QLatin1String("SS") && l.type != QLatin1String("FF")) std::abort();
    canonical << zproject::formatLink(l.ref, l.type, l.lag);
  }
  const auto again = zproject::parseLinkNotation(canonical.join(QLatin1Char(',')));
  if (!again || again.value() != links) std::abort();
  return 0;
}
