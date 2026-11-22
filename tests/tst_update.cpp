// The desktop updater (SPEC.md §21), offscreen against a mock release feed:
// a MockServer plays api.github.com and the release-asset download URLs.
// Fixtures are signed at run time with a throwaway minisign key; the real
// key (32960BBA77B36F1F) is only parsed. With ZPROJECT_SCREENSHOT_DIR set,
// screenshots() renders docs/screenshots/update-*.png.
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QStyleFactory>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>

#include <memory>

#include "MinisignTestSigner.hpp"
#include "MockServer.hpp"
#include "app/Minisign.hpp"
#include "app/Theme.hpp"
#include "app/Update.hpp"
#include "app/UpdateChecker.hpp"
#include "app/UpdateDialog.hpp"
#include "app/UpdateManager.hpp"
#include "core/UpdateSigningKey.hpp"

using namespace zproject;
using namespace zproject::app;
using zproject::test::MinisignTestKey;
using zproject::test::MockReply;
using zproject::test::MockRequest;
using zproject::test::MockServer;

namespace {

const QString kCurrent = QStringLiteral("0.8.0");
const QString kApiPath = QStringLiteral("/repos/sbj-ee/zproject/releases/latest");

QByteArray sha256Hex(const QByteArray& b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex(); }

// One release as the mock feed serves it.
struct Feed {
  QString tag = QStringLiteral("v0.9.0");
  QString base;  // http://127.0.0.1:<port>
  QByteArray deb = QByteArray("!<arch>\ndebian-binary zproject 0.9.0 desktop package\n").repeated(64);
  QByteArray serverDeb = QByteArray("zprojectd 0.9.0 server package\n").repeated(16);
  QByteArray dmg = QByteArray("koly zproject 0.9.0 dmg\n").repeated(32);
  QByteArray sums;
  QByteArray sig;
  QHash<QString, QString> urlOverride;  // asset name -> browser_download_url
  QHash<QString, MockReply> replyOverride;
  QStringList omit;  // asset names left out of the release
  int apiStatus = 200;
  bool prerelease = false;

  QString version() const { return tag.mid(1); }
  QString debName() const { return QStringLiteral("zproject_%1_amd64.deb").arg(version()); }
  QString serverDebName() const { return QStringLiteral("zprojectd_%1_amd64.deb").arg(version()); }
  QString dmgName() const { return QStringLiteral("zproject-%1-Darwin.dmg").arg(version()); }

  QByteArray sumsFor() const {
    return sha256Hex(deb) + "  " + debName().toLatin1() + "\n" + sha256Hex(serverDeb) + "  " +
           serverDebName().toLatin1() + "\n" + sha256Hex(dmg) + "  " + dmgName().toLatin1() + "\n";
  }
  void signWith(const MinisignTestKey& key) {
    sums = sumsFor();
    sig = key.sign(sums, expectedSignedComment(tag).toUtf8());
  }
  QHash<QString, QByteArray> files() const {
    return {{debName(), deb},
            {serverDebName(), serverDeb},
            {dmgName(), dmg},
            {QStringLiteral("SHA256SUMS"), sums},
            {QStringLiteral("SHA256SUMS.minisig"), sig}};
  }
  QByteArray json() const {
    QJsonArray assets;
    const auto f = files();
    for (auto it = f.cbegin(); it != f.cend(); ++it) {
      if (omit.contains(it.key())) continue;
      const QString url = urlOverride.value(it.key(), base + QStringLiteral("/download/") + tag + QLatin1Char('/') + it.key());
      assets.append(QJsonObject{{QStringLiteral("name"), it.key()},
                                {QStringLiteral("browser_download_url"), url},
                                {QStringLiteral("size"), it.value().size()}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("tag_name"), tag},
                                     {QStringLiteral("name"), QStringLiteral("zproject ") + tag},
                                     {QStringLiteral("body"), QStringLiteral("## What's new\n\n- The in-app updater.\n- "
                                                                             "Faster Gantt rendering.")},
                                     {QStringLiteral("html_url"),
                                      QStringLiteral("https://github.com/sbj-ee/zproject/releases/tag/") + tag},
                                     {QStringLiteral("published_at"), QStringLiteral("2026-11-22T18:00:00Z")},
                                     {QStringLiteral("draft"), false},
                                     {QStringLiteral("prerelease"), prerelease},
                                     {QStringLiteral("assets"), assets}})
        .toJson(QJsonDocument::Compact);
  }
  MockReply handle(const MockRequest& r) const {
    if (r.path == kApiPath) {
      if (apiStatus != 200) {
        MockReply m = MockReply::json(apiStatus, QJsonObject{{QStringLiteral("message"), QStringLiteral("nope")}});
        if (apiStatus == 403) m.headers.append(qMakePair(QByteArray("X-RateLimit-Remaining"), QByteArray("0")));
        return m;
      }
      MockReply m;
      m.headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
      m.body = json();
      return m;
    }
    const QString prefix = QStringLiteral("/download/") + tag + QLatin1Char('/');
    if (r.path.startsWith(prefix)) {
      const QString name = r.path.mid(prefix.size());
      if (replyOverride.contains(name)) return replyOverride.value(name);
      const auto f = files();
      if (f.contains(name)) {
        MockReply m;
        m.headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/octet-stream")));
        m.body = f.value(name);
        return m;
      }
    }
    return MockReply::json(404, QJsonObject{{QStringLiteral("message"), QStringLiteral("Not Found")}});
  }
};

QPushButton* button(UpdateDialog* d, const char* name) {
  return d->findChild<QPushButton*>(QString::fromLatin1(name));
}
QString detailText(UpdateDialog* d) { return d->findChild<QLabel*>(QStringLiteral("updateDetail"))->text(); }

}  // namespace

