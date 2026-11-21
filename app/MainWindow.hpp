#pragma once

#include <QMainWindow>

namespace zproject::app {

// Placeholder main window for the phase 0 skeleton. The sidebar, Gantt, List
// and inspector (SPEC.md §8) arrive in phase 1b.
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
};

}  // namespace zproject::app
