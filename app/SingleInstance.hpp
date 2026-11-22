#pragma once

#include <QString>

#include <memory>

class QLockFile;

namespace zproject::app {

// One zproject window per user (SPEC.md §8.1): a QLockFile in the cache
// directory (0700). A stale lock from a crashed run is taken over.
class SingleInstance {
 public:
  explicit SingleInstance(const QString& lockPath);
  ~SingleInstance();
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  // False if another live process holds it; *holderPid is then its pid.
  bool acquire(qint64* holderPid = nullptr);
  bool held() const;
  // Lets a restarted zproject (after an update) take the lock.
  void release();
  QString path() const { return path_; }

 private:
  QString path_;
  std::unique_ptr<QLockFile> lock_;
};

}  // namespace zproject::app
