#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

namespace zproject::app {

// The shell's keyboard shortcuts (SPEC.md §8.4), one list for the menus and
// Help → Keyboard Shortcuts. Ctrl is Cmd on macOS (Qt maps it).
struct ShortcutInfo {
  QString action;  // the QAction's objectName
  QString label;
  QList<QKeySequence> keys;
};
QList<ShortcutInfo> shellShortcuts();
QKeySequence shortcutFor(const QString& action);

}  // namespace zproject::app
