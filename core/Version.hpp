#pragma once

#include <QString>

namespace zproject {

// The application version (project() VERSION in CMakeLists.txt), shared by
// zprojectd, zproject and zproject-cli (SPEC.md §22).
QString appVersion();

// The HTTP API major version served under /v1 (SPEC.md §9.3). Changes only
// additively within a major app version line.
constexpr int kApiVersion = 1;

// "<program> <version>", the first line of every --version output.
QString versionLine(const QString& program);

}  // namespace zproject
