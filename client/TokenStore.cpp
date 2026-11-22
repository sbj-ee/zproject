#include "client/TokenStore.hpp"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace zproject::client {

namespace {

Error err(const char* code, const QString& message) { return Error{QString::fromLatin1(code), message, {}}; }

QString securityTool() { return QStringLiteral("/usr/bin/security"); }
QString secretTool() { return QStringLiteral("secret-tool"); }

QStringList attributes() {
  return {QStringLiteral("service"), QString::fromLatin1(TokenStore::kService), QStringLiteral("account"),
          QString::fromLatin1(TokenStore::kAccount)};
}

}  // namespace

CommandResult runCommand(const QString& program, const QStringList& args, const QByteArray& input) {
  QProcess p;
  p.setProgram(program);
  p.setArguments(args);
  p.start();
  CommandResult r;
  if (!p.waitForStarted(5000)) return r;
  if (!input.isEmpty()) p.write(input);
  p.closeWriteChannel();
  if (!p.waitForFinished(15000)) {
    p.kill();
    p.waitForFinished(1000);
    return r;
  }
  r.out = p.readAllStandardOutput();
  r.err = p.readAllStandardError();
  r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
  return r;
}

bool looksLikeToken(const QString& token) {
  static const QRegularExpression re(QStringLiteral("^zpt_[0-9a-f]{20}\\.[A-Za-z0-9_-]{43}$"));
  return re.match(token).hasMatch();
}

QString tokenLabel(const QString& token) {
  const qsizetype dot = token.indexOf(QLatin1Char('.'));
  return dot > 0 ? token.left(dot) : QStringLiteral("zpt_?");
}

TokenStore::TokenStore(QString tokenFile, CommandRunner runner, bool macos)
    : tokenFile_(std::move(tokenFile)), run_(std::move(runner)), macos_(macos) {}

Result<QString> TokenStore::readFile() const {
  const QByteArray path = QFile::encodeName(tokenFile_);
  struct stat st {};
  if (::lstat(path.constData(), &st) != 0) {
    if (errno == ENOENT) return err("not_found", QStringLiteral("no token file at %1").arg(tokenFile_));
    return err("io", QStringLiteral("cannot read %1: %2").arg(tokenFile_, QString::fromLocal8Bit(std::strerror(errno))));
  }
  if (!S_ISREG(st.st_mode)) return err("insecure", QStringLiteral("%1 is not a regular file").arg(tokenFile_));
  if (st.st_uid != ::getuid()) return err("insecure", QStringLiteral("%1 is not owned by you").arg(tokenFile_));
  if ((st.st_mode & 077) != 0) {
    return err("insecure", QStringLiteral("%1 is readable by others (mode %2); run: chmod 600 %1")
                               .arg(tokenFile_, QString::number(st.st_mode & 0777, 8)));
  }
  QFile f(tokenFile_);
  if (!f.open(QIODevice::ReadOnly)) return err("io", QStringLiteral("cannot read %1").arg(tokenFile_));
  const QString token = QString::fromLatin1(f.read(4096)).trimmed();
  if (!looksLikeToken(token)) return err("invalid", QStringLiteral("%1 does not hold a zproject token").arg(tokenFile_));
  return token;
}

