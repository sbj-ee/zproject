// The audit log (SPEC.md §5.1, §14): append-only through triggers, and a
// SHA-256 hash chain that --verify-audit walks. Also tokens and confirms.
#include <QTest>

#include "store/Audit.hpp"
#include "store/SqliteStore.hpp"
#include "store/Tokens.hpp"

using namespace zproject;
using namespace zproject::store;

namespace {

std::unique_ptr<SqliteStore> openMemory() {
  auto s = SqliteStore::open(QStringLiteral(":memory:"));
  if (!s.ok()) qFatal("open failed: %s", qPrintable(s.error().message));
  return std::move(s).value();
}

AuditEntry entry(int n) {
  AuditEntry e;
  e.ts = QStringLiteral("2026-11-21T12:00:00.%1Z").arg(n, 3, 10, QLatin1Char('0'));
  e.tokenId = QStringLiteral("aaaaaaaaaaaaaaaaaaaa");
  e.tokenName = QStringLiteral("agent-one");
  e.tokenKind = QStringLiteral("agent");
  e.method = QStringLiteral("PATCH");
  e.path = QStringLiteral("/v1/tasks/bbbbbbbbbbbbbbbbbbbb");
  e.entityKind = QStringLiteral("task");
  e.entityId = QStringLiteral("bbbbbbbbbbbbbbbbbbbb");
  e.before = QStringLiteral("{\"title\":\"a\"}");
  e.after = QStringLiteral("{\"title\":\"b%1\"}").arg(n);
  e.status = 200;
  e.requestId = QStringLiteral("req-%1").arg(n);
  return e;
}

}  // namespace

class TestAudit : public QObject {
  Q_OBJECT
 private slots:
  void chain() {
    auto s = openMemory();
    AuditLog log(s->database());
    QCOMPARE(log.verify().value(), 0);
    AuditEntry a = entry(1);
    QVERIFY(log.append(a).ok());
    QCOMPARE(a.prevHash, AuditLog::genesisHash());
    AuditEntry b = entry(2);
    QVERIFY(log.append(b).ok());
    QCOMPARE(b.prevHash, a.hash);
    QVERIFY(b.seq > a.seq);
    QCOMPARE(log.verify().value(), 2);
    const auto rows = log.since(0);
    QCOMPARE(rows->size(), 2);
    QCOMPARE(rows->at(1).after, QStringLiteral("{\"title\":\"b2\"}"));
    QCOMPARE(log.since(0, 10, QStringLiteral("cccccccccccccccccccc"))->size(), 0);
  }

  void appendOnly() {
    auto s = openMemory();
    AuditLog log(s->database());
    AuditEntry a = entry(1);
    QVERIFY(log.append(a).ok());
    auto upd = s->database().exec(QStringLiteral("UPDATE audit SET status = 500"));
    QVERIFY(!upd.ok());
    QVERIFY(upd.error().message.contains(QStringLiteral("append-only")));
    auto del = s->database().exec(QStringLiteral("DELETE FROM audit"));
    QVERIFY(!del.ok());
    QCOMPARE(log.verify().value(), 1);
  }

  void tamperingIsDetected() {
    auto s = openMemory();
    AuditLog log(s->database());
    for (int i = 1; i <= 3; ++i) {
      AuditEntry e = entry(i);
      QVERIFY(log.append(e).ok());
    }
    // Someone with the file drops the trigger and edits a row.
    QVERIFY(s->database().exec(QStringLiteral("DROP TRIGGER audit_no_update")).ok());
    QVERIFY(s->database().exec(QStringLiteral("UPDATE audit SET after = '{\"title\":\"x\"}' WHERE seq = 2")).ok());
    auto r = log.verify();
    QVERIFY(!r.ok());
    QCOMPARE(r.error().code, QStringLiteral("audit_broken"));
    QCOMPARE(r.error().details.value(QStringLiteral("seq")).toInteger(), qint64(2));
  }

  void removedRowIsDetected() {
    auto s = openMemory();
    AuditLog log(s->database());
    for (int i = 1; i <= 3; ++i) {
      AuditEntry e = entry(i);
      QVERIFY(log.append(e).ok());
    }
    QVERIFY(s->database().exec(QStringLiteral("DROP TRIGGER audit_no_delete")).ok());
    QVERIFY(s->database().exec(QStringLiteral("DELETE FROM audit WHERE seq = 2")).ok());
    auto r = log.verify();
    QVERIFY(!r.ok());
    QCOMPARE(r.error().details.value(QStringLiteral("seq")).toInteger(), qint64(3));
  }

  void tokensAndConfirms() {
    auto s = openMemory();
    TokenStore tokens(s->database());
    TokenRecord t;
    t.id = QStringLiteral("aaaaaaaaaaaaaaaaaaaa");
    t.createdAt = QStringLiteral("2026-11-21T12:00:00.000Z");
    t.name = QStringLiteral("agent-one");
    t.kind = QStringLiteral("agent");
    t.secretHash = QString(64, QLatin1Char('1'));
    t.scopes = {QStringLiteral("read")};
    t.projects = {QStringLiteral("bbbbbbbbbbbbbbbbbbbb")};
    t.requireConfirm = true;
    QVERIFY(tokens.insert(t).ok());
    const auto back = tokens.find(t.id);
    QVERIFY(back.ok());
    QCOMPARE(back->scopes, t.scopes);
    QCOMPARE(back->projects, t.projects);
    QVERIFY(back->requireConfirm);

    // The database itself refuses an agent token with admin.
    TokenRecord bad = t;
    bad.id = QStringLiteral("cccccccccccccccccccc");
    bad.scopes = {QStringLiteral("admin")};
    QVERIFY(!tokens.insert(bad).ok());

    const QString created = QStringLiteral("2026-11-21T12:00:00.000Z");
    const QString expires = QStringLiteral("2026-11-21T12:05:00.000Z");
    QVERIFY(tokens.addConfirm(QStringLiteral("c1"), t.id, QStringLiteral("h1"), created, expires).ok());
    QVERIFY(!tokens.consumeConfirm(QStringLiteral("c1"), t.id, QStringLiteral("h2"), created).value());
    QVERIFY(!tokens.consumeConfirm(QStringLiteral("c1"), t.id, QStringLiteral("h1"),
                                   QStringLiteral("2026-11-21T12:05:00.001Z")).value());
    QVERIFY(tokens.consumeConfirm(QStringLiteral("c1"), t.id, QStringLiteral("h1"), created).value());
    QVERIFY(!tokens.consumeConfirm(QStringLiteral("c1"), t.id, QStringLiteral("h1"), created).value());

    QVERIFY(tokens.revoke(t.id, QStringLiteral("2026-11-21T13:00:00.000Z")).ok());
    QCOMPARE(tokens.find(t.id)->revokedAt, QStringLiteral("2026-11-21T13:00:00.000Z"));
  }
};

QTEST_GUILESS_MAIN(TestAudit)
#include "tst_audit.moc"
