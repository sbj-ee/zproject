#include "ServerConfig.hpp"

namespace zproject::server {

bool parseListenAddress(const QString& text, ListenAddress* out, QString* error) {
  const QString t = text.trimmed();
  QString hostPart;
  QString portPart = t;
  const qsizetype colon = t.lastIndexOf(QLatin1Char(':'));
  if (colon >= 0) {
    hostPart = t.left(colon);
    portPart = t.mid(colon + 1);
  }
  if (hostPart.startsWith(QLatin1Char('[')) && hostPart.endsWith(QLatin1Char(']'))) {
    hostPart = hostPart.mid(1, hostPart.size() - 2);
  }

  bool ok = false;
  const uint port = portPart.toUInt(&ok);
  if (!ok || port == 0 || port > 65535) {
    if (error) *error = QStringLiteral("invalid port in '%1'").arg(text);
    return false;
  }

  QHostAddress host(QHostAddress::LocalHost);
  if (!hostPart.isEmpty()) {
    if (hostPart == QLatin1String("localhost")) {
      host = QHostAddress(QHostAddress::LocalHost);
    } else if (!host.setAddress(hostPart)) {
      if (error) *error = QStringLiteral("invalid address '%1'").arg(hostPart);
      return false;
    }
  }
  if (!host.isLoopback()) {
    if (error) {
      *error = QStringLiteral("refusing to listen on %1: only loopback addresses are allowed "
                              "(publish with `tailscale serve`)").arg(host.toString());
    }
    return false;
  }

  if (out) {
    out->host = host;
    out->port = static_cast<quint16>(port);
  }
  return true;
}

}  // namespace zproject::server
