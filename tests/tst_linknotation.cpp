// The predecessor notation "3FS+2d" (scheduling plan S3; SPEC.md §10, §19):
// what it accepts, what it refuses and where, the canonical form, and
// random input (the fuzz harness's job in depth; here, a quick sweep that
// runs everywhere).
#include <QRandomGenerator>
#include <QTest>

#include "core/LinkNotation.hpp"
#include "core/Model.hpp"

using namespace zproject;

namespace {

LinkSpec spec(const char* ref, const char* type = "FS", int lag = 0, bool isId = false) {
  LinkSpec s;
  s.ref = QLatin1String(ref);
  s.type = QLatin1String(type);
  s.lag = lag;
  s.isId = isId;
  return s;
}

QList<LinkSpec> ok(const char* text) {
  auto r = parseLinkNotation(QString::fromUtf8(text));
  if (!r) qWarning("%s: %s", text, qPrintable(r.error().message));
  return r ? r.value() : QList<LinkSpec>{};
}

}  // namespace

class TestLinkNotation : public QObject {
  Q_OBJECT

 private slots:
  void accepts_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<QList<LinkSpec>>("expected");
    QTest::newRow("plain") << "3" << QList<LinkSpec>{spec("3")};
    QTest::newRow("wbs") << "1.2.4" << QList<LinkSpec>{spec("1.2.4")};
    QTest::newRow("type") << "3SS" << QList<LinkSpec>{spec("3", "SS")};
    QTest::newRow("lower") << "3ff" << QList<LinkSpec>{spec("3", "FF")};
    QTest::newRow("lag") << "3FS+2d" << QList<LinkSpec>{spec("3", "FS", 2)};
    QTest::newRow("lead") << "1.2FF-1d" << QList<LinkSpec>{spec("1.2", "FF", -1)};
    QTest::newRow("no d") << "3SS+5" << QList<LinkSpec>{spec("3", "SS", 5)};
    QTest::newRow("lag, no type") << "3+2d" << QList<LinkSpec>{spec("3", "FS", 2)};
    QTest::newRow("spaces") << "  1.2 SS + 1 d , 4 " << QList<LinkSpec>{spec("1.2", "SS", 1), spec("4")};
    QTest::newRow("list") << "3,4FS+2d,5SS" << QList<LinkSpec>{spec("3"), spec("4", "FS", 2), spec("5", "SS")};
    QTest::newRow("id") << "0123456789abcdef0123"
                        << QList<LinkSpec>{spec("0123456789abcdef0123", "FS", 0, true)};
    QTest::newRow("id, type") << "0123456789abcdef0123SS+1d"
                              << QList<LinkSpec>{spec("0123456789abcdef0123", "SS", 1, true)};
    // An id ending in hex letters, then a type in lower case.
    QTest::newRow("id, lower type") << "0123456789abcdef0123ff"
                                    << QList<LinkSpec>{spec("0123456789abcdef0123", "FF", 0, true)};
    QTest::newRow("max lag") << "2FS+3650d" << QList<LinkSpec>{spec("2", "FS", 3650)};
  }
  void accepts() {
    QFETCH(QString, text);
    QFETCH(QList<LinkSpec>, expected);
    auto r = parseLinkNotation(text);
    QVERIFY2(r.ok(), qPrintable(r ? QString() : r.error().message));
    QCOMPARE(r.value(), expected);
  }

  void refuses_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("position");
    QTest::newRow("empty") << "" << 0;
    QTest::newRow("blank") << "   " << 3;
    QTest::newRow("letters") << "abc" << 0;
    QTest::newRow("zero") << "0" << 0;
    QTest::newRow("leading zero") << "1.02" << 2;
    QTest::newRow("trailing dot") << "1." << 2;
    QTest::newRow("bad type") << "3XY" << 1;
    QTest::newRow("SF") << "3SF" << 1;
    QTest::newRow("sign, no days") << "3FS+" << 4;
    QTest::newRow("lag too big") << "3FS+3651d" << 8;
    QTest::newRow("lag too long") << "3FS+1234567" << 10;
    QTest::newRow("trailing comma") << "3," << 2;
    QTest::newRow("junk after") << "3FS+2dx" << 6;
    QTest::newRow("21 hex") << "0123456789abcdef01234" << 0;
    QTest::newRow("wbs part too long") << "1234567890" << 0;
  }
  void refuses() {
    QFETCH(QString, text);
    QFETCH(int, position);
    auto r = parseLinkNotation(text);
    QVERIFY(!r.ok());
    QCOMPARE(r.error().code, QStringLiteral("validation"));
    QCOMPARE(r.error().details.value(QStringLiteral("field")).toString(), QStringLiteral("links"));
    QCOMPARE(r.error().details.value(QStringLiteral("position")).toInt(), position);
  }

  void limits() {
    QString many;
    for (int i = 0; i <= kMaxLinksInNotation; ++i) many += QStringLiteral("%1,").arg(i + 1);
    many.chop(1);
    QVERIFY(!parseLinkNotation(many).ok());
    QVERIFY(!parseLinkNotation(QString(kMaxLinkNotationLength + 1, QLatin1Char('1'))).ok());
    QString deep = QStringLiteral("1");
    for (int i = 0; i < 25; ++i) deep += QStringLiteral(".1");
    QVERIFY(!parseLinkNotation(deep).ok());
  }

  void canonicalForm() {
    QCOMPARE(formatLink(QStringLiteral("3"), QStringLiteral("FS"), 0), QStringLiteral("3"));
    QCOMPARE(formatLink(QStringLiteral("3"), QStringLiteral("SS"), 0), QStringLiteral("3SS"));
    QCOMPARE(formatLink(QStringLiteral("3"), QStringLiteral("FS"), 2), QStringLiteral("3FS+2d"));
    QCOMPARE(formatLink(QStringLiteral("1.2"), QStringLiteral("FF"), -1), QStringLiteral("1.2FF-1d"));
    // Parsing the canonical form gives the link back.
    const QList<LinkSpec> all = {spec("3"), spec("4", "SS", 7), spec("1.2", "FF", -3), spec("9", "FS", 3650)};
    for (const LinkSpec& s : all) {
      const QList<LinkSpec> back = ok(formatLink(s.ref, s.type, s.lag).toUtf8().constData());
      QCOMPARE(back.size(), 1);
      QCOMPARE(back.first(), s);
    }
  }

  // Never crashes, and anything accepted formats and parses back the same.
  void randomInput() {
    const QString alphabet = QStringLiteral("0123456789abcdefABCDEFSsFf.,+-d xyz\t");
    QRandomGenerator rng(8805);
    int accepted = 0;
    for (int i = 0; i < 20000; ++i) {
      QString s;
      const int n = static_cast<int>(rng.bounded(30));
      for (int k = 0; k < n; ++k) s += alphabet.at(static_cast<qsizetype>(rng.bounded(alphabet.size())));
      auto r = parseLinkNotation(s);
      if (!r) continue;
      ++accepted;
      QStringList parts;
      for (const LinkSpec& l : r.value()) {
        QVERIFY(l.lag >= -kMaxLagDays && l.lag <= kMaxLagDays);
        parts << formatLink(l.ref, l.type, l.lag);
      }
      auto again = parseLinkNotation(parts.join(QLatin1Char(',')));
      QVERIFY2(again.ok(), qPrintable(s));
      QCOMPARE(again.value(), r.value());
    }
    QVERIFY(accepted > 100);
  }
};

QTEST_GUILESS_MAIN(TestLinkNotation)
#include "tst_linknotation.moc"
