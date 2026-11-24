#include "core/Zip.hpp"

#include <QTimeZone>
#include <QtEndian>

#include <array>

namespace zproject {
namespace {

const std::array<quint32, 256>& crcTable() {
  static const std::array<quint32, 256> table = [] {
    std::array<quint32, 256> t{};
    for (quint32 i = 0; i < 256; ++i) {
      quint32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  return table;
}

void put16(QByteArray& b, quint16 v) {
  char buf[2];
  qToLittleEndian(v, buf);
  b.append(buf, 2);
}

void put32(QByteArray& b, quint32 v) {
  char buf[4];
  qToLittleEndian(v, buf);
  b.append(buf, 4);
}

// Raw deflate (RFC 1951), as zip method 8 wants it. qCompress() returns a
// 4-byte length followed by a zlib stream: a 2-byte header, the raw
// deflate data and a 4-byte Adler-32. Strip the wrapper.
QByteArray rawDeflate(const QByteArray& data) {
  const QByteArray z = qCompress(data, 9);
  if (z.size() < 10) return {};
  return z.mid(6, z.size() - 10);
}

}  // namespace

quint32 crc32(const QByteArray& data) {
  const auto& t = crcTable();
  quint32 c = 0xFFFFFFFFU;
  for (const char ch : data) c = t[(c ^ static_cast<quint8>(ch)) & 0xFFU] ^ (c >> 8);
  return c ^ 0xFFFFFFFFU;
}

ZipWriter::ZipWriter(const QDateTime& modified) {
  QDateTime t = modified.isValid() ? modified.toUTC() : QDateTime(QDate(1980, 1, 1), QTime(0, 0), QTimeZone::utc());
  if (t.date().year() < 1980) t = QDateTime(QDate(1980, 1, 1), QTime(0, 0), QTimeZone::utc());
  if (t.date().year() > 2107) t = QDateTime(QDate(2107, 12, 31), QTime(23, 59, 58), QTimeZone::utc());
  const QDate d = t.date();
  const QTime tm = t.time();
  dosDate_ = static_cast<quint16>(((d.year() - 1980) << 9) | (d.month() << 5) | d.day());
  dosTime_ = static_cast<quint16>((tm.hour() << 11) | (tm.minute() << 5) | (tm.second() / 2));
}

void ZipWriter::add(const QString& name, const QByteArray& data, bool compress) {
  if (finished_) return;
  Entry e;
  e.name = name.toLatin1();
  e.crc = crc32(data);
  e.size = static_cast<quint32>(data.size());
  QByteArray body = data;
  if (compress && !data.isEmpty()) {
    const QByteArray deflated = rawDeflate(data);
    if (!deflated.isEmpty() && deflated.size() < data.size()) {
      body = deflated;
      e.method = 8;
    }
  }
  e.compressedSize = static_cast<quint32>(body.size());
  e.offset = static_cast<quint32>(out_.size());

  put32(out_, 0x04034b50U);  // local file header
  put16(out_, 20);           // version needed: 2.0 (deflate)
  put16(out_, 0);            // flags
  put16(out_, e.method);
  put16(out_, dosTime_);
  put16(out_, dosDate_);
  put32(out_, e.crc);
  put32(out_, e.compressedSize);
  put32(out_, e.size);
  put16(out_, static_cast<quint16>(e.name.size()));
  put16(out_, 0);  // extra field length
  out_.append(e.name);
  out_.append(body);
  entries_.append(e);
}

QByteArray ZipWriter::finish() {
  if (finished_) return out_;
  finished_ = true;
  const auto cdOffset = static_cast<quint32>(out_.size());
  for (const Entry& e : entries_) {
    put32(out_, 0x02014b50U);  // central directory header
    put16(out_, 20);           // version made by: 2.0, MS-DOS attributes
    put16(out_, 20);           // version needed
    put16(out_, 0);
    put16(out_, e.method);
    put16(out_, dosTime_);
    put16(out_, dosDate_);
    put32(out_, e.crc);
    put32(out_, e.compressedSize);
    put32(out_, e.size);
    put16(out_, static_cast<quint16>(e.name.size()));
    put16(out_, 0);  // extra
    put16(out_, 0);  // comment
    put16(out_, 0);  // disk number
    put16(out_, 0);  // internal attributes
    put32(out_, 0);  // external attributes
    put32(out_, e.offset);
    out_.append(e.name);
  }
  const auto cdSize = static_cast<quint32>(out_.size()) - cdOffset;
  put32(out_, 0x06054b50U);  // end of central directory
  put16(out_, 0);
  put16(out_, 0);
  put16(out_, static_cast<quint16>(entries_.size()));
  put16(out_, static_cast<quint16>(entries_.size()));
  put32(out_, cdSize);
  put32(out_, cdOffset);
  put16(out_, 0);  // comment length
  return out_;
}

}  // namespace zproject
