#include "MainWindow.hpp"

#include <QCoreApplication>
#include <QLabel>

#include "core/Version.hpp"

namespace zproject::app {

namespace {
QString cliPath() {
#ifdef Q_OS_MACOS
  return QCoreApplication::applicationDirPath() + QStringLiteral("/zproject-cli");
#else
  return QStringLiteral("/usr/bin/zproject-cli");
#endif
}
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(zproject::versionLine(QStringLiteral("zproject")));
  auto* label = new QLabel(
      tr("zproject %1\n\nThe desktop client is not built yet. Use zproject-cli:\n%2\n\n"
         "Run `zproject-cli login --url https://<server>` first, then `zproject-cli --help`.")
          .arg(zproject::appVersion(), cliPath()),
      this);
  label->setAlignment(Qt::AlignCenter);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label->setAccessibleName(tr("zproject placeholder"));
  setCentralWidget(label);
  resize(960, 600);
}

}  // namespace zproject::app
