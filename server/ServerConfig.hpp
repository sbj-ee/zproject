#pragma once

#include <QHostAddress>
#include <QString>

namespace zproject::server {

// Where zprojectd listens (SPEC.md §9.1): 127.0.0.1:8788 by default, behind
// `tailscale serve`. Only loopback addresses are accepted: no raw LAN bind,
// never a public interface.
struct ListenAddress {
  QHostAddress host{QHostAddress::LocalHost};
  quint16 port = 8788;
};

constexpr quint16 kDefaultPort = 8788;

// Parses "HOST:PORT", ":PORT" or "PORT". Returns false and sets *error for
// anything that is not a loopback address with a port in 1..65535.
bool parseListenAddress(const QString& text, ListenAddress* out, QString* error);

}  // namespace zproject::server
