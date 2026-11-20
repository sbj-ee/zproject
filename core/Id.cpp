#include "Id.hpp"

#include <QUuid>

namespace zproject {

QString newId() {
  // QUuid::createUuid() is a random (version 4) UUID; drop the dashes and keep
  // the first 20 hex digits (80 bits, 76 of them random).
  return QUuid::createUuid().toString(QUuid::Id128).left(kIdLength).toLower();
}

bool isValidId(const QString& id) {
  if (id.size() != kIdLength) {
    return false;
  }
  for (const QChar c : id) {
    const bool digit = c >= QLatin1Char('0') && c <= QLatin1Char('9');
    const bool hex = c >= QLatin1Char('a') && c <= QLatin1Char('f');
    if (!digit && !hex) {
      return false;
    }
  }
  return true;
}

}  // namespace zproject
