#pragma once

#include <QString>

namespace zproject::app {

// `zproject --smoke-test` (packaging checks in CI, SPEC.md §19, §20): builds
// the real main window in a throwaway home, config and cache (so it reads
// and writes nothing of the user's, and never connects or checks for
// updates), shows it, and checks what an install can get wrong: a platform
// plugin, the Fusion style, the bundled help and icon, the menus and the
// views. Needs a QApplication. Returns 0 when all is well; `report` gets one
// line per check and a summary.
int runSmokeTest(QString* report);

}  // namespace zproject::app
