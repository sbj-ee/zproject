#pragma once

#include <QIcon>
#include <QString>

#include "app/Theme.hpp"

namespace zproject::app::Icons {

// The app's own icon set, drawn with QPainter in the theme's colours so it
// looks the same on every platform and never depends on the system icon
// theme (which can be empty on Linux desktops) or on the Qt SVG module.
// Each icon carries 16 px and 32 px pixmaps (HiDPI).

enum class Glyph {
  NewProject,
  Refresh,
  EditProject,
  DeleteProject,
  AllProjects,
  ProjectDetails,
  Outline,
  InsertTask,
  TaskDetails,
  SaveTask,
  DeleteTask,
  Find,
  CheckUpdates,
  Gantt,
  Milestones,
};

QIcon glyph(Glyph g, const ThemeColors& c);

// A project row in the sidebar: a folder in the theme's text colour (muted
// unless the project is active) with a RAG badge (green, amber or red; none
// when the RAG is unset).
QIcon project(const ThemeColors& c, const QString& rag, const QString& status);

}  // namespace zproject::app::Icons
