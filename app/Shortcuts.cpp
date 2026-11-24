#include "app/Shortcuts.hpp"

#include <QObject>

namespace zproject::app {

QList<ShortcutInfo> shellShortcuts() {
  return {
      {QStringLiteral("actionNewProject"), QObject::tr("New project"), {QKeySequence(Qt::CTRL | Qt::Key_N)}},
      {QStringLiteral("actionEditProject"), QObject::tr("Edit the project's details"),
       {QKeySequence(Qt::CTRL | Qt::Key_E)}},
      {QStringLiteral("actionExport"), QObject::tr("Export the project to a file"),
       {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E)}},
      {QStringLiteral("actionPrint"), QObject::tr("Print the project"), {QKeySequence(Qt::CTRL | Qt::Key_P)}},
      // The outline (SPEC.md §8.4); these keys act only while the task tree
      // has the focus.
      {QStringLiteral("actionInsertTask"), QObject::tr("Insert a task below (outline)"),
       {QKeySequence(Qt::Key_Insert), QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)}},
      {QStringLiteral("actionIndentTask"), QObject::tr("Indent the task (outline)"), {QKeySequence(Qt::Key_Tab)}},
      {QStringLiteral("actionOutdentTask"), QObject::tr("Outdent the task (outline)"),
       {QKeySequence(Qt::SHIFT | Qt::Key_Backtab), QKeySequence(Qt::Key_Backtab)}},
      {QStringLiteral("actionMoveTaskUp"), QObject::tr("Move the task up (outline)"),
       {QKeySequence(Qt::ALT | Qt::Key_Up)}},
      {QStringLiteral("actionMoveTaskDown"), QObject::tr("Move the task down (outline)"),
       {QKeySequence(Qt::ALT | Qt::Key_Down)}},
      {QStringLiteral("actionDeleteTask"), QObject::tr("Delete the task (outline; asks first)"),
       {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)}},
      {QStringLiteral("actionRenameTask"), QObject::tr("Edit the task's title (outline)"), {QKeySequence(Qt::Key_F2)}},
      {QStringLiteral("actionEditTask"), QObject::tr("Edit the task's details in the inspector (outline)"),
       {QKeySequence(Qt::ALT | Qt::Key_Return), QKeySequence(Qt::ALT | Qt::Key_Enter)}},
      {QStringLiteral("actionSaveTask"), QObject::tr("Save the task editor's changes"),
       {QKeySequence(Qt::CTRL | Qt::Key_S)}},
      {QStringLiteral("actionFind"), QObject::tr("Find a task"), {QKeySequence(Qt::CTRL | Qt::Key_F)}},
      {QStringLiteral("actionGantt"), QObject::tr("Show the Gantt"), {QKeySequence(Qt::CTRL | Qt::Key_G)}},
      {QStringLiteral("actionMilestones"), QObject::tr("Show the milestones"),
       {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M)}},
      {QStringLiteral("actionActivity"), QObject::tr("Show the project's activity"),
       {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A)}},
      {QStringLiteral("actionFindNext"), QObject::tr("Find next"), {QKeySequence(Qt::Key_F3)}},
      {QStringLiteral("actionFindPrevious"), QObject::tr("Find previous"), {QKeySequence(Qt::SHIFT | Qt::Key_F3)}},
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
