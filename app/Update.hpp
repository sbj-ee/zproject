#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace zproject::app {

// The in-app updater's rules (SPEC.md §21), ported from zterminal: version
// comparison, the GitHub release JSON, SHA256SUMS, trusted URLs, the
// signing key and when to check. No networking and no widgets here.

// Semantic version: MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD], a leading 'v'
// allowed (git tags). Missing parts count as 0 ("1.2" = 1.2.0).
struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
  QStringList prerelease;  // "rc.1" -> {"rc", "1"}; empty = a release
  bool valid = false;

  static SemVer parse(const QString& text);
  QString toString() const;
  // <0, 0, >0. Build metadata is ignored; 1.0.0-rc.1 < 1.0.0.
  static int compare(const SemVer& a, const SemVer& b);
};

// True when `candidate` is a newer valid version than `current`.
bool isNewerVersion(const QString& candidate, const QString& current);

struct ReleaseAsset {
  QString name;
  QString url;  // browser_download_url
  qint64 size = 0;
  bool isValid() const { return !name.isEmpty() && !url.isEmpty(); }
};

enum class PackagePlatform { Linux, MacOS };
PackagePlatform hostPackagePlatform();

// GET /repos/sbj-ee/zproject/releases/latest, the parts we use.
struct ReleaseInfo {
  QString tag;  // "v0.9.0"
  SemVer version;
  QString name;
  QString notes;    // body (Markdown)
  QString htmlUrl;  // release page
  QDateTime publishedAt;
  bool draft = false;
  bool prerelease = false;
  QList<ReleaseAsset> assets;

  static std::optional<ReleaseInfo> fromJson(const QByteArray& json, QString* error = nullptr);
  // The desktop package only: zproject_<v>_amd64.deb or
  // zproject-<v>-Darwin.dmg. Never zprojectd_<v>_amd64.deb (the server).
  std::optional<ReleaseAsset> packageAsset(PackagePlatform p = hostPackagePlatform()) const;
  static QString packageName(const QString& version, PackagePlatform p = hostPackagePlatform());
  std::optional<ReleaseAsset> checksumAsset() const;   // "SHA256SUMS"
  std::optional<ReleaseAsset> signatureAsset() const;  // "SHA256SUMS.minisig"
  QString versionString() const { return version.toString(); }
};

// True for the server package's file name in any spelling: the desktop
// updater never downloads or installs it.
bool isServerPackageName(const QString& fileName);

// SHA256SUMS ("<64 hex>  <name>" or "<64 hex> *<name>" per line).
struct ChecksumResult {
  enum class Status { Ok, Mismatch, NotListed, BadFile, Unreadable };
  Status status = Status::Unreadable;
  QString expected;  // lower-case hex from SHA256SUMS
  QString actual;    // lower-case hex of the file
  QString message() const;
  bool ok() const { return status == Status::Ok; }
};
QString checksumFor(const QByteArray& sha256sums, const QString& fileName);
QString sha256OfFile(const QString& path);  // lower-case hex, empty if unreadable
ChecksumResult verifyChecksum(const QString& filePath, const QByteArray& sha256sums, const QString& fileName);

// https://github.com/sbj-ee/zproject/releases/... only.
bool isTrustedReleasePageUrl(const QString& url);
// Exactly https://github.com/sbj-ee/zproject/releases/download/<tag>/<name>.
bool isTrustedAssetUrl(const QString& url, const QString& tag, const QString& name);
// Where GitHub may redirect such a download: https on its release-asset hosts.
bool isTrustedAssetRedirect(const QUrl& url);
// Tests serve assets from a local mock: base "http://127.0.0.1:<port>" makes
// <base>/download/<tag>/<name> (and redirects to that host) the trusted
// locations instead. Empty: back to github.com.
void setReleaseAssetBaseForTests(const QString& base);
// True only while a test has set a release-asset base; plain-http loopback
// feeds are refused otherwise (https only, SPEC.md §21).
bool releaseTestModeActive();

// The trusted comment the release workflow signs: "zproject v0.9.0 SHA256SUMS".
QString expectedSignedComment(const QString& tag);

// The base64 minisign public key (core/UpdateSigningKey.cpp, key ID
// 32960BBA77B36F1F), or the tests' key.
QString updateSigningPublicKey();
void setUpdateSigningPublicKeyForTests(const QString& base64);  // empty: the built-in key

// <config dir>/update-state.ini (SPEC.md §16.5), apart from zproject.ini.
struct UpdateState {
  QDateTime lastCheck;     // last automatic check (UTC)
  QString skippedVersion;  // "Skip This Version": the tag

  static UpdateState load(const QString& path);
  bool save(const QString& path) const;
};

struct UpdatePolicy {
  static constexpr qint64 kMinSecondsBetweenChecks = qint64{24} * 60 * 60;
  static bool shouldCheckAtStartup(bool enabled, const UpdateState& state, const QDateTime& now);
  // Newer than the running version, not a draft; a skipped tag stays quiet
  // only for automatic checks (anything newer is offered again).
  static bool shouldOffer(const ReleaseInfo& release, const QString& currentVersion, const UpdateState& state,
                          bool manual);
};

}  // namespace zproject::app
