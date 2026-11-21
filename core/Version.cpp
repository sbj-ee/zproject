#include "Version.hpp"

#include "zproject/version.hpp"

namespace zproject {

QString appVersion() { return QStringLiteral(ZPROJECT_VERSION); }

QString versionLine(const QString& program) {
  return program + QLatin1Char(' ') + appVersion();
}

}  // namespace zproject
