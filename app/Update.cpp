#include "app/Update.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>
#include <array>

#include "core/UpdateSigningKey.hpp"

namespace zproject::app {

namespace {

bool isNumeric(const QString& s) {
  return !s.isEmpty() && std::all_of(s.cbegin(), s.cend(), [](QChar c) { return c.isDigit(); });
}

QString& assetBaseForTests() {
  static QString base;
  return base;
}

QString& testKey() {
  static QString k;
  return k;
}

bool isPlainPathSegment(const QString& s) {
  static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9._~+-]+$"));
  return ok.match(s).hasMatch() && s != QLatin1String(".") && s != QLatin1String("..");
}

const QString kRepoPath = QStringLiteral("/sbj-ee/zproject/releases/");

}  // namespace

SemVer SemVer::parse(const QString& text) {
  SemVer v;
  QString s = text.trimmed();
  if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V'))) s.remove(0, 1);
  if (const qsizetype plus = s.indexOf(QLatin1Char('+')); plus >= 0) s.truncate(plus);
  QString pre;
  qsizetype dash = s.indexOf(QLatin1Char('-'));
  if (dash < 0) dash = s.indexOf(QLatin1Char('~'));  // Debian: 1.0.0~rc1
  if (dash >= 0) {
    pre = s.mid(dash + 1);
    s.truncate(dash);
  }
  const QStringList parts = s.split(QLatin1Char('.'));
  if (parts.isEmpty() || parts.size() > 3) return v;
  std::array<int, 3> nums{0, 0, 0};
  for (qsizetype i = 0; i < parts.size(); ++i) {
    if (!isNumeric(parts[i]) || parts[i].size() > 9) return v;
    nums[static_cast<std::size_t>(i)] = parts[i].toInt();
  }
  v.major = nums[0];
  v.minor = nums[1];
  v.patch = nums[2];
  if (!pre.isEmpty()) {
    v.prerelease = pre.split(QLatin1Char('.'));
    for (const QString& p : std::as_const(v.prerelease)) {
      if (p.isEmpty()) return SemVer{};
    }
  } else if (dash >= 0) {
    return SemVer{};  // "1.0.0-"
  }
  v.valid = true;
  return v;
}

QString SemVer::toString() const {
  QString s = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
  if (!prerelease.isEmpty()) s += QLatin1Char('-') + prerelease.join(QLatin1Char('.'));
  return s;
}

int SemVer::compare(const SemVer& a, const SemVer& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.prerelease.isEmpty() || b.prerelease.isEmpty()) {
    if (a.prerelease.isEmpty() == b.prerelease.isEmpty()) return 0;
    return a.prerelease.isEmpty() ? 1 : -1;  // a release ranks above its prereleases
  }
  for (qsizetype i = 0; i < std::min(a.prerelease.size(), b.prerelease.size()); ++i) {
    const QString& x = a.prerelease[i];
    const QString& y = b.prerelease[i];
    const bool nx = isNumeric(x);
    const bool ny = isNumeric(y);
    if (nx && ny) {
      const qint64 ix = x.toLongLong();
      const qint64 iy = y.toLongLong();
      if (ix != iy) return ix < iy ? -1 : 1;
    } else if (nx != ny) {
      return nx ? -1 : 1;
    } else if (const int c = QString::compare(x, y); c != 0) {
      return c < 0 ? -1 : 1;
    }
  }
  if (a.prerelease.size() != b.prerelease.size()) return a.prerelease.size() < b.prerelease.size() ? -1 : 1;
  return 0;
}

bool isNewerVersion(const QString& candidate, const QString& current) {
  const SemVer c = SemVer::parse(candidate);
  const SemVer r = SemVer::parse(current);
  return c.valid && r.valid && SemVer::compare(c, r) > 0;
}

PackagePlatform hostPackagePlatform() {
#ifdef Q_OS_MACOS
  return PackagePlatform::MacOS;
#else
  return PackagePlatform::Linux;
#endif
}