class TestUpdate : public QObject {
  Q_OBJECT

  std::unique_ptr<MinisignTestKey> key_;
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<MockServer> mock_;
  std::unique_ptr<QWidget> host_;
  Feed feed_;

  QString statePath() const { return dir_->filePath(QStringLiteral("update-state.ini")); }
  QString apiUrl() const { return feed_.base + kApiPath; }

  std::unique_ptr<UpdateManager> manager(PackagePlatform p = PackagePlatform::Linux) {
    auto m = std::make_unique<UpdateManager>(UpdateManager::Options{statePath(), kCurrent, p}, host_.get());
    m->checker()->setApiUrl(QUrl(apiUrl()));
    m->checker()->setTimeoutMs(5000);
    m->downloader()->setStallTimeoutMs(5000);
    m->setOpenUrlHandler([](const QUrl&) {});
    m->setPackageNameReaderForTests([](const QString&) { return QStringLiteral("zproject"); });
    return m;
  }
  static bool waitState(UpdateDialog* d, UpdateDialog::State s, int ms = 8000) {
    return QTest::qWaitFor([&] { return d != nullptr && d->state() == s; }, ms);
  }
  bool requested(const QString& name) const {
    for (const MockRequest& r : mock_->requests()) {
      if (r.path.endsWith(QLatin1Char('/') + name)) return true;
    }
    return false;
  }
  // Check → Available → Download and Verify; returns the dialog.
  UpdateDialog* checkAndDownload(UpdateManager& m) {
    UpdateDialog* d = m.checkNow();
    if (!waitState(d, UpdateDialog::State::Available)) return nullptr;
    QTest::mouseClick(button(d, "updatePrimary"), Qt::LeftButton);
    const bool settled = QTest::qWaitFor([&] {
      return d->state() != UpdateDialog::State::Downloading && d->state() != UpdateDialog::State::Available;
    }, 8000);
    return settled ? d : nullptr;
  }

