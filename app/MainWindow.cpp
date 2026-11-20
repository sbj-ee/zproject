#include "MainWindow.hpp"

#include <QLabel>

#include "core/Version.hpp"

namespace zproject::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(zproject::versionLine(QStringLiteral("zproject")));
  auto* label = new QLabel(
      tr("zproject %1\n\nThe desktop client arrives in phase 1b.").arg(zproject::appVersion()),
      this);
  label->setAlignment(Qt::AlignCenter);
  label->setAccessibleName(tr("zproject placeholder"));
  setCentralWidget(label);
  resize(960, 600);
}

}  // namespace zproject::app