std::optional<ReleaseInfo> ReleaseInfo::fromJson(const QByteArray& json, QString* error) {
  auto fail = [error](const QString& m) -> std::optional<ReleaseInfo> {
    if (error) *error = m;
    return std::nullopt;
  };
  QJsonParseError pe{};
  const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
    return fail(QStringLiteral("GitHub sent a reply zproject could not read."));
  }
  const QJsonObject o = doc.object();
  ReleaseInfo r;
  r.tag = o.value(QStringLiteral("tag_name")).toString();
  if (r.tag.isEmpty()) return fail(QStringLiteral("No published zproject release was found on GitHub."));
  r.version = SemVer::parse(r.tag);
  if (!r.version.valid) {
    return fail(QStringLiteral("The latest release has a tag zproject doesn't understand (%1).").arg(r.tag));
  }
  r.name = o.value(QStringLiteral("name")).toString();
  r.notes = o.value(QStringLiteral("body")).toString();
  r.htmlUrl = o.value(QStringLiteral("html_url")).toString();
  r.publishedAt = QDateTime::fromString(o.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
  r.draft = o.value(QStringLiteral("draft")).toBool();
  r.prerelease = o.value(QStringLiteral("prerelease")).toBool();
  const QJsonArray assets = o.value(QStringLiteral("assets")).toArray();
  for (const QJsonValue& v : assets) {
    const QJsonObject a = v.toObject();
    ReleaseAsset asset;
    asset.name = a.value(QStringLiteral("name")).toString();
    asset.url = a.value(QStringLiteral("browser_download_url")).toString();
    const qint64 size = a.value(QStringLiteral("size")).toInteger(0);
    asset.size = size < 0 ? 0 : size;
    if (asset.isValid()) r.assets << asset;
  }
  return r;
}

QString ReleaseInfo::packageName(const QString& version, PackagePlatform p) {
  return p == PackagePlatform::MacOS ? QStringLiteral("zproject-%1-Darwin.dmg").arg(version)
                                     : QStringLiteral("zproject_%1_amd64.deb").arg(version);
}

std::optional<ReleaseAsset> ReleaseInfo::packageAsset(PackagePlatform p) const {
  QStringList names{packageName(version.toString(), p)};
  if (p == PackagePlatform::Linux && !version.prerelease.isEmpty()) {
    QString deb = version.toString();
    deb.replace(QLatin1Char('-'), QLatin1Char('~'));
    names << packageName(deb, p);
  }
  for (const ReleaseAsset& a : assets) {
    if (names.contains(a.name) && !isServerPackageName(a.name)) return a;
  }
  return std::nullopt;
}

std::optional<ReleaseAsset> ReleaseInfo::checksumAsset() const {
  for (const ReleaseAsset& a : assets) {
    if (a.name == QLatin1String("SHA256SUMS")) return a;
  }
  return std::nullopt;
}

std::optional<ReleaseAsset> ReleaseInfo::signatureAsset() const {
  for (const ReleaseAsset& a : assets) {
    if (a.name == QLatin1String("SHA256SUMS.minisig")) return a;
  }
  return std::nullopt;
}

bool isServerPackageName(const QString& fileName) {
  return QFileInfo(fileName).fileName().startsWith(QLatin1String("zprojectd"), Qt::CaseInsensitive);
}

bool isTrustedReleasePageUrl(const QString& url) {
  const QUrl u(url, QUrl::StrictMode);
  return u.isValid() && u.scheme() == QLatin1String("https") && u.host() == QLatin1String("github.com") &&
         u.port() == -1 && u.userInfo().isEmpty() && u.path().startsWith(kRepoPath);
}

void setReleaseAssetBaseForTests(const QString& base) { assetBaseForTests() = base; }

bool releaseTestModeActive() { return !assetBaseForTests().isEmpty(); }

bool isTrustedAssetUrl(const QString& url, const QString& tag, const QString& name) {
  if (!isPlainPathSegment(tag) || !isPlainPathSegment(name)) return false;
  const QUrl u(url, QUrl::StrictMode);
  if (!u.isValid() || !u.userInfo().isEmpty() || u.hasQuery() || u.hasFragment()) return false;
  const QString path = u.path(QUrl::FullyEncoded);
  const QString& testBase = assetBaseForTests();
  if (!testBase.isEmpty()) {
    const QUrl b(testBase);
    return u.scheme() == b.scheme() && u.host() == b.host() && u.port() == b.port() &&
           path == b.path() + QStringLiteral("/download/") + tag + QLatin1Char('/') + name;
  }
  return u.scheme() == QLatin1String("https") && u.host() == QLatin1String("github.com") && u.port() == -1 &&
         path == kRepoPath + QStringLiteral("download/") + tag + QLatin1Char('/') + name;
}

bool isTrustedAssetRedirect(const QUrl& url) {
  if (!url.isValid() || !url.userInfo().isEmpty()) return false;
  const QString& testBase = assetBaseForTests();
  if (!testBase.isEmpty()) {
    const QUrl b(testBase);
    if (url.scheme() == b.scheme() && url.host() == b.host() && url.port() == b.port()) return true;
  }
  return url.scheme() == QLatin1String("https") && url.port() == -1 &&
         (url.host() == QLatin1String("release-assets.githubusercontent.com") ||
          url.host() == QLatin1String("objects.githubusercontent.com"));
}

