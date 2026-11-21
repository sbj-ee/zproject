#pragma once

#include <QList>
#include <QString>

#include "core/Error.hpp"
#include "store/Database.hpp"

namespace zproject::store {

// One row of the append-only, hash-chained `audit` table (SPEC.md §5.1, §14).
// `before`/`after` are compact JSON text (empty = NULL). Empty strings are
// stored as NULL.
struct AuditEntry {
  qint64 seq = 0;
  QString ts;
  QString tokenId;
  QString tokenName;
  QString tokenKind;
  QString tsUser;  // Tailscale-User-Login from tailscale serve
  QString tsNode;  // the forwarding tailnet peer
  QString method;
  QString path;
  QString mcpTool;
  QString entityKind;
  QString entityId;
  QString before;
  QString after;
  bool dryRun = false;
  QString confirmId;
  int status = 0;
  QString requestId;
  QString prevHash;
  QString hash;
};

// The audit log. Rows can only be appended: triggers refuse UPDATE and
// DELETE. Each row's hash is SHA-256 over its fields and the previous row's
// hash, so a removed, reordered or edited row breaks the chain.
class AuditLog {
 public:
  explicit AuditLog(Database& db) : db_(db) {}

  // Fills in prevHash and hash, inserts the row and sets entry.seq.
  Status append(AuditEntry& entry);
  // Rows with seq > since, oldest first; tokenId filters when not empty.
  Result<QList<AuditEntry>> since(qint64 since, int limit = 500, const QString& tokenId = {});
  // Walks the whole chain. Returns the number of rows, or an error naming the
  // first row that does not match.
  Result<qint64> verify();

  // 64 zeros: the prevHash of the first row.
  static QString genesisHash();
  static QString computeHash(const AuditEntry& e);

 private:
  Database& db_;
};

}  // namespace zproject::store
