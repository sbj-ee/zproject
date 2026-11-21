#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>
#include <utility>
#include <variant>

namespace zproject {

// Machine-readable error, matching the API error shape
// {"error": message, "code": code, "details": {...}} (SPEC.md §9.3).
struct Error {
  QString code;     // validation, not_found, conflict, rev_mismatch, cycle, internal, ...
  QString message;  // human text; never a secret or a stack trace
  QJsonObject details;

  static Error validation(const QString& message, QJsonObject details = {}) {
    return {QStringLiteral("validation"), message, std::move(details)};
  }
  static Error notFound(const QString& message) {
    return {QStringLiteral("not_found"), message, {}};
  }
  static Error conflict(const QString& message, QJsonObject details = {}) {
    return {QStringLiteral("conflict"), message, std::move(details)};
  }
  static Error revMismatch(qint64 expected, qint64 actual) {
    return {QStringLiteral("rev_mismatch"),
            QStringLiteral("revision mismatch: expected %1, current is %2").arg(expected).arg(actual),
            QJsonObject{{QStringLiteral("expected"), expected}, {QStringLiteral("current"), actual}}};
  }
  static Error cycle(const QString& message, QJsonObject details = {}) {
    return {QStringLiteral("cycle"), message, std::move(details)};
  }
  static Error internal(const QString& message) {
    return {QStringLiteral("internal"), message, {}};
  }
};

// A value or an Error. Small stand-in for std::expected (C++23).
template <typename T>
class Result {
 public:
  Result(T value) : data_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : data_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  bool ok() const { return std::holds_alternative<T>(data_); }
  explicit operator bool() const { return ok(); }

  const T& value() const& { return std::get<T>(data_); }
  T& value() & { return std::get<T>(data_); }
  T&& value() && { return std::get<T>(std::move(data_)); }
  const T& operator*() const& { return value(); }
  const T* operator->() const { return &std::get<T>(data_); }

  const Error& error() const { return std::get<Error>(data_); }

 private:
  std::variant<T, Error> data_;
};

// Result for operations with no value.
struct Ok {};
using Status = Result<Ok>;

}  // namespace zproject
