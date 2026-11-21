#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;

namespace zproject::server {

// Posts alerts to ts-notify-hub (SPEC.md §7.8, decision Q19) using its client
// contract: POST <notify-hub-url>/v1/publish/<topic>, Bearer token,
// {"title", "body", "priority" 1-5, "tags"}. The hub token is read from a
// file (never from the config, the database or a log). With no URL set,
// alerts are off and every call is a no-op.
class Notifier : public QObject {
  Q_OBJECT
 public:
  struct Settings {
    QString url;    // <notify-hub-url>; empty = off
    QString topic = QStringLiteral("zproject");
    QString tokenFile;
    int timeoutMs = 10'000;
  };

  explicit Notifier(Settings settings, QObject* parent = nullptr);
  ~Notifier() override;

  bool enabled() const { return !settings_.url.isEmpty(); }

  // Sends and waits (for the one-shot admin commands). Returns false and
  // sets *error on failure; never puts the token in the error.
  bool sendBlocking(const QString& title, const QString& body, int priority, const QStringList& tags,
                    QString* error);

 private:
  Settings settings_;
  QNetworkAccessManager* net_;
};

}  // namespace zproject::server
