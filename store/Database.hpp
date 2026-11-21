#pragma once

#include <QString>
#include <QVariant>

#include <memory>

#include "core/Error.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace zproject::store {

class Statement;

// Thin RAII wrapper around one sqlite3 connection (the bundled amalgamation,
// not QtSql; SPEC.md §4.3).
class Database {
 public:
  Database() = default;
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&& other) noexcept;
  Database& operator=(Database&& other) noexcept;

  // Opens (creating if needed) a database file, or ":memory:". Applies the
  // connection settings of SPEC.md §16.1: WAL (files only), synchronous=NORMAL,
  // foreign_keys=ON, busy_timeout=5000.
  static Result<Database> open(const QString& path);
  // Opens an existing file read-only with no pragmas that write (for checking
  // a backup copy without turning it into a WAL database).
  static Result<Database> openReadOnly(const QString& path);

  bool isOpen() const { return db_ != nullptr; }
  sqlite3* handle() const { return db_; }

  // Runs one or more statements with no results.
  Status exec(const QString& sql);
  Result<Statement> prepare(const QString& sql);

  // Convenience: first column of the first row, or a null QVariant.
  Result<QVariant> scalar(const QString& sql);

  QString lastErrorMessage() const;

 private:
  explicit Database(sqlite3* db) : db_(db) {}
  sqlite3* db_ = nullptr;
};

// A prepared statement. Bind parameters by 1-based index; step() returns true
// while there is a row.
class Statement {
 public:
  ~Statement();
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&& other) noexcept;
  Statement& operator=(Statement&& other) noexcept;

  Statement& bind(int index, const QString& value);      // empty string binds ''
  Statement& bindNullable(int index, const QString& value);  // empty string binds NULL
  Statement& bind(int index, qint64 value);
  Statement& bind(int index, int value) { return bind(index, static_cast<qint64>(value)); }
  Statement& bind(int index, double value);
  Statement& bind(int index, bool value) { return bind(index, static_cast<qint64>(value ? 1 : 0)); }
  Statement& bindNull(int index);

  // true: a row is available; false: done. Errors are reported in *error.
  bool step(Error* error = nullptr);
  // Runs to completion (for INSERT/UPDATE); returns an error on failure.
  Status run();
  void reset();

  QString text(int column) const;
  qint64 int64(int column) const;
  double real(int column) const;
  bool isNull(int column) const;

 private:
  friend class Database;
  Statement(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

// A write transaction, rolled back unless commit() succeeded. The outermost
// one is BEGIN IMMEDIATE ... COMMIT; one opened inside another becomes a
// SAVEPOINT, so the server can wrap a store write and its audit row in one
// transaction (SPEC.md §16.1).
class Transaction {
 public:
  explicit Transaction(Database& db);
  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  bool ok() const { return active_; }
  const Error& error() const { return error_; }
  Status commit();

 private:
  Database& db_;
  Error error_;
  QString savepoint_;  // empty for the outermost transaction
  bool active_ = false;
};

}  // namespace zproject::store
