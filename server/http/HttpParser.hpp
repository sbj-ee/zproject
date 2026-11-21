#pragma once

#include <QByteArray>

#include "server/http/HttpTypes.hpp"

namespace zproject::server::http {

// Limits for the request parser (SPEC.md §9.1: bodies <= 1 MiB).
struct Limits {
  qsizetype maxRequestLine = 8 * 1024;
  qsizetype maxHeaderBytes = 32 * 1024;  // all header lines together
  int maxHeaders = 64;
  qsizetype maxBody = 1024 * 1024;
};

// Incremental HTTP/1.1 request parser: feed bytes as they arrive; take()
// yields complete requests. Deliberately small: no chunked request bodies,
// no obs-fold, no absolute-form targets, no HTTP/2 or upgrades. Anything
// outside that is an error with the status to answer before closing.
class Parser {
 public:
  enum class State { NeedMore, Ready, Error };

  explicit Parser(Limits limits = {}) : limits_(limits) {}

  // Appends data and parses as far as possible.
  State feed(const QByteArray& data);
  State state() const { return state_; }

  // The parsed request (state Ready). Resets for the next request on the
  // same connection; bytes already received for it are kept.
  Request take();

  int errorStatus() const { return errorStatus_; }  // 400, 413, 431, 501, 505
  const QString& errorMessage() const { return errorMessage_; }

 private:
  State fail(int status, const QString& message);
  State parse();
  bool parseHead(const QByteArray& head);

  Limits limits_;
  QByteArray buffer_;
  State state_ = State::NeedMore;
  Request current_;
  bool headDone_ = false;
  qsizetype bodyLength_ = 0;
  int errorStatus_ = 0;
  QString errorMessage_;
};

}  // namespace zproject::server::http
