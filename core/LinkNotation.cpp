#include "core/LinkNotation.hpp"

#include <cstdlib>

#include "core/Model.hpp"

namespace zproject {

namespace {

constexpr int kMaxWbsDepth = 20;
constexpr int kMaxWbsPartDigits = 9;

bool isDigit(QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); }
bool isHex(QChar c) { return isDigit(c) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')); }

class Parser {
 public:
  explicit Parser(const QString& s) : s_(s) {}

  Result<QList<LinkSpec>> run() {
    QList<LinkSpec> out;
    skipSpace();
    if (atEnd()) return fail(QStringLiteral("no predecessor given"));
    while (true) {
      if (out.size() >= kMaxLinksInNotation) {
        return fail(QStringLiteral("at most %1 links at a time").arg(kMaxLinksInNotation));
      }
      auto item = parseItem();
      if (!item) return item.error();
      out.append(item.value());
      skipSpace();
      if (atEnd()) break;
      if (s_.at(i_) != QLatin1Char(',')) return fail(QStringLiteral("expected a comma or the end"));
      ++i_;
      skipSpace();
    }
    return out;
  }

 private:
  bool atEnd() const { return i_ >= s_.size(); }
  void skipSpace() {
    while (!atEnd() && s_.at(i_).isSpace()) ++i_;
  }
  Error fail(const QString& message) const {
    return Error::validation(QStringLiteral("%1 (at character %2)").arg(message).arg(i_ + 1),
                             QJsonObject{{QStringLiteral("field"), QStringLiteral("links")},
                                         {QStringLiteral("position"), static_cast<qint64>(i_)}});
  }

  Result<LinkSpec> parseItem() {
    LinkSpec spec;
    // A task id: 20 hex characters, not followed by another one unless what
    // follows is a link type written in lower case ("…3cffs").
    qsizetype j = i_;
    while (j < s_.size() && isHex(s_.at(j)) && j - i_ < 22) ++j;
    const QString after = s_.mid(i_ + 20, 2).toUpper();
    const bool typeNext = after == QLatin1String("FS") || after == QLatin1String("SS") ||
                          after == QLatin1String("FF") || after == QLatin1String("SF");
    if (j - i_ >= 20 && (j - i_ == 20 || typeNext)) {
      j = i_ + 20;
      spec.ref = s_.mid(i_, 20);
      spec.isId = true;
      i_ = j;
    } else {
      // A WBS code: digits ("." digits)*.
      const qsizetype start = i_;
      int depth = 0;
      while (true) {
        const qsizetype partStart = i_;
        while (!atEnd() && isDigit(s_.at(i_))) ++i_;
        const qsizetype len = i_ - partStart;
        if (len == 0) return fail(QStringLiteral("expected a WBS code (like 3 or 1.2) or a task id"));
        if (len > kMaxWbsPartDigits || s_.at(partStart) == QLatin1Char('0')) {
          i_ = partStart;  // point at the number, not past it
          return fail(len > kMaxWbsPartDigits ? QStringLiteral("WBS number too long")
                                              : QStringLiteral("WBS numbers start at 1"));
        }
        if (++depth > kMaxWbsDepth) return fail(QStringLiteral("WBS code too deep"));
        if (!atEnd() && s_.at(i_) == QLatin1Char('.')) {
          ++i_;
          continue;
        }
        break;
      }
      spec.ref = s_.mid(start, i_ - start);
    }
    skipSpace();
    // Type.
    if (i_ + 1 < s_.size()) {
      const QString two = s_.mid(i_, 2).toUpper();
      if (two == QLatin1String("FS") || two == QLatin1String("SS") || two == QLatin1String("FF")) {
        spec.type = two;
        i_ += 2;
        skipSpace();
      } else if (two == QLatin1String("SF")) {
        return fail(QStringLiteral("SF links are not supported yet"));
      }
    }
    // Lag.
    if (!atEnd() && (s_.at(i_) == QLatin1Char('+') || s_.at(i_) == QLatin1Char('-'))) {
      const bool negative = s_.at(i_) == QLatin1Char('-');
      ++i_;
      skipSpace();
      const qsizetype start = i_;
      while (!atEnd() && isDigit(s_.at(i_)) && i_ - start < 6) ++i_;
      if (i_ == start) return fail(QStringLiteral("expected a number of days after the sign"));
      if (!atEnd() && isDigit(s_.at(i_))) return fail(QStringLiteral("lag too long"));
      const int days = s_.mid(start, i_ - start).toInt();
      if (days > kMaxLagDays) return fail(QStringLiteral("lag must be at most %1 days").arg(kMaxLagDays));
      spec.lag = negative ? -days : days;
      skipSpace();
      if (!atEnd() && (s_.at(i_) == QLatin1Char('d') || s_.at(i_) == QLatin1Char('D'))) ++i_;
    }
    return spec;
  }

  const QString& s_;
  qsizetype i_ = 0;
};

}  // namespace

Result<QList<LinkSpec>> parseLinkNotation(const QString& text) {
  if (text.size() > kMaxLinkNotationLength) {
    return Error::validation(QStringLiteral("at most %1 characters").arg(kMaxLinkNotationLength),
                             QJsonObject{{QStringLiteral("field"), QStringLiteral("links")}});
  }
  return Parser(text).run();
}

QString formatLink(const QString& ref, const QString& type, int lag) {
  if (type == QLatin1String("FS") && lag == 0) return ref;
  QString out = ref + type;
  if (lag != 0) out += QStringLiteral("%1%2d").arg(lag > 0 ? QStringLiteral("+") : QStringLiteral("-")).arg(std::abs(lag));
  return out;
}

}  // namespace zproject
