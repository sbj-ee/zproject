// libFuzzer harness: a dependency body as the API decodes it (JSON parse,
// dependencyFromJson, validateDependency; SPEC.md §19 "JSON body
// decoding"). Whatever validates must be a link the store can take: FS, SS
// or FF, two different non-empty task ids, a lag within ±3650.
#include <QJsonDocument>
#include <QJsonObject>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "core/Model.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  QJsonParseError err{};
  const QJsonDocument doc =
      QJsonDocument::fromJson(QByteArray::fromRawData(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size)), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) return 0;
  const zproject::Dependency d = zproject::dependencyFromJson(doc.object());
  if (!zproject::validateDependency(d).isEmpty()) return 0;
  if (d.predecessor.isEmpty() || d.successor.isEmpty() || d.predecessor == d.successor) std::abort();
  if (d.lag < -zproject::kMaxLagDays || d.lag > zproject::kMaxLagDays) std::abort();
  if (d.type != QLatin1String("FS") && d.type != QLatin1String("SS") && d.type != QLatin1String("FF")) std::abort();
  // And it survives the round trip the store's change rows make.
  const zproject::Dependency back = zproject::dependencyFromJson(zproject::toJson(d));
  if (back.predecessor != d.predecessor || back.successor != d.successor || back.type != d.type || back.lag != d.lag) {
    std::abort();
  }
  return 0;
}