Status TokenStore::writeFile(const QString& token) const {
  if (!looksLikeToken(token)) return err("invalid", QStringLiteral("not a zproject token"));
  const QFileInfo info(tokenFile_);
  const QString dir = info.absolutePath();
  if (!QDir().mkpath(dir)) return err("io", QStringLiteral("cannot create %1").arg(dir));
  // The directory is ours alone (it may also hold cli.ini).
  ::chmod(QFile::encodeName(dir).constData(), 0700);
  const QByteArray tmp = QFile::encodeName(tokenFile_ + QStringLiteral(".tmp"));
  ::unlink(tmp.constData());
  const int fd = ::open(tmp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return err("io", QStringLiteral("cannot write %1").arg(tokenFile_));
  const QByteArray data = token.toLatin1() + '\n';
  bool ok = ::fchmod(fd, 0600) == 0 && ::write(fd, data.constData(), static_cast<size_t>(data.size())) == data.size() &&
            ::fsync(fd) == 0;
  ok = ::close(fd) == 0 && ok;
  if (!ok || ::rename(tmp.constData(), QFile::encodeName(tokenFile_).constData()) != 0) {
    ::unlink(tmp.constData());
    return err("io", QStringLiteral("cannot write %1").arg(tokenFile_));
  }
  return Ok{};
}

Status TokenStore::removeFile() const {
  if (::unlink(QFile::encodeName(tokenFile_).constData()) != 0 && errno != ENOENT) {
    return err("io", QStringLiteral("cannot remove %1").arg(tokenFile_));
  }
  return Ok{};
}

Result<QString> TokenStore::readKeychain() const {
  CommandResult r;
  if (macos_) {
    r = run_(securityTool(),
             {QStringLiteral("find-generic-password"), QStringLiteral("-s"), QString::fromLatin1(kService),
              QStringLiteral("-a"), QString::fromLatin1(kAccount), QStringLiteral("-w")},
             {});
    if (r.exitCode == 44) return err("not_found", QStringLiteral("no zproject token in the Keychain"));
  } else {
    r = run_(secretTool(), QStringList{QStringLiteral("lookup")} + attributes(), {});
    if (r.exitCode == 1 && r.out.isEmpty() && r.err.isEmpty()) {
      return err("not_found", QStringLiteral("no zproject token in the Secret Service keyring"));
    }
  }
  if (r.exitCode == -1) {
    return err("unavailable", macos_ ? QStringLiteral("cannot run /usr/bin/security")
                                     : QStringLiteral("secret-tool is not available (sudo apt install libsecret-tools)"));
  }
  if (r.exitCode != 0) {
    return err("unavailable", QStringLiteral("keychain lookup failed: %1").arg(QString::fromLocal8Bit(r.err).trimmed()));
  }
  const QString token = QString::fromLatin1(r.out).trimmed();
  if (!looksLikeToken(token)) return err("invalid", QStringLiteral("the keychain entry is not a zproject token"));
  return token;
}

Status TokenStore::writeKeychain(const QString& token) const {
  if (!looksLikeToken(token)) return err("invalid", QStringLiteral("not a zproject token"));
  CommandResult r;
  if (macos_) {
    // `security -i` reads the command from stdin, so the token never appears
    // in a process listing. The token's alphabet needs no quoting.
    const QByteArray cmd = "add-generic-password -U -s " + QByteArray(kService) + " -a " + QByteArray(kAccount) +
                           " -l zproject -w " + token.toLatin1() + '\n';
    r = run_(securityTool(), {QStringLiteral("-i")}, cmd);
  } else {
    r = run_(secretTool(), QStringList{QStringLiteral("store"), QStringLiteral("--label=zproject")} + attributes(),
             token.toLatin1());
  }
  if (r.exitCode == -1) {
    return err("unavailable", macos_ ? QStringLiteral("cannot run /usr/bin/security")
                                     : QStringLiteral("secret-tool is not available (sudo apt install libsecret-tools)"));
  }
  if (r.exitCode != 0) {
    return err("unavailable", QStringLiteral("keychain store failed: %1").arg(QString::fromLocal8Bit(r.err).trimmed()));
  }
  return Ok{};
}

Status TokenStore::removeKeychain() const {
  CommandResult r;
  if (macos_) {
    r = run_(securityTool(),
             {QStringLiteral("delete-generic-password"), QStringLiteral("-s"), QString::fromLatin1(kService),
              QStringLiteral("-a"), QString::fromLatin1(kAccount)},
             {});
    if (r.exitCode == 44) return Ok{};  // nothing stored
  } else {
    r = run_(secretTool(), QStringList{QStringLiteral("clear")} + attributes(), {});
  }
  if (r.exitCode == -1) return err("unavailable", QStringLiteral("keychain tool not available"));
  if (r.exitCode != 0 && !macos_) return Ok{};  // secret-tool clear: nothing to clear
  if (r.exitCode != 0) return err("unavailable", QStringLiteral("keychain delete failed"));
  return Ok{};
}

Result<ResolvedToken> resolveDeviceToken(const QHash<QString, QString>& env, const QString& configuredStore,
                                         const TokenStore& store) {
  auto authError = [](const QString& message) { return Error{QStringLiteral("auth"), message, {}}; };
  const QString envToken = env.value(QStringLiteral("ZPROJECT_TOKEN")).trimmed();
  if (!envToken.isEmpty()) {
    if (!looksLikeToken(envToken)) return authError(QStringLiteral("ZPROJECT_TOKEN is not a zproject token"));
    return ResolvedToken{envToken, QStringLiteral("ZPROJECT_TOKEN")};
  }
  const bool keychainFirst = configuredStore == QLatin1String("keychain");
  if (keychainFirst) {
    auto t = store.readKeychain();
    if (!t) return authError(t.error().message);
    return ResolvedToken{t.value(), QStringLiteral("keychain")};
  }
  auto f = store.readFile();
  if (f) return ResolvedToken{f.value(), QStringLiteral("file ") + store.tokenFile()};
  if (f.error().code != QLatin1String("not_found")) return authError(f.error().message);
  if (store.macos() && configuredStore.isEmpty()) {
    auto t = store.readKeychain();
    if (t) return ResolvedToken{t.value(), QStringLiteral("keychain")};
    if (t.error().code != QLatin1String("not_found")) return authError(t.error().message);
  }
  return authError(QStringLiteral("no token: run `zproject-cli login` (or set ZPROJECT_TOKEN)"));
}

}  // namespace zproject::client
