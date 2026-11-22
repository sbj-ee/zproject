#include "app/SingleInstance.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>

#include <sys/stat.h>

namespace zproject::app {

SingleInstance::SingleInstance(const QString& lockPath) : path_(lockPath), lock_(std::make_unique<QLockFile>(lockPath)) {
  lock_->setStaleLockTime(0);  // only a dead owner makes it stale
}

SingleInstance::~SingleInstance() = default;

bool SingleInstance::acquire(qint64* holderPid) {
  const QString dir = QFileInfo(path_).absolutePath();
  if (!QDir().mkpath(dir)) return false;
  ::chmod(QFile::encodeName(dir).constData(), 0700);
  if (lock_->tryLock(0)) return true;
  if (holderPid) {
    QString host;
    QString app;
    *holderPid = 0;
    lock_->getLockInfo(holderPid, &host, &app);
  }
  return false;
}

bool SingleInstance::held() const { return lock_->isLocked(); }

}  // namespace zproject::app
