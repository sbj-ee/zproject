#include "app/SmokeTest.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QMenuBar>
#include <QStackedWidget>
#include <QStyle>
#include <QTemporaryDir>
#include <QWindow>

#include "app/MainWindow.hpp"
#include "core/Version.hpp"

namespace zproject::app {

int runSmokeTest(QString* report) {
  QStringList lines;
  int failures = 0;
  auto check = [&](bool ok, const QString& what) {
    lines << QStringLiteral("%1 %2").arg(ok ? QStringLiteral("ok  ") : QStringLiteral("FAIL"), what);
    if (!ok) ++failures;
  };

  QTemporaryDir home;
  check(home.isValid(), QStringLiteral("temporary home"));
  {
    MainWindow::Options o = MainWindow::defaultOptions();
    // Only these: no ZPROJECT_URL, ZPROJECT_TOKEN or the user's HOME.
    o.env = {{QStringLiteral("HOME"), home.path()},
             {QStringLiteral("ZPROJECT_CONFIG_DIR"), home.filePath(QStringLiteral("config"))},
             {QStringLiteral("ZPROJECT_CACHE_DIR"), home.filePath(QStringLiteral("cache"))}};
    o.paths = client::desktopPaths(o.env, o.macos);
    o.runner = [](const QString&, const QStringList&, const QByteArray&) { return client::CommandResult{-1, {}, {}}; };
    o.autoConnect = false;
    o.startupUpdateCheck = false;

    const QString platform = QGuiApplication::platformName();
    check(!platform.isEmpty(), QStringLiteral("platform plugin: %1").arg(platform));
    const QString style = QApplication::style() ? QApplication::style()->name() : QString();
    check(style.compare(QLatin1String("fusion"), Qt::CaseInsensitive) == 0, QStringLiteral("style: %1").arg(style));
    check(QFile::exists(QStringLiteral(":/help/zproject-help.md")), QStringLiteral("help guide bundled"));
    check(!QIcon(QStringLiteral(":/icons/zproject-256.png")).isNull(), QStringLiteral("app icon bundled"));

    MainWindow w(std::move(o));
    w.show();
    QElapsedTimer t;
    t.start();
    while (!(w.windowHandle() && w.windowHandle()->isExposed()) && t.elapsed() < 5000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    const bool exposed = w.windowHandle() && w.windowHandle()->isExposed();
    check(exposed, QStringLiteral("main window shown"));
    const QString title = w.windowTitle().remove(QStringLiteral("[*]"));
    check(title.startsWith(QStringLiteral("zproject %1").arg(appVersion())), QStringLiteral("title: %1").arg(title));
    check(w.menuBar()->actions().size() >= 5, QStringLiteral("menus: %1").arg(w.menuBar()->actions().size()));
    // Every view is in the window (they fill once a project is selected).
    const QStringList views = {QStringLiteral("projectTable"), QStringLiteral("milestonesView"),
                               QStringLiteral("activityView")};
    for (const QString& name : views) {
      check(w.mainStack()->findChild<QWidget*>(name) != nullptr, QStringLiteral("view: %1").arg(name));
    }
    // An empty config: no server, so sign-in is needed and nothing connects.
    check(!w.connectToServer() && w.needsSignIn(), QStringLiteral("signed out in the throwaway config"));
    w.close();
  }
  lines << (failures == 0 ? QStringLiteral("zproject %1 smoke test: ok").arg(appVersion())
                          : QStringLiteral("zproject %1 smoke test: %2 failed").arg(appVersion()).arg(failures));
  if (report) *report = lines.join(QLatin1Char('\n'));
  return failures == 0 ? 0 : 1;
}

}  // namespace zproject::app
