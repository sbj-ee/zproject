// zproject: the desktop client (SPEC.md §8).
#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QStyleFactory>
#include <QTextStream>
#include <QTimer>

#include <cstring>

#include "app/MainWindow.hpp"
#include "app/SignInDialog.hpp"
#include "app/SingleInstance.hpp"
#include "core/Version.hpp"

int main(int argc, char* argv[]) {
  // --version works without a display (CI, packaging smoke tests).
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0) {
      QTextStream(stdout) << zproject::versionLine(QStringLiteral("zproject")) << '\n';
      return 0;
    }
  }

  Q_INIT_RESOURCE(zproject_app);
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("zproject"));
  QApplication::setApplicationVersion(zproject::appVersion());
  QApplication::setOrganizationDomain(QStringLiteral("sbj.ee"));
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/zproject-256.png")));

  // The platform palette decides "System" on Qt 6.4; read it before Fusion
  // and our palette replace it.
  zproject::app::MainWindow::Options options = zproject::app::MainWindow::defaultOptions();
  QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

  zproject::app::SingleInstance instance(options.paths.cacheDir + QStringLiteral("/zproject.lock"));
  qint64 pid = 0;
  if (!instance.acquire(&pid)) {
    QMessageBox::information(nullptr, QStringLiteral("zproject"),
                             QObject::tr("zproject is already running (process %1).").arg(pid));
    return 1;
  }

  zproject::app::MainWindow window(std::move(options));
  window.show();
  if (window.needsSignIn()) {
    QTimer::singleShot(0, &window, [&window] { window.showSignIn(); });
  }
  return QApplication::exec();
}
