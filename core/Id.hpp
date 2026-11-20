#pragma once

#include <QString>

namespace zproject {

// Entity ids: 20 lower-case hex characters taken from a random UUID, the
// shared z-suite id format (SPEC.md §5).
constexpr int kIdLength = 20;

QString newId();
bool isValidId(const QString& id);

}  // namespace zproject
