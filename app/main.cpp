// zproject: the desktop client (SPEC.md §8). Phase 0 skeleton.
#include <QApplication>
#include <QIcon>
#include <QTextStream>

#include <cstring>

#include "app/MainWindow.hpp"
#include "core/Version.hpp"

int main(int argc, char* argv[]) {
  // --version works without a display (CI, packaging smoke tests).
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0) {
      QTextStream(stdout) << zproject::versionLine(QStringLiteral("zproject")) << '\n';
      return 0;
    }
  }

  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("zproject"));
  QApplication::setApplicationVersion(zproject::appVersion());
  QApplication::setOrganizationDomain(QStringLiteral("sbj.ee"));
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/zproject-256.png")));

  zproject::app::MainWindow window;
  window.show();
  return QApplication::exec();
}