 private slots:
  void initTestCase() {
    Q_INIT_RESOURCE(zproject_app);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
    key_ = std::make_unique<MinisignTestKey>();
  }
  void init() {
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());
    mock_ = std::make_unique<MockServer>();
    QVERIFY(mock_->listen());
    feed_ = Feed();
    feed_.base = mock_->url().toString();
    feed_.signWith(*key_);
    mock_->setHandler([this](const MockRequest& r) { return feed_.handle(r); });
    setReleaseAssetBaseForTests(feed_.base);
    setUpdateSigningPublicKeyForTests(key_->publicKeyBase64());
    host_ = std::make_unique<QWidget>();
  }
  void cleanup() {
    host_.reset();
    mock_.reset();
    setReleaseAssetBaseForTests(QString());
    setUpdateSigningPublicKeyForTests(QString());
  }

  // ---- pure logic ----

  void realKeyParses() {
    setUpdateSigningPublicKeyForTests(QString());
    QCOMPARE(updateSigningPublicKey(), QString::fromLatin1(kUpdateSigningPublicKey));
    const auto k = MinisignPublicKey::parse(updateSigningPublicKey());
    QVERIFY(k.has_value());
    QCOMPARE(k->keyIdHex(), QStringLiteral("32960BBA77B36F1F"));
  }

  void semver() {
    QVERIFY(isNewerVersion(QStringLiteral("v0.9.0"), QStringLiteral("0.8.0")));
    QVERIFY(isNewerVersion(QStringLiteral("0.8.1"), QStringLiteral("0.8.0")));
    QVERIFY(isNewerVersion(QStringLiteral("1.0.0"), QStringLiteral("0.10.0")));
    QVERIFY(!isNewerVersion(QStringLiteral("v0.8.0"), QStringLiteral("0.8.0")));
    QVERIFY(!isNewerVersion(QStringLiteral("0.7.9"), QStringLiteral("0.8.0")));
    QVERIFY(isNewerVersion(QStringLiteral("0.9.0"), QStringLiteral("0.9.0-rc.2")));
    QVERIFY(isNewerVersion(QStringLiteral("0.9.0-rc.10"), QStringLiteral("0.9.0-rc.2")));
    QVERIFY(!isNewerVersion(QStringLiteral("garbage"), QStringLiteral("0.8.0")));
  }

  void minisign() {
    const auto pk = MinisignPublicKey::parse(key_->publicKeyBase64());
    QVERIFY(pk.has_value());
    const QByteArray msg("abc  file\n");
    const QByteArray trusted("zproject v0.9.0 SHA256SUMS");
    MinisignResult r = verifyMinisign(msg, key_->sign(msg, trusted), *pk);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.trustedComment, QString::fromLatin1(trusted));
    QVERIFY(verifyMinisign(msg, key_->sign(msg, trusted, false), *pk).ok);  // legacy "Ed"
    QVERIFY(!verifyMinisign(msg + "x", key_->sign(msg, trusted), *pk).ok);  // tampered file
    // A tampered trusted comment breaks the global signature.
    QByteArray sig = key_->sign(msg, trusted);
    sig.replace("v0.9.0", "v9.9.9");
    QVERIFY(!verifyMinisign(msg, sig, *pk).ok);
    // Another key (also a different key id).
    MinisignTestKey other;
    QVERIFY(!verifyMinisign(msg, other.sign(msg, trusted), *pk).ok);
    QVERIFY(!verifyMinisign(msg, QByteArray("not a signature"), *pk).ok);
    QVERIFY(!verifyMinisign(msg, QByteArray(), *pk).ok);
    QVERIFY(!MinisignPublicKey::parse(QStringLiteral("RWQ")).has_value());
  }

  void checksums() {
    const QString path = dir_->filePath(QStringLiteral("zproject_0.9.0_amd64.deb"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(feed_.deb);
    f.close();
    QVERIFY(verifyChecksum(path, feed_.sums, feed_.debName()).ok());
    QCOMPARE(verifyChecksum(path, feed_.sums, QStringLiteral("other.deb")).status, ChecksumResult::Status::NotListed);
    QByteArray bad = feed_.sums;
    bad.replace(sha256Hex(feed_.deb), QByteArray(64, 'a'));
    QCOMPARE(verifyChecksum(path, bad, feed_.debName()).status, ChecksumResult::Status::Mismatch);
    QCOMPARE(verifyChecksum(dir_->filePath(QStringLiteral("missing")), feed_.sums, feed_.debName()).status,
             ChecksumResult::Status::Unreadable);
  }

  void trustedUrls() {
    setReleaseAssetBaseForTests(QString());
    const QString ok = QStringLiteral("https://github.com/sbj-ee/zproject/releases/download/v0.9.0/zproject_0.9.0_amd64.deb");
    QVERIFY(isTrustedAssetUrl(ok, QStringLiteral("v0.9.0"), QStringLiteral("zproject_0.9.0_amd64.deb")));
    QVERIFY(!isTrustedAssetUrl(QString(ok).replace(QStringLiteral("https"), QStringLiteral("http")),
                               QStringLiteral("v0.9.0"), QStringLiteral("zproject_0.9.0_amd64.deb")));
    QVERIFY(!isTrustedAssetUrl(QString(ok).replace(QStringLiteral("github.com"), QStringLiteral("evil.example")),
                               QStringLiteral("v0.9.0"), QStringLiteral("zproject_0.9.0_amd64.deb")));
    QVERIFY(!isTrustedAssetUrl(QString(ok).replace(QStringLiteral("sbj-ee/zproject"), QStringLiteral("x/zproject")),
                               QStringLiteral("v0.9.0"), QStringLiteral("zproject_0.9.0_amd64.deb")));
    QVERIFY(!isTrustedAssetUrl(ok, QStringLiteral("v0.9.1"), QStringLiteral("zproject_0.9.0_amd64.deb")));
    QVERIFY(!isTrustedAssetUrl(ok + QStringLiteral("?x=1"), QStringLiteral("v0.9.0"),
                               QStringLiteral("zproject_0.9.0_amd64.deb")));
    QVERIFY(isTrustedAssetRedirect(QUrl(QStringLiteral("https://release-assets.githubusercontent.com/a/b"))));
    QVERIFY(!isTrustedAssetRedirect(QUrl(QStringLiteral("http://release-assets.githubusercontent.com/a/b"))));
    QVERIFY(!isTrustedAssetRedirect(QUrl(QStringLiteral("https://evil.example/a"))));
    QVERIFY(isTrustedReleasePageUrl(QStringLiteral("https://github.com/sbj-ee/zproject/releases/tag/v0.9.0")));
    QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("javascript:alert(1)")));
    // https only: plain http is refused outside the tests' loopback feed.
    QVERIFY(isAllowedUpdateUrl(QUrl(UpdateChecker::defaultApiUrl())));
    QVERIFY(!isAllowedUpdateUrl(QUrl(QStringLiteral("http://127.0.0.1:1/x"))));
    QVERIFY(!isAllowedUpdateUrl(QUrl(QStringLiteral("http://api.github.com/x"))));
    QVERIFY(!isAllowedUpdateUrl(QUrl(QStringLiteral("ftp://api.github.com/x"))));
  }

  void serverPackageIsNeverChosen() {
    ReleaseInfo r;
    r.tag = QStringLiteral("v0.9.0");
    r.version = SemVer::parse(r.tag);
    r.assets = {{QStringLiteral("zprojectd_0.9.0_amd64.deb"), QStringLiteral("u1"), 1}};
    QVERIFY(!r.packageAsset(PackagePlatform::Linux).has_value());
    r.assets.append({QStringLiteral("zproject_0.9.0_amd64.deb"), QStringLiteral("u2"), 1});
    QCOMPARE(r.packageAsset(PackagePlatform::Linux)->name, QStringLiteral("zproject_0.9.0_amd64.deb"));
    QVERIFY(isServerPackageName(QStringLiteral("zprojectd_0.9.0_amd64.deb")));
    QVERIFY(!isServerPackageName(QStringLiteral("zproject_0.9.0_amd64.deb")));
    const QStringList cmd = UpdateManager::installCommand(QStringLiteral("/tmp/x/zproject_0.9.0_amd64.deb"));
    QCOMPARE(cmd, (QStringList{QStringLiteral("/usr/bin/pkexec"), QStringLiteral("/usr/bin/apt"),
                               QStringLiteral("install"), QStringLiteral("-y"),
                               QStringLiteral("/tmp/x/zproject_0.9.0_amd64.deb")}));
  }

  void policy() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    UpdateState st;
    QVERIFY(!UpdatePolicy::shouldCheckAtStartup(false, st, now));
    QVERIFY(UpdatePolicy::shouldCheckAtStartup(true, st, now));
    st.lastCheck = now.addSecs(-3600);
    QVERIFY(!UpdatePolicy::shouldCheckAtStartup(true, st, now));
    st.lastCheck = now.addDays(-2);
    QVERIFY(UpdatePolicy::shouldCheckAtStartup(true, st, now));
    st.lastCheck = now.addDays(2);  // clock went backwards
    QVERIFY(UpdatePolicy::shouldCheckAtStartup(true, st, now));

    ReleaseInfo r;
    r.tag = QStringLiteral("v0.9.0");
    r.version = SemVer::parse(r.tag);
    UpdateState skip;
    skip.skippedVersion = QStringLiteral("v0.9.0");
    QVERIFY(UpdatePolicy::shouldOffer(r, kCurrent, UpdateState(), false));
    QVERIFY(!UpdatePolicy::shouldOffer(r, kCurrent, skip, false));
    QVERIFY(UpdatePolicy::shouldOffer(r, kCurrent, skip, true));  // Help → Check always offers
    QVERIFY(!UpdatePolicy::shouldOffer(r, QStringLiteral("0.9.0"), UpdateState(), true));

    UpdateState saved;
    saved.lastCheck = QDateTime::fromSecsSinceEpoch(1700000000).toUTC();
    saved.skippedVersion = QStringLiteral("v0.9.0");
    QVERIFY(saved.save(statePath()));
    QCOMPARE(QFileInfo(statePath()).permissions() & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteOther),
             QFile::Permissions());
    const UpdateState loaded = UpdateState::load(statePath());
    QCOMPARE(loaded.lastCheck, saved.lastCheck);
    QCOMPARE(loaded.skippedVersion, saved.skippedVersion);
  }

  // ---- the mock release feed ----

  void validUpdateVerifiesThenInstalls() {
    auto m = manager();
    QStringList installed;
    m->setInstallerForTests(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("test -s \"$0\"")});
    m->setPackageNameReaderForTests([&installed](const QString& deb) {
      installed << deb;
      return QStringLiteral("zproject");
    });
    QSignalSpy verified(m.get(), &UpdateManager::verificationFinished);
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QCOMPARE(d->state(), UpdateDialog::State::Verified);
    QCOMPARE(verified.count(), 1);
    QVERIFY(verified.at(0).at(0).toBool());
    // Signature first, then the package; never the server package.
    QStringList order;
    for (const MockRequest& r : mock_->requests()) order << QFileInfo(r.path).fileName();
    QCOMPARE(order, (QStringList{QStringLiteral("latest"), QStringLiteral("SHA256SUMS"),
                                 QStringLiteral("SHA256SUMS.minisig"), feed_.debName()}));
    QVERIFY(!requested(feed_.serverDebName()));
    QCOMPARE(sha256OfFile(m->verifiedPackage()), QString::fromLatin1(sha256Hex(feed_.deb)));
    QCOMPARE(m->downloader()->verifiedSha256(), QString::fromLatin1(sha256Hex(feed_.deb)));
    QVERIFY(detailText(d).contains(QStringLiteral("signature verified")));
    QVERIFY(installed.isEmpty());  // nothing installs until the user says so

    QSignalSpy done(m.get(), &UpdateManager::installFinished);
    QTest::mouseClick(button(d, "updatePrimary"), Qt::LeftButton);  // Install…
    QVERIFY(waitState(d, UpdateDialog::State::Installed));
    QCOMPARE(done.count(), 1);
    QVERIFY(done.at(0).at(0).toBool());
    QCOMPARE(installed.size(), 1);
    QVERIFY(installed.at(0) != m->verifiedPackage());  // a private staged copy
    QVERIFY(installed.at(0).endsWith(QLatin1Char('/') + feed_.debName()));
    bool restarted = false;
    m->setRestartHandlerForTests([&restarted] { restarted = true; });
    QTest::mouseClick(button(d, "updatePrimary"), Qt::LeftButton);  // Restart Now
    QVERIFY(restarted);
    QVERIFY(m->restartRequested());
  }

  void badSignatureIsRefused() {
    MinisignTestKey other;
    feed_.sig = other.sign(feed_.sums, expectedSignedComment(feed_.tag).toUtf8());
    auto m = manager();
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QCOMPARE(d->state(), UpdateDialog::State::Refused);
    QVERIFY(!requested(feed_.debName()));  // the package is never fetched
    QVERIFY(m->verifiedPackage().isEmpty());
    QVERIFY(!button(d, "updatePrimary")->isVisible() || button(d, "updatePrimary")->text() != tr("&Install…"));
  }

  void tamperedChecksumsAreRefused() {
    // SHA256SUMS changed after signing (the attacker swapped the hash).
    feed_.sums.replace(sha256Hex(feed_.deb), sha256Hex("evil"));
    auto m = manager();
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QCOMPARE(d->state(), UpdateDialog::State::Refused);
    QVERIFY(!requested(feed_.debName()));
  }

  void signatureForAnotherReleaseIsRefused() {
    feed_.sig = key_->sign(feed_.sums, QByteArrayLiteral("zproject v0.8.0 SHA256SUMS"));  // replayed
    auto m = manager();
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QCOMPARE(d->state(), UpdateDialog::State::Refused);
    QVERIFY(detailText(d).contains(QStringLiteral("v0.8.0")) || detailText(d).contains(QStringLiteral("comment")));
    QVERIFY(!requested(feed_.debName()));
  }

  void badChecksumIsRefusedAndDeleted() {
    // A correctly signed SHA256SUMS, but the package bytes differ.
    feed_.deb.append("tampered");
    QByteArray sums = feed_.sums;
    feed_.signWith(*key_);           // recomputes sums from the new deb …
    feed_.sums = sums;               // … then restore the old, signed list
    feed_.sig = key_->sign(feed_.sums, expectedSignedComment(feed_.tag).toUtf8());
    auto m = manager();
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QCOMPARE(d->state(), UpdateDialog::State::Refused);
    QVERIFY(requested(feed_.debName()));
    QVERIFY(detailText(d).contains(QStringLiteral("SHA-256")) || detailText(d).contains(QStringLiteral("checksum")));
    QVERIFY(m->verifiedPackage().isEmpty());
    QVERIFY(!QFileInfo::exists(QDir::tempPath() + QStringLiteral("/") + feed_.debName()));
  }

  void noNewerVersion() {
    feed_.tag = QStringLiteral("v0.8.0");
    feed_.signWith(*key_);
    auto m = manager();
    QSignalSpy spy(m.get(), &UpdateManager::checkFinished);
    UpdateDialog* d = m->checkNow();
    QVERIFY(waitState(d, UpdateDialog::State::UpToDate));
    QCOMPARE(spy.count(), 1);
    QVERIFY(!spy.at(0).at(0).toBool());
    QVERIFY(detailText(d).contains(QStringLiteral("latest version")));
    feed_.tag = QStringLiteral("v0.7.2");  // older than what's running
    feed_.signWith(*key_);
    QTest::mouseClick(button(d, "updatePrimary"), Qt::LeftButton);  // OK closes
    d = m->checkNow();
    QVERIFY(waitState(d, UpdateDialog::State::UpToDate));
    QCOMPARE(mock_->requests().size(), 2);  // API only, no downloads
  }

  void networkError() {
    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const quint16 port = closed.serverPort();
    closed.close();
    auto m = manager();
    m->checker()->setApiUrl(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(kApiPath)));
    UpdateDialog* d = m->checkNow();
    QVERIFY(waitState(d, UpdateDialog::State::CheckFailed));
    QVERIFY(!detailText(d).isEmpty());
    // Try Again with the network back.
    m->checker()->setApiUrl(QUrl(apiUrl()));
    QTest::mouseClick(button(d, "updatePrimary"), Qt::LeftButton);
    QVERIFY(waitState(d, UpdateDialog::State::Available));
  }

  void networkErrorDuringDownload() {
    MockReply drop;
    drop.drop = true;
    feed_.replyOverride.insert(feed_.debName(), drop);
    auto m = manager();
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QCOMPARE(d->state(), UpdateDialog::State::Refused);
    QVERIFY(m->verifiedPackage().isEmpty());
  }

  void apiErrors() {
    feed_.apiStatus = 404;  // no release yet
    auto m = manager();
    UpdateDialog* d = m->checkNow();
    QVERIFY(waitState(d, UpdateDialog::State::UpToDate));
    feed_.apiStatus = 403;  // rate limited
    QTest::mouseClick(button(d, "updatePrimary"), Qt::LeftButton);
    d = m->checkNow();
    QVERIFY(waitState(d, UpdateDialog::State::CheckFailed));
    QVERIFY(detailText(d).contains(QStringLiteral("rate"), Qt::CaseInsensitive));
  }

  void httpsOnly() {
    auto m = manager();
    setReleaseAssetBaseForTests(QString());  // not test mode: loopback http is refused too
    UpdateDialog* d = m->checkNow();
    QVERIFY(waitState(d, UpdateDialog::State::CheckFailed));
    QVERIFY(mock_->requests().isEmpty());
  }

  void untrustedAssetUrlIsRefused() {
    feed_.urlOverride.insert(feed_.debName(), QStringLiteral("https://evil.example/zproject_0.9.0_amd64.deb"));
    auto m = manager();
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QCOMPARE(d->state(), UpdateDialog::State::Refused);
    QCOMPARE(mock_->requests().size(), 1);  // just the API
  }

  void redirectToForeignHostIsRefused() {
    MockReply redirect;
    redirect.status = 302;
    redirect.headers.append(qMakePair(QByteArray("Location"), QByteArray("http://evil.example/zproject_0.9.0_amd64.deb")));
    feed_.replyOverride.insert(feed_.debName(), redirect);
    auto m = manager();
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QCOMPARE(d->state(), UpdateDialog::State::Refused);
    QVERIFY(detailText(d).contains(QStringLiteral("evil.example")));
  }

  void serverOnlyReleaseCannotBeInstalled() {
    feed_.omit << feed_.debName();
    auto m = manager();
    UpdateDialog* d = m->checkNow();
    QVERIFY(waitState(d, UpdateDialog::State::Available));
    QVERIFY(!button(d, "updatePrimary")->isEnabled());
    m->downloader()->start(m->release(), dir_->path(), PackagePlatform::Linux);  // even if forced
    QTest::qWait(100);
    QVERIFY(!requested(feed_.serverDebName()));
    QVERIFY(!requested(QStringLiteral("SHA256SUMS")));
  }

  void serverPackageContentsAreNotInstalled() {
    // The asset is named like the desktop package but its control file
    // says zprojectd: refuse before pkexec.
    auto m = manager();
    bool ran = false;
    m->setInstallerForTests(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 0")});
    m->setPackageNameReaderForTests([&ran](const QString&) {
      ran = true;
      return QStringLiteral("zprojectd");
    });
    QSignalSpy done(m.get(), &UpdateManager::installFinished);
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QCOMPARE(d->state(), UpdateDialog::State::Verified);
    QTest::mouseClick(button(d, "updatePrimary"), Qt::LeftButton);
    QVERIFY(waitState(d, UpdateDialog::State::InstallFailed));
    QVERIFY(ran);
    QCOMPARE(done.count(), 1);
    QVERIFY(!done.at(0).at(0).toBool());
    QVERIFY(detailText(d).contains(QStringLiteral("zprojectd")));
    QVERIFY(!detailText(d).contains(QStringLiteral("sudo apt install")));
    QVERIFY(m->verifiedPackage().isEmpty());  // deleted, not offered for a manual install
  }

  void installDismissedKeepsPackage() {
    auto m = manager();
    m->setInstallerForTests(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 126")});
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QTest::mouseClick(button(d, "updatePrimary"), Qt::LeftButton);
    QVERIFY(waitState(d, UpdateDialog::State::InstallFailed));
    QVERIFY(detailText(d).contains(QStringLiteral("dismissed")));
    QVERIFY(detailText(d).contains(QStringLiteral("sudo apt install")));
    const QString kept = m->verifiedPackage();
    QVERIFY(QFileInfo::exists(kept));
    m.reset();
    // Kept on purpose for a manual install; tidy up exactly what the test made.
    const QDir keptDir = QFileInfo(kept).absoluteDir();
    QVERIFY(QFileInfo::exists(kept));
    for (const QString& name : {feed_.debName(), QStringLiteral("SHA256SUMS"), QStringLiteral("SHA256SUMS.minisig")}) {
      QVERIFY(QFile::remove(keptDir.filePath(name)));
    }
    QVERIFY(QDir().rmdir(keptDir.absolutePath()));
  }

  void macDiskImage() {
    auto m = manager(PackagePlatform::MacOS);
    QStringList opened;
    m->setInstallerForTests(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("test -s \"$0\"")});
    UpdateDialog* d = checkAndDownload(*m);
    QVERIFY(d);
    QCOMPARE(d->state(), UpdateDialog::State::Verified);
    QVERIFY(requested(feed_.dmgName()));
    QVERIFY(!requested(feed_.debName()));
    QVERIFY(!requested(feed_.serverDebName()));
  }

  void skipThisVersion() {
    auto m = manager();
    UpdateDialog* d = m->checkNow();
    QVERIFY(waitState(d, UpdateDialog::State::Available));
    QTest::mouseClick(button(d, "updateSkip"), Qt::LeftButton);
    QCOMPARE(UpdateState::load(statePath()).skippedVersion, QStringLiteral("v0.9.0"));
    // The daily check stays quiet about v0.9.0 …
    QSignalSpy spy(m.get(), &UpdateManager::checkFinished);
    QVERIFY(m->startupCheckIfDue(true));
    QVERIFY(spy.wait(5000));
    QVERIFY(!spy.at(0).at(0).toBool());
    QVERIFY(UpdateState::load(statePath()).lastCheck.isValid());
    QVERIFY(!m->startupCheckIfDue(true));  // … and won't ask again today
    // … but v0.9.1 is offered.
    UpdateState st = UpdateState::load(statePath());
    st.lastCheck = QDateTime();
    QVERIFY(st.save(statePath()));
    feed_.tag = QStringLiteral("v0.9.1");
    feed_.signWith(*key_);
    QVERIFY(m->startupCheckIfDue(true));
    QVERIFY(spy.wait(5000));
    QVERIFY(spy.at(1).at(0).toBool());
    QVERIFY(waitState(m->dialog(), UpdateDialog::State::Available));
  }

  void startupCheckIsQuiet() {
    auto m = manager();
    QVERIFY(!m->startupCheckIfDue(false));  // turned off: no request at all
    QTest::qWait(50);
    QVERIFY(mock_->requests().isEmpty());
    feed_.apiStatus = 500;
    QSignalSpy spy(m.get(), &UpdateManager::checkFinished);
    QVERIFY(m->startupCheckIfDue(true));
    QVERIFY(spy.wait(5000));
    QVERIFY(m->dialog() == nullptr);  // failures don't interrupt
  }

  void cancelDownload() {
    MockReply hang;
    hang.hang = true;
    feed_.replyOverride.insert(QStringLiteral("SHA256SUMS"), hang);
    auto m = manager();
    UpdateDialog* d = m->checkNow();
    QVERIFY(waitState(d, UpdateDialog::State::Available));
    QTest::mouseClick(button(d, "updatePrimary"), Qt::LeftButton);
    QVERIFY(waitState(d, UpdateDialog::State::Downloading));
    QTest::mouseClick(button(d, "updateLater"), Qt::LeftButton);  // Cancel
    QVERIFY(waitState(d, UpdateDialog::State::Available));
  }

  void defaultButtonFollowsState() {
    ReleaseInfo r = *ReleaseInfo::fromJson(feed_.json());
    UpdateDialog d(kCurrent);
    d.setPlatform(PackagePlatform::Linux);
    d.show();
    QVERIFY(QTest::qWaitForWindowExposed(&d));
    d.showAvailable(r);
    QVERIFY(button(&d, "updatePrimary")->isDefault());
    QCOMPARE(d.focusWidget(), button(&d, "updatePrimary"));
    d.showInstalled(QStringLiteral("done"), true);
    QVERIFY(button(&d, "updatePrimary")->isDefault());
    QCOMPARE(d.focusWidget(), button(&d, "updatePrimary"));
  }

  // ---- screenshots (docs/screenshots/), only on request ----

  void screenshots() {
    const QString out = qEnvironmentVariable("ZPROJECT_SCREENSHOT_DIR");
    if (out.isEmpty()) QSKIP("set ZPROJECT_SCREENSHOT_DIR to render the screenshots");
    QVERIFY(QDir().mkpath(out));
    // The real key id on the pictures.
    setUpdateSigningPublicKeyForTests(QString());
    const ThemeColors light = Theme::colors(QStringLiteral("light"));
    QApplication::setPalette(Theme::palette(light));
    qApp->setStyleSheet(Theme::styleSheet(light, QApplication::font().pointSizeF()));
    auto save = [&](UpdateDialog* d, const QString& name) {
      d->resize(620, d->sizeHint().height());
      QCoreApplication::processEvents();
      QTest::qWait(50);
      QVERIFY2(d->grab().save(out + QLatin1Char('/') + name), qPrintable(name));
    };
    ReleaseInfo r = *ReleaseInfo::fromJson(feed_.json());
    UpdateDialog d(kCurrent);
    d.setPlatform(PackagePlatform::Linux);
    d.show();
    QVERIFY(QTest::qWaitForWindowExposed(&d));
    d.showAvailable(r);
    save(&d, QStringLiteral("update-available.png"));
    d.showDownloading(feed_.debName(), 412, 1000);
    save(&d, QStringLiteral("update-downloading.png"));
    d.showVerified(QStringLiteral("/tmp/zproject-update-Ab12Cd/") + feed_.debName(),
                   QString::fromLatin1(sha256Hex(feed_.deb)),
                   QStringLiteral("Install runs “pkexec apt install” for the zproject desktop package only; you'll "
                                  "be asked for your password. The zprojectd server package is not touched."));
    save(&d, QStringLiteral("update-verified.png"));
    d.showRefused(QStringLiteral("The release's SHA256SUMS signature is not valid (the signature was made with a "
                                 "different key), so nothing was downloaded or installed."));
    save(&d, QStringLiteral("update-refused-signature.png"));
    {
      ChecksumResult bad;
      bad.status = ChecksumResult::Status::Mismatch;
      bad.expected = QString::fromLatin1(sha256Hex(feed_.deb));
      bad.actual = QString::fromLatin1(sha256Hex("tampered"));
      d.showRefused(bad.message());
    }
    save(&d, QStringLiteral("update-refused-checksum.png"));
    d.showUpToDate(QStringLiteral("You're running the latest version of zproject (0.8.0)."));
    save(&d, QStringLiteral("update-up-to-date.png"));
    d.showCheckFailed(QStringLiteral("Couldn't reach GitHub to check for updates (no network?)."));
    save(&d, QStringLiteral("update-check-failed.png"));
    d.showInstalled(QStringLiteral("zproject v0.9.0 was installed. Restart zproject to use it."), true);
    save(&d, QStringLiteral("update-installed.png"));
  }
};

QTEST_MAIN(TestUpdate)
#include "tst_update.moc"