QString expectedSignedComment(const QString& tag) { return QStringLiteral("zproject %1 SHA256SUMS").arg(tag); }

QString updateSigningPublicKey() {
  return testKey().isEmpty() ? QString::fromLatin1(kUpdateSigningPublicKey) : testKey();
}

void setUpdateSigningPublicKeyForTests(const QString& base64) { testKey() = base64; }

QString checksumFor(const QByteArray& sha256sums, const QString& fileName) {
  static const QRegularExpression line(QStringLiteral("^([0-9a-fA-F]{64}) [ *](.+)$"));
  for (const QByteArray& raw : sha256sums.split('\n')) {
    const auto m = line.match(QString::fromUtf8(raw).trimmed());
    if (!m.hasMatch()) continue;
    QString name = m.captured(2).trimmed();
    if (name.startsWith(QLatin1String("./"))) name.remove(0, 2);
    if (name == fileName) return m.captured(1).toLower();
  }
  return {};
}

QString sha256OfFile(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash h(QCryptographicHash::Sha256);
  if (!h.addData(&f)) return {};
  return QString::fromLatin1(h.result().toHex());
}

ChecksumResult verifyChecksum(const QString& filePath, const QByteArray& sha256sums, const QString& fileName) {
  ChecksumResult r;
  r.expected = checksumFor(sha256sums, fileName);
  if (r.expected.isEmpty()) {
    r.status = sha256sums.trimmed().isEmpty() ? ChecksumResult::Status::BadFile : ChecksumResult::Status::NotListed;
    return r;
  }
  r.actual = sha256OfFile(filePath);
  if (r.actual.isEmpty()) {
    r.status = ChecksumResult::Status::Unreadable;
    return r;
  }
  r.status = r.actual == r.expected ? ChecksumResult::Status::Ok : ChecksumResult::Status::Mismatch;
  return r;
}

QString ChecksumResult::message() const {
  switch (status) {
    case Status::Ok:
      return QStringLiteral("SHA-256 verified (%1).").arg(actual);
    case Status::Mismatch:
      return QStringLiteral("The downloaded package does not match its signed SHA-256 checksum, so it was deleted "
                            "and nothing was installed.\nExpected: %1\nGot:      %2")
          .arg(expected, actual);
    case Status::NotListed:
      return QStringLiteral("The signed SHA256SUMS does not list the package, so it can't be verified and nothing "
                            "was installed.");
    case Status::BadFile:
      return QStringLiteral("The release's SHA256SUMS is empty or unreadable, so nothing was installed.");
    case Status::Unreadable:
      break;
  }
  return QStringLiteral("The downloaded package could not be read, so nothing was installed.");
}

UpdateState UpdateState::load(const QString& path) {
  QSettings s(path, QSettings::IniFormat);
  UpdateState st;
  st.lastCheck = QDateTime::fromString(s.value(QStringLiteral("updates/lastCheck")).toString(), Qt::ISODate);
  st.skippedVersion = s.value(QStringLiteral("updates/skippedVersion")).toString();
  return st;
}

bool UpdateState::save(const QString& path) const {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QSettings s(path, QSettings::IniFormat);
  s.setValue(QStringLiteral("updates/lastCheck"),
             lastCheck.isValid() ? lastCheck.toUTC().toString(Qt::ISODate) : QString());
  s.setValue(QStringLiteral("updates/skippedVersion"), skippedVersion);
  s.sync();
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return s.status() == QSettings::NoError;
}

bool UpdatePolicy::shouldCheckAtStartup(bool enabled, const UpdateState& state, const QDateTime& now) {
  if (!enabled) return false;
  if (!state.lastCheck.isValid()) return true;
  const qint64 age = state.lastCheck.secsTo(now);
  return age < 0 || age >= kMinSecondsBetweenChecks;  // a clock set back also allows a check
}

bool UpdatePolicy::shouldOffer(const ReleaseInfo& release, const QString& currentVersion, const UpdateState& state,
                               bool manual) {
  if (release.draft || !isNewerVersion(release.tag, currentVersion)) return false;
  if (manual || state.skippedVersion.isEmpty()) return true;
  const SemVer skipped = SemVer::parse(state.skippedVersion);
  return skipped.valid ? SemVer::compare(release.version, skipped) > 0 : release.tag != state.skippedVersion;
}

}  // namespace zproject::app
