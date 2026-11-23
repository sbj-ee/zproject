#pragma once

#include <QList>
#include <QString>

#include "core/Error.hpp"

// The predecessor notation (SPEC.md §10; scheduling plan S3), as MS Project
// writes it: `3FS+2d` is "after task 3 finishes, plus two working days".
//
//   list  := item ("," item)*
//   item  := ref [type] [lag]
//   ref   := WBS code ("3", "1.2.4") | task id (20 lower-case hex)
//   type  := FS | SS | FF            (any case; default FS)
//   lag   := ("+" | "-") digits ["d"] (whole working days; default 0)
//
// Spaces around the parts are ignored: "1.2 SS + 1d, 4". Untrusted input
// (CLI arguments, later the desktop's link editor): fuzzed (fuzz/), with
// limits on length, items and lag.
namespace zproject {

struct LinkSpec {
  QString ref;         // a WBS code or a task id
  bool isId = false;   // ref is a task id
  QString type = QStringLiteral("FS");
  int lag = 0;
  bool operator==(const LinkSpec&) const = default;
};

inline constexpr int kMaxLinkNotationLength = 2000;
inline constexpr int kMaxLinksInNotation = 100;

// Errors are 400 validation with details {position (0-based), field: "links"}.
Result<QList<LinkSpec>> parseLinkNotation(const QString& text);

// The canonical form: "3", "3SS", "3FS+2d", "1.2FF-1d" (FS with no lag is
// just the ref).
QString formatLink(const QString& ref, const QString& type, int lag);

}  // namespace zproject
