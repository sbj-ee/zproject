#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>

#include <memory>
#include <optional>

#include "client/ApiError.hpp"
#include "client/ChangeRecord.hpp"
#include "core/Error.hpp"
#include "store/Database.hpp"

namespace zproject::client {

// The desktop's read-only plan cache (SPEC.md §16.5): projects, tasks, WBS
// codes, recent changes and the feed position, in SQLite (the bundled
// amalgamation). The file is 0600 in a 0700 directory and never holds a
// token. It is a copy: the server is the only source of truth, and a full
// reload rebuilds it from scratch.
//
// Entities are stored as the server's JSON. A change record's `after`
// (changed fields only) is merged into the stored object; a record whose
// `rev` is not newer than the stored one is skipped, so replaying the same
// records twice changes nothing.
class Cache {
 public:
  static constexpr int kFormat = 1;          // bump to rebuild old caches
  static constexpr int kActivityCap = 5000;  // change records kept for activity

  // Opens or creates the cache (":memory:" for tests). A cache of another
  // format is emptied.
  static Result<std::unique_ptr<Cache>> open(const QString& path);
  // Deletes the cache files (Preferences → Clear cache). The cache must be
  // closed.
  static Status removeFiles(const QString& path);

  struct ApplyResult {
    QSet<QString> projects;  // projects whose data changed
    int applied = 0;         // records that changed something
    // A record updated an entity the cache never saw created: the cache is
    // missing data and needs a full reload.
    bool incomplete = false;
  };
  // Applies feed records in one transaction and moves lastSeq to the last
  // one. Records at or below lastSeq are ignored.
  Result<ApplyResult> apply(const QList<ChangeRecord>& records);
  // Replaces every project and task (the list-polling fallback for
  // project-limited tokens). lastSeq is left alone.
  Status replaceSnapshot(const QList<QJsonObject>& projects, const QList<QJsonObject>& tasks);
  Status setWbs(const QString& projectId, const QJsonArray& items);
  // Empties the cache for a full reload (keeps nothing but the format).
  Status reset();

  Status setServer(const ServerHealth& health);
  Status markRefreshed(const QDateTime& when);

  qint64 lastSeq() const;
  QString lastSeqTs() const;  // ts of the record at lastSeq (restore check)
  int schemaVersion() const;  // the server's, at the last check; 0 if none
  QString serverVersion() const;
  QDateTime refreshedAt() const;  // invalid if never
  bool isEmpty() const;

  // Live (not deleted) entities, in the server's order.
  QList<QJsonObject> projects() const;
  std::optional<QJsonObject> project(const QString& id) const;
  QList<QJsonObject> tasks(const QString& projectId) const;
  std::optional<QJsonObject> task(const QString& id) const;
  QJsonArray wbs(const QString& projectId) const;
  // Newest first; an empty projectId means every project.
  QList<ChangeRecord> activity(const QString& projectId, int limit) const;

 private:
  explicit Cache(store::Database db) : db_(std::move(db)) {}
  Status createSchema();
  QString meta(const QString& key) const;
  Status setMeta(const QString& key, const QString& value);
  std::optional<QJsonObject> row(const QString& table, const QString& id) const;
  QList<QJsonObject> rows(const QString& sql, const QString& arg) const;

  mutable store::Database db_;
};

}  // namespace zproject::client
