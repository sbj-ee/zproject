#include "Database.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <utility>

namespace zproject::store {
namespace {

// SQLITE_TRANSIENT without its old-style cast (-Wold-style-cast).
const sqlite3_destructor_type kTransient =
    reinterpret_cast<sqlite3_destructor_type>(static_cast<std::intptr_t>(-1));

Error sqliteError(sqlite3* db, const QString& what) {
  const char* msg = db ? sqlite3_errmsg(db) : "out of memory";
  const int code = db ? sqlite3_extended_errcode(db) : SQLITE_NOMEM;
  return {QStringLiteral("internal"),
          QStringLiteral("%1: %2").arg(what, QString::fromUtf8(msg)),
          QJsonObject{{QStringLiteral("sqlite"), code}}};
}

}  // namespace

Database::~Database() {
  if (db_) {
    sqlite3_close_v2(db_);
  }
}

Database::Database(Database&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Database& Database::operator=(Database&& other) noexcept {
  if (this != &other) {
    if (db_) sqlite3_close_v2(db_);
    db_ = std::exchange(other.db_, nullptr);
  }
  return *this;
}

Result<Database> Database::open(const QString& path) {
  sqlite3* raw = nullptr;
  const QByteArray utf8 = path.toUtf8();
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  const int rc = sqlite3_open_v2(utf8.constData(), &raw, flags, nullptr);
  Database db(raw);  // closes raw on every return path
  if (rc != SQLITE_OK) {
    return sqliteError(raw, QStringLiteral("cannot open database '%1'").arg(path));
  }
  sqlite3_extended_result_codes(raw, 1);

  const bool memory = path == QLatin1String(":memory:") || path.isEmpty();
  if (!memory) {
    auto mode = db.scalar(QStringLiteral("PRAGMA journal_mode=WAL"));
    if (!mode) return mode.error();
    if (mode->toString().compare(QLatin1String("wal"), Qt::CaseInsensitive) != 0) {
      return Error::internal(QStringLiteral("cannot enable WAL on '%1' (journal_mode=%2)")
                                 .arg(path, mode->toString()));
    }
  }
  if (auto s = db.exec(QStringLiteral("PRAGMA synchronous=NORMAL;"
                                      "PRAGMA foreign_keys=ON;"
                                      "PRAGMA busy_timeout=5000;"
                                      "PRAGMA trusted_schema=OFF;"));
      !s) {
    return s.error();
  }
  return db;
}

Status Database::exec(const QString& sql) {
  char* err = nullptr;
  const QByteArray utf8 = sql.toUtf8();
  const int rc = sqlite3_exec(db_, utf8.constData(), nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    Error e{QStringLiteral("internal"),
            QString::fromUtf8(err ? err : sqlite3_errmsg(db_)),
            QJsonObject{{QStringLiteral("sqlite"), sqlite3_extended_errcode(db_)}}};
    sqlite3_free(err);
    return e;
  }
  return Ok{};
}

Result<Statement> Database::prepare(const QString& sql) {
  sqlite3_stmt* stmt = nullptr;
  const QByteArray utf8 = sql.toUtf8();
  const int rc = sqlite3_prepare_v3(db_, utf8.constData(), static_cast<int>(utf8.size()), 0, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return sqliteError(db_, QStringLiteral("prepare failed"));
  }
  return Statement(db_, stmt);
}

Result<QVariant> Database::scalar(const QString& sql) {
  auto st = prepare(sql);
  if (!st) return st.error();
  Error err;
  if (!st.value().step(&err)) {
    if (!err.code.isEmpty()) return err;
    return QVariant();
  }
  if (st.value().isNull(0)) return QVariant();
  if (sqlite3_column_type(st.value().stmt_, 0) == SQLITE_INTEGER) {
    return QVariant::fromValue(st.value().int64(0));
  }
  return QVariant(st.value().text(0));
}

QString Database::lastErrorMessage() const {
  return db_ ? QString::fromUtf8(sqlite3_errmsg(db_)) : QString();
}

Statement::~Statement() {
  if (stmt_) sqlite3_finalize(stmt_);
}

Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
  if (this != &other) {
    if (stmt_) sqlite3_finalize(stmt_);
    db_ = std::exchange(other.db_, nullptr);
    stmt_ = std::exchange(other.stmt_, nullptr);
  }
  return *this;
}

Statement& Statement::bind(int index, const QString& value) {
  const QByteArray utf8 = value.toUtf8();
  sqlite3_bind_text(stmt_, index, utf8.constData(), static_cast<int>(utf8.size()), kTransient);
  return *this;
}

Statement& Statement::bindNullable(int index, const QString& value) {
  return value.isEmpty() ? bindNull(index) : bind(index, value);
}

Statement& Statement::bind(int index, qint64 value) {
  sqlite3_bind_int64(stmt_, index, value);
  return *this;
}

Statement& Statement::bind(int index, double value) {
  sqlite3_bind_double(stmt_, index, value);
  return *this;
}

Statement& Statement::bindNull(int index) {
  sqlite3_bind_null(stmt_, index);
  return *this;
}

bool Statement::step(Error* error) {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc != SQLITE_DONE && error) {
    *error = sqliteError(db_, QStringLiteral("statement failed"));
  }
  return false;
}

Status Statement::run() {
  Error err;
  while (step(&err)) {
  }
  if (!err.code.isEmpty()) return err;
  return Ok{};
}

void Statement::reset() {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
}

QString Statement::text(int column) const {
  const auto* p = sqlite3_column_text(stmt_, column);
  if (!p) return {};
  return QString::fromUtf8(reinterpret_cast<const char*>(p), sqlite3_column_bytes(stmt_, column));
}

qint64 Statement::int64(int column) const { return sqlite3_column_int64(stmt_, column); }
double Statement::real(int column) const { return sqlite3_column_double(stmt_, column); }
bool Statement::isNull(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

}  // namespace zproject::store
