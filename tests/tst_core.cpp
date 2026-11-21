#include <QRegularExpression>
#include <QSet>
#include <QTest>

#include "core/Id.hpp"
#include "core/Version.hpp"
#include "zproject/version.hpp"

class TstCore : public QObject {
  Q_OBJECT

 private slots:
  void versionIsSemver() {
    static const QRegularExpression semver(QStringLiteral("^\\d+\\.\\d+\\.\\d+$"));
    QVERIFY(semver.match(zproject::appVersion()).hasMatch());
    QCOMPARE(zproject::appVersion(), QStringLiteral(ZPROJECT_VERSION));
    QCOMPARE(zproject::appVersion(), QStringLiteral("%1.%2.%3")
                                         .arg(ZPROJECT_VERSION_MAJOR)
                                         .arg(ZPROJECT_VERSION_MINOR)
                                         .arg(ZPROJECT_VERSION_PATCH));
  }

  void versionLine() {
    QCOMPARE(zproject::versionLine(QStringLiteral("zprojectd")),
             QStringLiteral("zprojectd ") + zproject::appVersion());
  }

  void newIdFormat() {
    for (int i = 0; i < 100; ++i) {
      const QString id = zproject::newId();
      QCOMPARE(id.size(), zproject::kIdLength);
      QVERIFY2(zproject::isValidId(id), qPrintable(id));
    }
  }

  void newIdIsUnique() {
    QSet<QString> seen;
    for (int i = 0; i < 10000; ++i) {
      seen.insert(zproject::newId());
    }
    QCOMPARE(seen.size(), 10000);
  }

  void isValidIdRejects_data() {
    QTest::addColumn<QString>("id");
    QTest::newRow("empty") << QString();
    QTest::newRow("short") << QStringLiteral("0123456789abcdef012");
    QTest::newRow("long") << QStringLiteral("0123456789abcdef01234");
    QTest::newRow("upper") << QStringLiteral("0123456789ABCDEF0123");
    QTest::newRow("non-hex") << QStringLiteral("0123456789abcdefg123");
    QTest::newRow("dash") << QStringLiteral("01234567-9abcdef0123");
  }
  void isValidIdRejects() {
    QFETCH(QString, id);
    QVERIFY(!zproject::isValidId(id));
  }
};

QTEST_GUILESS_MAIN(TstCore)
#include "tst_core.moc"
