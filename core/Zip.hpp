#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

// A minimal ZIP writer for OpenDocument packages (the .ods export, SPEC.md
// §7.9). Qt has no public zip API, and the package needs only what is
// here: a few entries, stored or deflated, no zip64, no encryption, no
// streaming. Pure Qt Core; the output is deterministic for the same input.
namespace zproject {

// CRC-32 (IEEE 802.3, as zip and gzip use it).
quint32 crc32(const QByteArray& data);

class ZipWriter {
 public:
  // Entry timestamps: DOS time with two-second resolution, taken in UTC so
  // the bytes don't depend on the host's time zone; clamped to 1980–2107.
  explicit ZipWriter(const QDateTime& modified = {});

  // Adds one entry. `name` is ASCII with '/' separators. With `compress`
  // the data is deflated when that makes it smaller; otherwise it is
  // stored (ODF requires the first entry, "mimetype", to be stored).
  void add(const QString& name, const QByteArray& data, bool compress = true);

  // The whole archive. The writer can't be added to afterwards.
  QByteArray finish();

 private:
  struct Entry {
    QByteArray name;
    quint32 crc = 0;
    quint32 size = 0;
    quint32 compressedSize = 0;
    quint16 method = 0;
    quint32 offset = 0;
  };
  QByteArray out_;
  QList<Entry> entries_;
  quint16 dosTime_ = 0;
  quint16 dosDate_ = 0;
  bool finished_ = false;
};

}  // namespace zproject
