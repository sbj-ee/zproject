#include "app/Shortcuts.hpp"

#include <QObject>

namespace zproject::app {

QList<ShortcutInfo> shellShortcuts() {
  return {
      {QStringLiteral("actionNewProject"), QObject::tr("New project"), {QKeySequence(Qt::CTRL | Qt::Key_N)}},
      {QStringLiteral("actionEditProject"), QObject::tr("Edit the project's details"),
       {QKeySequence(Qt::CTRL | Qt::Key_E)}},
      {QStringLiteral("actionRefresh"), QObject::tr("Refresh from the server"),
       {QKeySequence(Qt::Key_F5), QKeySequence(Qt::CTRL | Qt::Key_R)}},
      {QStringLiteral("actionPreferences"), QObject::tr("Preferences"),
       {QKeySequence(Qt::CTRL | Qt::Key_Comma)}},
      {QStringLiteral("actionQuit"), QObject::tr("Quit"), {QKeySequence(Qt::CTRL | Qt::Key_Q)}},
      {QStringLiteral("actionZoomIn"), QObject::tr("Zoom in (UI text)"),
       {QKeySequence(Qt::CTRL | Qt::Key_Equal), QKeySequence(Qt::CTRL | Qt::Key_Plus)}},
      {QStringLiteral("actionZoomOut"), QObject::tr("Zoom out (UI text)"), {QKeySequence(Qt::CTRL | Qt::Key_Minus)}},
      {QStringLiteral("actionZoomReset"), QObject::tr("Actual size (UI text)"), {QKeySequence(Qt::CTRL | Qt::Key_0)}},
      {QStringLiteral("actionToggleSidebar"), QObject::tr("Show or hide the sidebar"),
       {QKeySequence(Qt::CTRL | Qt::Key_B)}},
      {QStringLiteral("actionToggleInspector"), QObject::tr("Show or hide the inspector"),
       {QKeySequence(Qt::CTRL | Qt::Key_I)}},
      {QStringLiteral("actionFocusSidebar"), QObject::tr("Move focus to the project list"),
       {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P)}},
      {QStringLiteral("actionHelp"), QObject::tr("zproject Help"), {QKeySequence(Qt::Key_F1)}},
      {QStringLiteral("actionShortcuts"), QObject::tr("Keyboard shortcuts"),
       {QKeySequence(Qt::CTRL | Qt::Key_Slash)}},
  };
}

QKeySequence shortcutFor(const QString& action) {
  for (const auto& s : shellShortcuts()) {
    if (s.action == action && !s.keys.isEmpty()) return s.keys.first();
  }
  return {};
}

}  // namespace zproject::app
